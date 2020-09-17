
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <ccan/list/list.h>
#include <ccan/minmax/minmax.h>
#include <ccan/likely/likely.h>
#include <ccan/bitops/bitops.h>

#include "pht.h"


#define TOMBSTONE (1)
/* this value is convenient because 1ull << 64 is 0, removing a conditional
 * branch or cmov from all spots that expand the perfect mask.
 */
#define NO_PERFECT_BIT (sizeof(uintptr_t) * CHAR_BIT)


struct _pht_table
{
	struct list_node link;	/* in pht.tables */
	size_t elems, deleted;
	/* next entry to migrate. 0 for not started, 1<<bits for completed.
	 * entries at indexes less than nextmig have been migrated and should be
	 * ignored by iteration and deletion.
	 */
	size_t nextmig;
	/* start of the first hash chain beginning in the migration zone, i.e. a
	 * non-empty slot following an empty.
	 */
	size_t chain_start;
	uintptr_t common_bits, common_mask;
	short bits;	/* size_log2 */
	short perfect_bit;
	uintptr_t table[]
		__attribute__((aligned(64)));
};


void pht_init(struct pht *ht,
	size_t (*rehash)(const void *elem, void *priv), void *priv)
{
	*ht = (struct pht)PHT_INITIALIZER(*ht, rehash, priv);
}


size_t pht_count(const struct pht *ht) {
	return ht->elems;
}


void pht_clear(struct pht *ht)
{
	struct _pht_table *cur, *next;
	list_for_each_safe(&ht->tables, cur, next, link) {
		list_del_from(&ht->tables, &cur->link);
		free(cur);
	}
	assert(list_empty(&ht->tables));
}


struct pht *pht_check(const struct pht *ht, const char *abortstr)
{
#ifndef NDEBUG
	ssize_t total = ht->elems;
	const struct _pht_table *t;
	list_for_each(&ht->tables, t, link) {
		total -= t->elems;
		assert(t->deleted <= (size_t)1 << t->bits);
	}
	assert(total == 0);

	/* TODO: add more, use @abortstr somehow */
#endif

	return (struct pht *)ht;
}


static bool is_valid(uintptr_t e) {
	return e != 0 && e != TOMBSTONE;
}


static inline void *entry_to_ptr(const struct _pht_table *t, uintptr_t e) {
	return (void *)((e & ~t->common_mask) | t->common_bits);
}


static inline uintptr_t ptr_to_entry(
	const struct _pht_table *t, const void *p, uintptr_t perfect)
{
	return ((uintptr_t)p & ~t->common_mask) | perfect;
}


static struct _pht_table *new_table(
	struct pht *ht, const struct _pht_table *prev)
{
	/* find a size that can hold all items in @ht twice before hitting
	 * t_max_elems().
	 */
	size_t target = (ht->elems * 2 * 4) / 3;
	int bits = ht->elems > 0 ? 31 - bitops_clz32(target) : 0;
	if((size_t)1 << bits < target) bits++;
	/* (the t_max_elems formula breaks down below bits < 2.) */
	assert(bits < 2 || ((size_t)3 << bits) / 4 >= ht->elems * 2);
	struct _pht_table *t = calloc(1, sizeof *t + (sizeof(uintptr_t) << bits));
	if(t == NULL) return NULL;

	assert(t->elems == 0);
	assert(t->deleted == 0);
	assert(t->nextmig == 0);
	assert(t->chain_start == 0);
	t->bits = bits;
	if(prev != NULL) {
		t->common_mask = prev->common_mask;
		t->common_bits = prev->common_bits;
		t->perfect_bit = prev->perfect_bit;
	} else {
		t->perfect_bit = NO_PERFECT_BIT;
		t->common_mask = ~0ul;
		assert(t->common_bits == 0);
	}
	list_add(&ht->tables, &t->link);

	return t;
}


static struct _pht_table *update_common(
	struct pht *ht, struct _pht_table *t, const void *p)
{
	assert((uintptr_t)p != TOMBSTONE);
	if(ht->elems == 0) {
		/* de-common exactly one set bit above TOMBSTONE, so that valid
		 * entries will never look like 0 or TOMBSTONE.
		 */
		int b = ffsll((uintptr_t)p & ~3ul) - 1;
		assert(b >= 0);
		t->common_mask = ~((uintptr_t)1 << b);
		t->common_bits = (uintptr_t)p & t->common_mask;
		t->perfect_bit = 1;
	} else {
		if(t->elems > 0) {
			t = new_table(ht, t);
			if(t == NULL) return NULL;
		}

		uintptr_t diffmask = t->common_bits ^ (t->common_mask & (uintptr_t)p);
		t->common_mask &= ~diffmask;
		t->common_bits = (uintptr_t)p & t->common_mask;
		t->perfect_bit = ffsll(t->common_mask & ~1ul) - 1;
		if(t->perfect_bit < 0) t->perfect_bit = NO_PERFECT_BIT;
		assert(t->perfect_bit > 0);
	}
	assert(((uintptr_t)p & ~t->common_mask) != 0
		&& ((uintptr_t)p & ~t->common_mask) != TOMBSTONE);

