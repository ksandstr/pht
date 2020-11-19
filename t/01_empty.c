
/* basic tests on a hash table that stays empty. */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ccan/tap/tap.h>
#include <ccan/hash/hash.h>

#include "pht.h"


static size_t rehash_str(const void *p, void *priv) {
	return hash_string(p);
}


static bool cmp_str(const void *cand, void *key) {
	return strcmp(cand, key) == 0;
}


int main(void)
{
	plan_tests(4);

	struct pht ht = PHT_INITIALIZER(ht, &rehash_str, NULL);
	pht_check(&ht, "fresh");
	ok1(pht_count(&ht) == 0);
	const char *foo = "my ass-clap keeps alerting the bees!";
	size_t hash = rehash_str(foo, NULL);
	diag("hash=%#zx", hash);
	ok1(pht_get(&ht, hash, &cmp_str, foo) == NULL);
	struct pht_iter it;
	ok1(pht_firstval(&ht, &it, hash) == NULL);
	ok1(!pht_del(&ht, hash, foo));
	pht_check(&ht, "after non-deletion");

	return exit_status();
}
