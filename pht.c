
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


/* (see t_perfect_mask()) */
#define NO_PERFECT_BIT (sizeof(uintptr_t) * CHAR_BIT - 1)
#define TOMBSTONE (1)

/* _pht_table flags */
#define KEEP_CHAIN 1
#define CHAIN_SAFE 2


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
	int credit;	/* # of extra entries moved without rehash */
	uintptr_t common_bits, common_mask;
	uint16_t flags;	/* , as is tradition */
	uint8_t bits;	/* size_log2 */
	uint8_t perfect_bit;

	uintptr_t table[];
};


static bool is_valid(uintptr_t e) {
	return e != 0 && e != TOMBSTONE;
}


static size_t t_max_elems(const struct _pht_table *t) {
	return ((size_t)3 << t->bits) / 4;
}


static size_t t_max_fill(const struct _pht_table *t) {
	/* 0.90625 is close enough to 9/10, and computes faster. */
	return ((size_t)29 << t->bits) / 32;
}


static uintptr_t t_perfect_mask(const struct _pht_table *t) {
	/* shifting by the width of a value is undefined, so we'll shift word-size
	 * 2 by at most its width minus one, and never allocate the perfect bit at
	 * the very bottom.
	 */
	return (uintptr_t)2 << t->perfect_bit;
}


static inline size_t t_bucket(const struct _pht_table *t, size_t hash) {
	/* increase at the low end to optimize rehash avoidance. since many hash
	 * functions are stronger at the low end, rotate to the right 17 bits
	 * (arbitrarily) and xor it in.
	 */
	hash ^= (hash >> 17) | (hash << (sizeof hash * CHAR_BIT - 17));
	return t->bits > 0 ? hash >> (sizeof hash * CHAR_BIT - t->bits) : 0;
}


static inline void *entry_to_ptr(const struct _pht_table *t, uintptr_t e) {
	return (void *)((e & ~t->common_mask) | t->common_bits);
}


static inline uintptr_t ptr_to_entry(
	const struct _pht_table *t, const void *p)
{
	return (uintptr_t)p & ~t->common_mask;
}


static inline uintptr_t stash_bits(const struct _pht_table *t, size_t hash) {
	/* same reason as t_bucket(), but this time because most of the common
	 * bits are up high. rotation distance picked arbitrarily.
	 */
	hash ^= (hash >> 14) | (hash << (sizeof hash * CHAR_BIT - 14));
	return hash & t->common_mask & ~t_perfect_mask(t);
}


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
	ssize_t phantom = ht->elems;
	const struct _pht_table *t,
		*primary = list_top(&ht->tables, struct _pht_table, link);
	list_for_each(&ht->tables, t, link) {
		phantom -= t->elems;
		assert(t->deleted <= (size_t)1 << t->bits);

		size_t deleted = 0, empty = 0, item = 0;
		uintptr_t perf_mask = t_perfect_mask(t);
		for(size_t i=0; i < (size_t)1 << t->bits; i++) {
			uintptr_t e = t->table[i];
			switch(e) {
				case 0: empty++; break;
				case TOMBSTONE: deleted++; break;
				default:
					assert(is_valid(e));
					if(i >= t->nextmig) item++; else empty++;
			}

			/* (we require these things of items behind the migration horizon,
			 * too, for the purpose of catching memory corruption there as
			 * well. performance junkies should disagree.)
			 */
			if(is_valid(e)) {
				uintptr_t extra = e & t->common_mask;
				size_t hash = (*ht->rehash)(entry_to_ptr(t, e), ht->priv);

				assert((extra & ~perf_mask) == stash_bits(t, hash));
				assert(!!(e & perf_mask) == (i == t_bucket(t, hash)));
				if(~e & perf_mask) {
					/* a contiguous hash chain exists from the home slot to
					 * `i'.
					 */
					size_t slot = t_bucket(t, hash);
					while(slot != i) {
						assert(t->table[slot] != 0);
						slot = (slot + 1) & (((size_t)1 << t->bits) - 1);
					}
				}
			}
		}
		assert(deleted == t->deleted);
		assert(item == t->elems);
		assert(empty == ((size_t)1 << t->bits) - t->deleted - t->elems);

		/* only the first secondary's tombstones are retained, since migration
		 * proceeds back to front.
		 */
		assert(list_prev(&ht->tables, t, link) == primary
			|| (~t->flags & KEEP_CHAIN));
		assert((~t->flags & KEEP_CHAIN) || t->bits >= primary->bits);
	}
	assert(phantom == 0);