	return t;
}


static void table_add(struct _pht_table *t, size_t hash, const void *p)
{
	assert(t->elems < 1 << t->bits);
	uintptr_t perfect = 1ul << t->perfect_bit;
	size_t mask = ((size_t)1 << t->bits) - 1, i = hash & mask;
	while(is_valid(t->table[i])) {
		i = (i + 1) & mask;
		assert(i != (hash & mask));
		perfect = 0;
	}

	assert(t->table[i] <= 1);
	assert(t->table[i] == 0 || t->deleted > 0);
	t->deleted -= t->table[i];

	t->table[i] = ptr_to_entry(t, p, perfect);
	assert(is_valid(t->table[i]));
	t->elems++;
}


bool pht_copy(struct pht *dst, const struct pht *src)
{
	return true;
}


static size_t t_max_elems(const struct _pht_table *t) {
	return ((size_t)3 << t->bits) / 4;
}


static size_t t_max_fill(const struct _pht_table *t) {
	/* 0.90625 is close enough to 9/10, and computes faster. */
	return ((size_t)29 << t->bits) / 32;
}


bool pht_add(struct pht *ht, size_t hash, const void *p)
{
	struct _pht_table *t = list_top(&ht->tables, struct _pht_table, link);
	if(unlikely(t == NULL)
		|| t->elems + 1 > t_max_elems(t)
		|| t->elems + 1 + t->deleted > t_max_fill(t))
	{
		/* by the time the max-elems condition hits, migration should have
		 * completed entirely.
		 */
		assert(t == NULL
			|| t->elems + 1 <= t_max_elems(t)
			|| list_tail(&ht->tables, struct _pht_table, link) == t);
		t = new_table(ht, t);
	}
	if(t == NULL) return false;
	assert(t == list_top(&ht->tables, struct _pht_table, link));

	if(((uintptr_t)p & t->common_mask) != t->common_bits) {
		t = update_common(ht, t, p);
	}

	assert(p != NULL);
	table_add(t, hash, p);
	ht->elems++;

	/* where applicable, migrate one item from the very last subtable. */
	struct _pht_table *mig = list_tail(&ht->tables, struct _pht_table, link);
	assert(mig != NULL);
	if(mig != t) {
		assert(mig->elems > 0);
		uintptr_t e;
		bool new_chain = false;
		size_t off = mig->nextmig, mig_size = (size_t)1 << mig->bits,
			lim = mig_size;	/* TODO: scanning policy? */
		assert(lim <= mig_size);
		do {
			assert(off < mig_size);
			e = mig->table[off];
			new_chain |= (e == 0);
		} while(!is_valid(e) && ++off < lim);
		assert(lim < mig_size || is_valid(e));
		if(is_valid(e)) {
			const void *m = entry_to_ptr(mig, e);
			table_add(t, (*ht->rehash)(m, ht->priv), m);
			mig->elems--;
		}
		if(mig->elems > 0 && off + 1 < mig_size) {
			mig->nextmig = off + 1;
			if(new_chain) mig->chain_start = off;
		} else {
			/* dispose of old table. */
			list_del_from(&ht->tables, &mig->link);
			free(mig);
		}
	}

	return true;
}


bool pht_del(struct pht *ht, size_t hash, const void *p)
{
	struct pht_iter it;
	for(void *cand = pht_firstval(ht, &it, hash);
		cand != NULL; cand = pht_nextval(ht, &it, hash))
	{
		if(cand == (void *)p) {
			pht_delval(ht, &it);
			return true;
		}
	}
	return false;
}


