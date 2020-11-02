
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <ccan/list/list.h>
#include <ccan/minmax/minmax.h>
#include <ccan/likely/likely.h>

#include "pht.h"


#define TOMBSTONE (1)

#define MSB(x) (sizeof((x)) * CHAR_BIT - __builtin_clzl((x)) - 1)


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
	short bits;	/* size_log2 */
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


static struct _pht_table *new_table(struct pht *ht, int sizelog2)
{
	assert(sizelog2 >= 0);
	size_t size = (size_t)1 << sizelog2;

	struct _pht_table *t = calloc(1, sizeof *t + sizeof(uintptr_t) * size);
	if(t == NULL) return NULL;
	assert(t->elems == 0);
	assert(t->deleted == 0);
	assert(t->nextmig == 0);
	assert(t->chain_start == 0);
	t->bits = sizelog2;
	list_add(&ht->tables, &t->link);

	return t;
}


static void table_add(struct _pht_table *t, size_t hash, const void *p)
{
	size_t mask = ((size_t)1 << t->bits) - 1, i = hash & mask;
	while(is_valid(t->table[i])) {
		i = (i + 1) & mask;
		assert(i != (hash & mask));
	}
#if 0
	if(t->table[i] == TOMBSTONE) {
		assert(t->deleted > 0);
		t->deleted--;
	}
#else
	assert(t->table[i] <= 1);
	assert(t->table[i] == 0 || t->deleted > 0);
	t->deleted -= t->table[i];
#endif
	t->table[i] = (uintptr_t)p;
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
	if(unlikely(t == NULL)) {
		assert(t == NULL);
		t = new_table(ht, 0);
	} else if(t->elems + 1 > t_max_elems(t)
		|| t->elems + 1 + t->deleted > t_max_fill(t))
	{
		/* TODO: find next size that can hold all of them, since the
		 * fill-condition may be satisfied with a low element count.
		 */
		t = new_table(ht, t->bits + 1);
	}
	if(t == NULL) return false;
	assert(t == list_top(&ht->tables, struct _pht_table, link));

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
		if(is_valid(e)) {
			const void *m = (void *)e;
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


static bool table_next(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	it->t = list_next(&ht->tables, it->t, link);
	if(it->t == NULL) return false;

	size_t mask = ((size_t)1 << it->t->bits) - 1, first = hash & mask;
	if(likely(first >= it->t->nextmig)) {
		it->off = first;
		it->last = first;
	} else if(first < it->t->chain_start) {
		/* first is in an already-migrated chain; skip table. */
		return table_next(ht, it, hash);
	} else {
		/* would've started in the migration zone within the existing hash
		 * chain; skip to nextmig.
		 */
		it->off = it->t->nextmig;
		it->last = 0;
	}

	assert(it->off >= it->t->nextmig);
	return true;
}


static void *table_val(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	assert(it->t != NULL);
	const struct _pht_table *t = it->t;
	size_t mask = ((size_t)1 << t->bits) - 1, off = it->off;
	assert(off >= t->nextmig);
	do {
		if(is_valid(t->table[off])) {
			it->off = off;
			return (void *)t->table[off];
		}
		if(t->table[off] == 0) break;
		off = (off + 1) & mask;
		if(off == 0 && off != it->last) off = t->nextmig;
	} while(off != it->last);

	if(table_next(ht, it, hash)) return table_val(ht, it, hash);
	else {
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
	return table_val(ht, it, hash);
}


void *pht_nextval(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	if(it->t == NULL) return NULL;
	it->off = (it->off + 1) & ((1u << it->t->bits) - 1);
	if(it->off == it->last
		|| (it->off == 0 && it->t->chain_start > 0)
		|| (it->off == 0 && it->last <= it->t->nextmig))
	{
		/* end of probe */
		if(!table_next(ht, it, hash)) return NULL;
	} else if(it->off == 0) {
		/* wrap around */
		it->off = it->t->nextmig;
	}
	return table_val(ht, it, hash);
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
		table_next(ht, it, it->hash);
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
			/* TODO: demunge the pointer */
			it->off = off;
			return (void *)it->t->table[off];
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