#endif

	return (struct pht *)ht;
}


static struct _pht_table *new_table(
	struct pht *ht, struct _pht_table *prev,
	bool keep_chain)
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
	assert(t->credit == 0);
	t->bits = bits;
	if(prev != NULL) {
		t->common_mask = prev->common_mask;
		t->common_bits = prev->common_bits;
		t->perfect_bit = prev->perfect_bit;
		assert(~prev->flags & KEEP_CHAIN);
		assert(~prev->flags & CHAIN_SAFE);
		if(keep_chain && prev->bits >= t->bits) prev->flags |= KEEP_CHAIN;
	} else {
		t->perfect_bit = NO_PERFECT_BIT;
		t->common_mask = ~0ul;
		assert(t->common_bits == 0);
	}
	list_add(&ht->tables, &t->link);

	/* since migration proceeds oldest-first, we must only rely on tombstone
	 * recreation in the most recent table.
	 */
	struct _pht_table *oth;
	list_for_each(&ht->tables, oth, link) {
		if(oth != t && oth != prev) oth->flags &= ~KEEP_CHAIN;
	}

	return t;
}


static struct _pht_table *update_common(
	struct pht *ht, struct _pht_table *t, const void *p)
{
	assert((uintptr_t)p != TOMBSTONE);
	if(ht->elems == 0) {
		/* de-common exactly one set bit above TOMBSTONE, so that the sole
		 * valid entry won't look like 0 or TOMBSTONE.
		 */
		int b = ffsl((uintptr_t)p & ~1ul) - 1;
		assert(b >= 0);
		t->common_mask = ~((uintptr_t)1 << b);
		t->common_bits = (uintptr_t)p & t->common_mask;

		/* this'd waste both space and scanning time when t->bits > 0, so
		 * let's only waste space instead.
		 */
		assert(t->elems == 0);
		t->bits = 0;
	} else {
		if(t->elems > 0) {
			t = new_table(ht, t, true);
			if(t == NULL) return NULL;
		}

		uintptr_t diffmask = t->common_bits ^ (t->common_mask & (uintptr_t)p);
		t->common_mask &= ~diffmask;
		t->common_bits = (uintptr_t)p & t->common_mask;
	}
	assert(((uintptr_t)p & ~t->common_mask) != 0
		&& ((uintptr_t)p & ~t->common_mask) != TOMBSTONE);

	int pb = ffsl(t->common_mask & ~1ul) - 1;
	t->perfect_bit = pb == 0 ? NO_PERFECT_BIT : pb - 1;
	assert(t->common_mask & t_perfect_mask(t));

	return t;
}


static void table_add(struct _pht_table *t, size_t hash, const void *p)
{
	assert(t->elems < 1 << t->bits);
	uintptr_t perfect = t_perfect_mask(t),
		e = stash_bits(t, hash) | ptr_to_entry(t, p);
	size_t mask = ((size_t)1 << t->bits) - 1, i = t_bucket(t, hash);
	if(is_valid(t->table[i]) && (~t->table[i] & perfect)) {
		/* use an imperfect entry's slot to store @p perfectly, then
		 * reinsert the previous item somewhere down the hash chain.
		 */
		uintptr_t olde = t->table[i];
		t->table[i] = e | perfect;
		e = olde;
		perfect = 0;
		i = (i + 1) & mask;
	}
	while(is_valid(t->table[i])) {
		i = (i + 1) & mask;
		assert(i != t_bucket(t, hash));
		perfect = 0;
	}

	assert(t->table[i] <= 1);
	assert(t->table[i] == 0 || t->deleted > 0);
	t->deleted -= t->table[i];

	t->table[i] = e | perfect;
	assert(is_valid(t->table[i]));
	t->elems++;
}


