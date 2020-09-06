
#include <stdbool.h>
#include <stdlib.h>
#include <ccan/list/list.h>

#include "pht.h"


struct _pht_table {
	struct list_node link;	/* in pht.tables */
};


void pht_init(struct pht *ht,
	size_t (*rehash)(const void *elem, void *priv), void *priv)
{
}


size_t pht_count(const struct pht *ht)
{
	return 0;
}


void pht_clear(struct pht *ht)
{
}


struct pht *pht_check(const struct pht *ht, const char *abortstr) {
	/* TODO: the rest of the fucking owl */
	return (struct pht *)ht;
}


bool pht_copy(struct pht *dst, const struct pht *src)
{
	return true;
}


bool pht_add(struct pht *ht, size_t hash, const void *p)
{
	return true;
}


bool pht_del(struct pht *ht, size_t hash, const void *p)
{
	return false;
}


void *pht_firstval(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	return NULL;
}


void *pht_nextval(const struct pht *ht, struct pht_iter *it, size_t hash)
{
	return NULL;
}


void pht_delval(struct pht *ht, struct pht_iter *it)
{
}


void *pht_first(const struct pht *ht, struct pht_iter *it)
{
	return NULL;
}


void *pht_next(const struct pht *ht, struct pht_iter *it)
{
	return NULL;
}


void *pht_prev(const struct pht *ht, struct pht_iter *it)
{
	return NULL;
}