static bool table_next(
	const struct pht *ht, struct pht_iter *it, size_t hash,
	uintptr_t *perfect)
{
	it->t = list_next(&ht->tables, it->t, link);
	if(it->t == NULL) return false;

	size_t mask = ((size_t)1 << it->t->bits) - 1, first = hash & mask;
	if(likely(first >= it->t->nextmig)) {
		it->off = first;
		it->last = first;
		*perfect = 1ul << it->t->perfect_bit;
	} else if(first < it->t->chain_start) {
		/* first is in an already-migrated chain; skip table. */
		return table_next(ht, it, hash, perfect);
	} else {
		/* would've started in the migration zone within the existing hash
		 * chain; skip to nextmig and clear perfect.
		 */
		it->off = it->t->nextmig;
		it->last = 0;
		*perfect = 0;
	}

	assert(it->off >= it->t->nextmig);
	return true;
}


static void *table_val(
	const struct pht *ht, struct pht_iter *it,
	size_t hash, uintptr_t perfect)
{
	assert(it->t != NULL);
	const struct _pht_table *t = it->t;
	size_t mask = ((size_t)1 << t->bits) - 1, off = it->off;
	assert(off >= t->nextmig);
	do {
		if(is_valid(t->table[off])
			&& (t->table[off] & t->common_mask) == perfect)
		{
			it->off = off;
			return entry_to_ptr(t, t->table[off]);
		}
		if(t->table[off] == 0) break;
		perfect = 0;
		off = (off + 1) & mask;
		if(off == 0 && off != it->last) off = t->nextmig;
	} while(off != it->last);

	if(table_next(ht, it, hash, &perfect)) {
		return table_val(ht, it, hash, perfect);
	} else {
		/* done. */
		assert(it->t == NULL);
		return NULL;
	}
}


void *pht_firstval(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	it->t = list_top(&ht->tables, struct _pht_table, link);
	if(it->t == NULL) return NULL;
	assert(it->t->nextmig == 0);

	size_t mask = ((size_t)1 << it->t->bits) - 1;
	it->off = hash & mask;
	it->last = it->off;
	it->hash = hash;
	return table_val(ht, it, hash, 1ul << it->t->perfect_bit);
}


void *pht_nextval(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	if(it->t == NULL) return NULL;
	it->off = (it->off + 1) & ((1u << it->t->bits) - 1);
	uintptr_t perf = 0;
	if(it->off == it->last
		|| (it->off == 0 && it->t->chain_start > 0)
		|| (it->off == 0 && it->last <= it->t->nextmig))
	{
		/* end of probe */
		if(!table_next(ht, it, hash, &perf)) return NULL;
	} else if(it->off == 0) {
		/* wrap around */
		it->off = it->t->nextmig;
	}
	return table_val(ht, it, hash, perf);
}


void pht_delval(struct pht *ht, struct pht_iter *it)
{
	assert(it->t != NULL);
	assert(it->t->elems > 0);
	assert(is_valid(it->t->table[it->off]));

	it->t->table[it->off] = TOMBSTONE;
	it->t->deleted++;
	ht->elems--;
	if(--it->t->elems == 0
		&& it->t != list_top(&ht->tables, struct _pht_table, link))
	{
		struct _pht_table *dead = it->t;
		table_next(ht, it, it->hash, &(uintptr_t){ 0 });
		list_del_from(&ht->tables, &dead->link);
		free(dead);
	}
}


static bool table_next_all(const struct pht *ht, struct pht_iter *it)
{
	it->t = list_next(&ht->tables, it->t, link);
	if(it->t == NULL) return false;

	assert(it->last == 0);
	assert(it->hash == 0);
	it->off = it->t->nextmig;
	return true;
}


static void *table_val_all(const struct pht *ht, struct pht_iter *it)
{
	assert(it->t != NULL);
	size_t off = it->off, last = (size_t)1 << it->t->bits;
	do {
		if(is_valid(it->t->table[off])) {
			it->off = off;
			return entry_to_ptr(it->t, it->t->table[off]);
		}
	} while(++off != last);
	return table_next_all(ht, it) ? table_val_all(ht, it) : NULL;
}


void *pht_first(const struct pht *ht, struct pht_iter *it)
{
	it->t = list_top(&ht->tables, struct _pht_table, link);
	if(it->t == NULL) return NULL;
	assert(it->t->nextmig == 0);

	it->off = 0; it->last = 0; it->hash = 0;
	return table_val_all(ht, it);
}


void *pht_next(const struct pht *ht, struct pht_iter *it)
{
	if(it->t == NULL) return NULL;
	return ++it->off < (size_t)1 << it->t->bits || table_next_all(ht, it)
		? table_val_all(ht, it) : NULL;
}


void *pht_prev(const struct pht *ht, struct pht_iter *it)
{
	return NULL;
}