/* migrate @mig->table[off] == @e to @t while avoiding a rehash. returns false
 * if the item must be rehashed and reinserted, and true otherwise. caller
 * must adjust @mig->elems when successful.
 */
static bool fast_migrate(
	struct _pht_table *t, struct _pht_table *mig, uintptr_t e)
{
	assert(t->elems < (size_t)1 << t->bits);
	assert(t->nextmig == 0);
	/* compatibility criteria between the two tables: the target table must
	 * have a common_mask no heavier than the source, so that extra bits can
	 * be copied over without rehashing. asserted because this should be
	 * guaranteed by update_common() and new_table().
	 */
	assert((t->common_mask & ~mig->common_mask) == 0);
	/* perfect-bits should also be compatible by either being the same bit, by
	 * the target not having a perfect bit, or by having the source table's
	 * perfect bit unmasked in the destination. asserted because that's what
	 * the algorithm expects.
	 */
	assert(t->perfect_bit == NO_PERFECT_BIT
		|| t->perfect_bit == mig->perfect_bit
		|| (~t->common_mask & t_perfect_mask(mig)));
	size_t off = mig->nextmig - 1, t_mask = ((size_t)1 << t->bits) - 1;
	uintptr_t perfect;
	if(e & t_perfect_mask(mig)) {
		if(t->bits <= mig->bits) {
			/* perfect items may migrate to same-sized and smaller tables
			 * directly, losing the perfect bit only when the sole home
			 * position is occupied.
			 */
			off >>= mig->bits - t->bits;
			perfect = t_perfect_mask(t);
		} else {
			/* a perfect item may also migrate to a position after its home
			 * slot range in a larger table iff those slots are already
			 * non-empty. we cannot use a tombstone slot in that range, or the
			 * last slot, because it's not known if the perfect bit should be
			 * set.
			 */
			if(t->bits < 2) {
				/* this breaks down when there are exactly two slots;
				 * then we'd get the perfect bit wrong half the time.
				 */
				return false;
			}
			int scale = t->bits - mig->bits;
			assert((off + 1) << scale <= (size_t)1 << t->bits);
			for(size_t i = off << scale; i < (off + 1) << scale; i++) {
				/* to make that happen, let's instead add tombstones so that
				 * all perfect items migrate without rehash even if that loses
				 * perfect until next time.
				 */
				if(t->table[i] == 0) {
					t->table[i] = TOMBSTONE;
					t->deleted++;
				}
			}
			off = ((off + 1) << scale) & t_mask;
			perfect = 0;
		}
	} else if(mig->chain_start == 0) {
		/* imperfect items until the first chain break may have wrapped
		 * around, so should be rehashed always.
		 *
		 * (technically this could be avoided when the very last slot is 0,
		 * possibly by setting chain_start to 1 << mig->bits, but there's no
		 * convenient way to test that right now.)
		 */
		assert(~e & t_perfect_mask(mig));
		assert(~mig->flags & CHAIN_SAFE);
		return false;
	} else {
		/* imperfect items may migrate to a corresponding position, or farther
		 * down, iff all the potential slots of the item's entire hash chain
		 * are occupied in the larger destination. this is guaranteed when the
		 * latter is no larger than the source and tombstones are copied by
		 * migration, or when there aren't chain-ends produced by not-copied
		 * tombstones or imperfect migrations to a larger table.
		 */
		if(t->bits <= mig->bits) {
			if((~mig->flags & KEEP_CHAIN) && (~mig->flags & CHAIN_SAFE)) {
				return false;
			}
			off >>= mig->bits - t->bits;
		} else if(mig->flags & CHAIN_SAFE) {
			off <<= t->bits - mig->bits;
			mig->flags &= ~CHAIN_SAFE;
		} else {
			return false;
		}
		perfect = 0;
	}

	/* brekkie's up, ya slack cunt */
	assert(off < (size_t)1 << t->bits);
	e = (e & t->common_mask & ~t_perfect_mask(t))
		| (((e & ~mig->common_mask) | mig->common_bits) & ~t->common_mask);
	if(is_valid(t->table[off]) && (~t->table[off] & perfect)) {
		/* same bump logic as in table_add() */
		assert(~t->table[off] & t_perfect_mask(t));
		assert(perfect == t_perfect_mask(t));
		uintptr_t olde = t->table[off];
		t->table[off] = e | perfect;
		e = olde;
		perfect = 0;
		off = (off + 1) & t_mask;
	}
	assert(~e & t_perfect_mask(t));
	while(is_valid(t->table[off])) {
		perfect = 0;
		off = (off + 1) & t_mask;
	}
	t->deleted -= t->table[off];
	t->table[off] = e | perfect;
	t->elems++;

