
/* progressively rehashed hashed multiset. implements the same interface as
 * CCAN htable, but spends more memory to provide nicer upper bounds for
 * htable_add().
 */
#ifndef _PHT_H
#define _PHT_H

#include <stdbool.h>
#include <stdlib.h>
#include <ccan/list/list.h>


struct _pht_table;

struct pht
{
	size_t (*rehash)(const void *, void *);
	void *priv;
	size_t elems;
	struct list_head tables; /* of _pht_table */
};


#define PHT_INITIALIZER(name, rehash, priv) \
	{ (rehash), (priv), 0, LIST_HEAD_INIT((name).tables) }

extern void pht_init(struct pht *ht,
	size_t (*rehash)(const void *elem, void *priv), void *priv);

extern size_t pht_count(const struct pht *ht);
extern void pht_clear(struct pht *ht);

/* heavyweight fsck-like operation on @ht, useful for catching memory
 * corruption not found by valgrind. not compiled under NDEBUG. returns @ht
 * always.
 */
extern struct pht *pht_check(const struct pht *ht, const char *abortstr);

/* pht_add() returns true on success, and false when either there was a malloc
 * failure or @p is NULL, which cannot be added or found by iterator under the
 * current interface.
 *
 * NOTE: due to effects of progressive migration, calling pht_add()
 * invalidates all iterators referencing @ht.
 */
extern bool pht_add(struct pht *ht, size_t hash, const void *p);
extern bool pht_copy(struct pht *dst, const struct pht *src);
extern bool pht_del(struct pht *ht, size_t hash, const void *p);

struct pht_iter {
	struct _pht_table *t;
	size_t off, last, hash;
};

extern void *pht_firstval(const struct pht *ht,
	struct pht_iter *it, size_t hash);
extern void *pht_nextval(const struct pht *ht,
	struct pht_iter *it, size_t hash);
extern void pht_delval(struct pht *ht, struct pht_iter *it);

static inline void *pht_get(const struct pht *ht, size_t h,
	bool (*cmp)(const void *cand, void *ptr), const void *ptr)
{
	struct pht_iter it;
	void *cand = pht_firstval(ht, &it, h);
	while(cand != NULL && !(*cmp)(cand, (void *)ptr)) {
		cand = pht_nextval(ht, &it, h);
	}
	return cand;
}

extern void *pht_first(const struct pht *ht, struct pht_iter *it);
extern void *pht_next(const struct pht *ht, struct pht_iter *it);
extern void *pht_prev(const struct pht *ht, struct pht_iter *it);


#endif
