
/* very basic add/get/del/iter tests. other tests may be constructed
 * specifically to provoke various internal states of pht.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ccan/tap/tap.h>
#include <ccan/hash/hash.h>
#include <ccan/array_size/array_size.h>

#include "pht.h"


static bool cmp_verbose = false;


static size_t rehash_str(const void *p, void *priv) {
	return hash(p, strlen(p), (uintptr_t)priv);
}


static bool cmp_str(const void *cand, void *key) {
	if(cmp_verbose) {
		diag("%s: cand=`%s', key=`%s'", __func__,
			(const char *)cand, (char *)key);
	}
	return strcmp(cand, key) == 0;
}


static size_t key_count(
	const struct pht *ht, size_t hash,
	bool (*cmp)(const void *, void *), const void *key)
{
	size_t count = 0;
	struct pht_iter it;
	for(void *cand = pht_firstval(ht, &it, hash);
		cand != NULL; cand = pht_nextval(ht, &it, hash))
	{
		if((*cmp)(cand, (void *)key)) count++;
	}
	return count;
}


int main(void)
{
	plan_tests(15);
	todo_start("o rly?");

	static const char *const strs[] = {
		"my ass-clap keeps alerting the bees!",
		"foo", "bar", "zot", "hoge", "lemon", "melon", "grape",
		"banana", "apple", "orange", "watermelon", "rhubarb",
		"parsnip", "barley", "maize", "rye", "flax", "quinoa",
		"tea", "coffee", "cocoa", "data", "datum", "datums",
		"mutex", "mutices", "mutexes", "gecko", "newt", "rothe",
		"iguana", "woodchuck", "oracle", "vlad", "rodney",
		"the wood nymph zaps a wand of death! -more-",
	};
	assert(ARRAY_SIZE(strs) == 37);	/* because prime. */

	struct pht ht = PHT_INITIALIZER(ht, &rehash_str, NULL);
	ok1(pht_count(&ht) == 0);

	/* add strings. */
	bool adds_ok = true, counts_ok = true, total_ok = true;
	for(int i=0; i < ARRAY_SIZE(strs); i++) {
		size_t hash = rehash_str(strs[i], NULL);
		//diag("str=`%s', hash=%#zx", strs[i], hash);
		bool ok = pht_add(&ht, hash, strs[i]);
		if(!ok) {
			diag("add failed at i=%d", i);
			adds_ok = false;
		}

		if(pht_count(&ht) != i + 1) {
			diag("table count=%zu is wrong at i=%d", pht_count(&ht), i);
			total_ok = false;
		}

		/* check that previously-added strings are found exactly once. */
		for(int j=0; j < ARRAY_SIZE(strs); j++) {
			hash = rehash_str(strs[j], NULL);
			size_t ct = key_count(&ht, hash, &cmp_str, strs[j]);
			if((j <= i && ct != 1) || (j > i && ct > 0)) {
				diag("count=%zu for j=%d `%s' is wrong", ct, j, strs[j]);
				counts_ok = false;
			}
		}
	}
	ok1(adds_ok);
	ok1(counts_ok);
	ok1(total_ok);
	ok1(pht_count(&ht) == ARRAY_SIZE(strs));

	/* delete ones at odd indexes */
	bool dels_ok = true;
	int n_removed = 0;
	for(int i=1; i < ARRAY_SIZE(strs); i += 2) {
		bool ok = pht_del(&ht, rehash_str(strs[i], NULL), strs[i]);
		if(!ok) dels_ok = false; else n_removed++;
	}
	ok1(dels_ok);
	ok1(pht_count(&ht) == ARRAY_SIZE(strs) - n_removed);

	/* look them up, one by one. */
	cmp_verbose = true;
	bool gets_ok = true, notgets_ok = true;
	for(int i=0; i < ARRAY_SIZE(strs); i++) {
		size_t hash = rehash_str(strs[i], NULL);
		void *ptr = pht_get(&ht, hash, &cmp_str, strs[i]);
		if((i & 1) && ptr != NULL) {
			diag("`%s' found at i=%d, should not", strs[i], i);
			notgets_ok = false;
		} else if((~i & 1) && (ptr == NULL || ptr != strs[i])) {
			diag("`%s' not found at i=%d, but should", strs[i], i);
			gets_ok = false;
		}
	}
	ok1(gets_ok);
	ok1(notgets_ok);

	/* iterate through the whole thing, confirm that even ones are there and
	 * odd ones aren't.
	 */
	bool itn_ok = true;
	int present[ARRAY_SIZE(strs)];
	for(int i=0; i < ARRAY_SIZE(present); i++) present[i] = 0;
	struct pht_iter it;
	for(const char *cand = pht_first(&ht, &it);
		cand != NULL; cand = pht_next(&ht, &it))
	{
		int i;
		for(i=0; i < ARRAY_SIZE(strs); i++) {
			if(streq(strs[i], cand)) break;
		}
		assert(i < ARRAY_SIZE(strs));
		present[i]++;
		if(i & 1) {
			diag("cand=`%s', i=%d shouldn't be found by iterator", cand, i);
			itn_ok = false;
		}
	}
	ok1(itn_ok);
	bool it_ok = true, it_count_ok = true;
	for(int i=0; i < ARRAY_SIZE(strs); i += 2) {
		if(present[i] == 0) {
			diag("strs[%d]=`%s' not found by iterator", i, strs[i]);
			it_ok = false;
		} else if(present[i] > 1) {
			diag("strs[%d]=`%s' found %d times by iterator",
				i, strs[i], present[i]);
			it_count_ok = false;
		}
	}
	ok1(it_ok);
	ok1(it_count_ok);

	/* remove the even ones, but this time open-code the iteration, and go
	 * until the end to confirm that exactly one was seen.
	 */
	bool clean_ok = true;
	for(int i=0; i < ARRAY_SIZE(strs); i += 2) {
		size_t hash = rehash_str(strs[i], NULL);
		struct pht_iter it;
		int got = 0;
		for(void *cand = pht_firstval(&ht, &it, hash);
			cand != NULL; cand = pht_nextval(&ht, &it, hash))
		{
			//diag("after: str=`%s', cand=`%s'", strs[i], (const char *)cand);
			if(streq(cand, strs[i])) {
				got++;
				pht_delval(&ht, &it);
			}
		}
		if(got != 1) {
			diag("failed to clean up `%s': got=%d", strs[i], got);
			clean_ok = false;
		}
	}
	ok1(clean_ok);
	ok1(pht_count(&ht) == 0);

	pht_clear(&ht);
	pass("didn't crash in pht_clear()");

	return exit_status();
}