	return true;
}


/* @mig is invalidated when @mig->elems == 1 before call. */
static bool mig_item(
	struct pht *ht, struct _pht_table *t, struct _pht_table *mig,
	uintptr_t e, bool fast_only)
{
	assert(is_valid(e));
	bool fast = fast_migrate(t, mig, e);
	if(!fast) {
		if(fast_only) return false;
		const void *m = entry_to_ptr(mig, e);
		table_add(t, (*ht->rehash)(m, ht->priv), m);
	}
	if(unlikely(--mig->elems == 0)) {
		/* dispose of old table. */
		list_del_from(&ht->tables, &mig->link);
		free(mig);
	}
	return fast;
}


static inline void mig_scan_item(
	struct _pht_table *t, struct _pht_table *mig,
	uintptr_t e)
{
	if(e == 0) {
		mig->chain_start = mig->nextmig;
		mig->flags |= CHAIN_SAFE;
	} else if(e == TOMBSTONE) {
		mig->flags &= ~CHAIN_SAFE;
		if(mig->flags & KEEP_CHAIN) {
			assert(mig->bits >= t->bits);
			size_t off = (mig->nextmig - 1) >> (mig->bits - t->bits);
			if(t->table[off] == 0) {
				t->table[off] = TOMBSTONE;
				t->deleted++;
			}
		}
	}
}


/* as necessary for a single successful call of pht_add(), migrate one item
 * from the very last subtable while calling rehash at most once.
 */
static void mig_step(struct pht *ht, struct _pht_table *t)
{
	struct _pht_table *mig = list_tail(&ht->tables, struct _pht_table, link);
	assert(mig != NULL);
	if(mig == t) return;
	assert(mig->elems > 0);

	if(mig->credit > 0 && ((uintptr_t)&mig->table[mig->nextmig] & 63) == 0) {
		mig->credit--;
		return;
	}

	/* the first scan looks for an item at any distance, since at least one
	 * must be moved per step.
	 */
	uintptr_t e;
	do {
		assert(mig->nextmig < (size_t)1 << mig->bits);
		e = mig->table[mig->nextmig++];
		mig_scan_item(t, mig, e);
	} while(!is_valid(e));
	size_t elems = mig->elems - 1;
	bool rehashed = !mig_item(ht, t, mig, e, false);
	if(elems == 0) return;
	assert(elems == mig->elems);

	/* the second scan tries to finish the last cacheline touched, stopping
	 * only if a second item requiring a rehash is found.
	 */
	ssize_t left = (64 - ((uintptr_t)&mig->table[mig->nextmig] & 63)) & 63;
	size_t lim = min((size_t)1 << mig->bits,
		mig->nextmig + left / sizeof(uintptr_t));
	while(mig->nextmig < lim) {
		e = mig->table[mig->nextmig++];
		mig_scan_item(t, mig, e);
		assert((left -= sizeof(uintptr_t), left >= 0));
		if(is_valid(e)) {
			assert(elems == mig->elems);
			if(!mig_item(ht, t, mig, e, rehashed)) {
				if(rehashed) {
					mig->nextmig--;
					return;
				}
				rehashed = true;
			}
			if(--elems == 0) return;
			mig->credit++;
		}
	}
	assert(left == 0);
}


bool pht_add(struct pht *ht, size_t hash, const void *p)
{
	if(unlikely(p == NULL)) return false;

	struct _pht_table *t = list_top(&ht->tables, struct _pht_table, link);
	if(unlikely(t == NULL
		|| t->elems + 1 > t_max_elems(t)
		|| t->elems + 1 + t->deleted > t_max_fill(t)))
	{
		/* by the time the max-elems condition hits, migration should have
		 * completed entirely.
		 */
		assert(t == NULL
			|| t->elems + 1 <= t_max_elems(t)
			|| list_tail(&ht->tables, struct _pht_table, link) == t);

		/* remove tombstones when fill condition was hit. */
		t = new_table(ht, t,
			t == NULL || t->elems + 1 + t->deleted <= t_max_fill(t));
		if(unlikely(t == NULL)) return false;
	}
	assert(t == list_top(&ht->tables, struct _pht_table, link));

	if(((uintptr_t)p & t->common_mask) != t->common_bits) {
		t = update_common(ht, t, p);
		if(unlikely(t == NULL)) return false;
	}

	assert(p != NULL);
	table_add(t, hash, p);
	ht->elems++;

	mig_step(ht, t);
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

#ifdef DEBUG_ME_HARDER
	/* verify that the value definitely doesn't exist, or that it doesn't
	 * rehash to @hash. (the latter is pedantic, but correct because iteration
	 * is not guaranteed not to return @p under some other hash.)
	 */
	for(void *cand = pht_first(ht, &it);
		cand != NULL; cand = pht_next(ht, &it))
	{
		assert(cand != p || (*ht->rehash)(cand, ht->priv) != hash);
	}
#endif

	return false;
}


bool pht_copy(struct pht *dst, const struct pht *src)
{
	pht_init(dst, src->rehash, src->priv);
	/* when in doubt, use brute force. it'd be much quicker to complete all
	 * migration in @src and then memdup the resulting primary, but this one
	 * is simpler at the cost of forming fresh hash chains in the destination
	 * and using more memory.
	 */
	struct pht_iter it;
	for(void *ptr = pht_first(src, &it);
		ptr != NULL; ptr = pht_next(src, &it))
	{
		bool ok = pht_add(dst, (*src->rehash)(ptr, src->priv), ptr);
		if(!ok) {
			pht_clear(dst);
			return false;
		}
	}
	return true;
}


static bool table_next(
	const struct pht *ht, struct pht_iter *it, size_t hash,
	uintptr_t *perfect)
{
	it->t = list_next(&ht->tables, it->t, link);
	if(it->t == NULL) return false;

	assert(it->hash == hash);
	size_t first = t_bucket(it->t, hash);
	if(first >= it->t->nextmig) {
		it->off = first;
		it->last = first;
		*perfect = t_perfect_mask(it->t);
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
	assert(it->hash == hash);
	const struct _pht_table *t = it->t;
	size_t off = it->off;
	uintptr_t extra = stash_bits(it->t, hash) | perfect;
	assert(off >= t->nextmig);
	do {
		if(is_valid(t->table[off])
			&& (t->table[off] & t->common_mask) == extra)
		{
			it->off = off;
			return entry_to_ptr(t, t->table[off]);
		}
		if(t->table[off] == 0) break;
		extra &= ~perfect;
		off = (off + 1) & (((size_t)1 << t->bits) - 1);
		if(off == 0 && off != it->last) {
			if(t->chain_start > 0) break;
			off = t->nextmig;
		}
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
	if(unlikely(it->t == NULL)) return NULL;
	assert(it->t->nextmig == 0);

	it->off = t_bucket(it->t, hash);
	it->last = it->off;
	it->hash = hash;
	return table_val(ht, it, hash, t_perfect_mask(it->t));
}


void *pht_nextval(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	if(it->t == NULL) return NULL;
	it->off = (it->off + 1) & (((size_t)1 << it->t->bits) - 1);
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

	ht->elems--;
	if(unlikely(--it->t->elems == 0)
		&& (it->t != list_top(&ht->tables, struct _pht_table, link)
			|| it->t == list_tail(&ht->tables, struct _pht_table, link)))
	{
		/* (that or-clause is a mildly inobvious way to test for either a
		 * non-first item, or a sole item.)
		 */
		struct _pht_table *dead = it->t;
		table_next(ht, it, it->hash, &(uintptr_t){ 0 });
		list_del_from(&ht->tables, &dead->link);
		free(dead);
	} else {
		it->t->table[it->off] = TOMBSTONE;
		it->t->deleted++;
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
	if(unlikely(it->t == NULL)) return NULL;
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
	/* TODO */
	return NULL;
}
