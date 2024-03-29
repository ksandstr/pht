
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
#include <ccan/str/str.h>

#include "pht.h"


static bool cmp_verbose = false;


static size_t rehash_str(const void *p, void *priv) {
	return hash(p, strlen(p), (uintptr_t)priv);
}


static bool cmp_str(const void *cand, void *key) {
	if(cmp_verbose && !streq(cand, key)) {
		diag("%s: cand=`%s', key=`%s'", __func__,
			(const char *)cand, (char *)key);
	}
	return streq(cand, key);
}


static int ord_ptr(const void *a, const void *b) {
	return *(const void *const *)a - *(const void *const *)b;
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


static size_t key_count_all(
	const struct pht *ht,
	bool (*cmp)(const void *, void *), const void *key)
{
	size_t count = 0;
	struct pht_iter it;
	for(void *cand = pht_first(ht, &it);
		cand != NULL; cand = pht_next(ht, &it))
	{
		if((*cmp)(cand, (void *)key)) count++;
	}
	return count;
}


static bool same(const struct pht *pa, const struct pht *pb)
{
	if(pa == pb) return true;
	if(pht_count(pa) != pht_count(pb)) return false;
	size_t sz = pht_count(pa), n;
	void *a[sz], *b[sz];
	struct pht_iter it;
	n = 0;
	for(void *p = pht_first(pa, &it); p != NULL; p = pht_next(pa, &it)) {
		a[n++] = p;
	}
	assert(n == sz);
	qsort(a, sz, sizeof a[0], &ord_ptr);
	n = 0;
	for(void *p = pht_first(pb, &it); p != NULL; p = pht_next(pb, &it)) {
		b[n++] = p;
	}
	assert(n == sz);
	qsort(b, sz, sizeof b[0], &ord_ptr);
	for(size_t i=0; i < n; i++) {
		if(a[i] != b[i]) return false;
	}
	return true;
}


static bool disjoint(const struct pht *pa, const struct pht *pb)
{
	if(pa == pb) return false;
	size_t sa = pht_count(pa), sb = pht_count(pb), n;
	void *a[sa], *b[sb];
	struct pht_iter it;

	n = 0;
	for(void *p = pht_first(pa, &it); p != NULL; p = pht_next(pa, &it)) {
		a[n++] = p;
	}
	assert(n == sa);
	qsort(a, sa, sizeof a[0], &ord_ptr);

	n = 0;
	for(void *p = pht_first(pb, &it); p != NULL; p = pht_next(pb, &it)) {
		b[n++] = p;
	}
	assert(n == sb);
	qsort(b, sb, sizeof b[0], &ord_ptr);

	/* zip them up, but instead of storing equal values, return false when
	 * common items are found.
	 */
	size_t ia = 0, ib = 0;
	while(ia < sa && ib < sb) {
		if(a[ia] < b[ib]) ia++;
		else if(a[ia] > b[ib]) ib++;
		else return false;
	}

	return true;
}


int main(void)
{
	plan_tests(20);

	static const char *const strs[] = {
		"my ass-clap keeps alerting the bees!",
		"foo", "bar", "zot", "hoge", "lemon", "melon", "grape",
		"banana", "apple", "orange", "watermelon", "rhubarb",
		"parsnip", "barley", "maize", "rye", "flax", "quinoa",
		"tea", "coffee", "cocoa", "data", "datum", "datums",
		"mutex", "mutices", "mutexes", "gecko", "newt", "rothe",
		"iguana", "woodchuck", "oracle", "vlad", "rodney",
		"the wood nymph zaps a wand of death! -more-",

		"bean", "warp", "zonk", "awk", "sed", "grep",
		"trash", "junk", "guff", "dross", "garbo",
		"faff", "wank", "toss", "piffle", "drivel",
		"blather", "hogwash", "bunk", "balderdash", "hokum", "twaddle",

		"it's a man's life in the british dental association",
		"guitar", "violin", "cello", "bassoon", "tuba", "bagpipe",
		"mandolin", "piano", "saxophone", "kazoo", "otamatone",

		"cheese", "milk", "cream", "half-and-half", "soylent green",
		"bachelor chow", "catfood", "dogfood", "birdseed", "pellets",

		"ranarama", "super pipeline", "pitfall", "hektik", "commando",
		"solomon's key", "elite", "creatures", "grand monster slam", "wizball",
		"delta", "zaxxon", "uridium", "sanxion", "salamander", "krakout",
		"the way of the exploding fist", "blue max", "choplifter",
		"little computer people", "bagitman", "bozo's night out",

		"white", "black", "spanish", "yellow", "hot", "cold",
		"wet", "tight", "big", "bloody", "fat", "hairy",
		"smelly", "velvet", "silk", "naugahyde", "snappin'",
		"horse", "dog", "chicken", "fake", "apple pie",
		"slashed in half", "blown out",
	};
	assert(ARRAY_SIZE(strs) == 127);	/* because prime. */

	struct pht ht = PHT_INITIALIZER(ht, &rehash_str, NULL);
	ok1(pht_count(&ht) == 0);

	/* add strings. */
	bool adds_ok = true, counts_ok = true, counts_all_ok = true, total_ok = true;
	for(int i=0; i < ARRAY_SIZE(strs); i++) {
		size_t hash = rehash_str(strs[i], NULL);
		//diag("str=`%s', hash=%#zx", strs[i], hash);
		bool ok = pht_add(pht_check(&ht, NULL), hash, strs[i]);
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
				diag("[hashed] count=%zu for j=%d `%s' is wrong", ct, j, strs[j]);
				counts_ok = false;
			}

			ct = key_count_all(&ht, &cmp_str, strs[j]);
			if((j <= i && ct != 1) || (j > i && ct > 0)) {
				diag("[all] count=%zu for j=%d `%s' is wrong", ct, j, strs[j]);
				counts_all_ok = false;
			}
		}
	}
	pht_check(&ht, NULL);
	ok1(adds_ok);
	ok1(counts_ok);
	ok1(counts_all_ok);
	ok1(total_ok);
	ok1(pht_count(&ht) == ARRAY_SIZE(strs));

	/* make a copy and confirm its contents. */
	struct pht ht2 = PHT_INITIALIZER(ht2, &rehash_str, NULL);
	if(!ok1(pht_copy(&ht2, &ht))) {
		pht_init(&ht2, &rehash_str, NULL);
	}
	pht_check(&ht2, NULL);
	ok1(same(&ht, &ht2));

	/* delete ones at odd indexes in `ht', and even indexes in `ht2'. */
	bool dels_ok = true;
	int n_removed = 0;
	for(int i=0; i < ARRAY_SIZE(strs); i++) {
		struct pht *target = (i & 1) ? &ht : &ht2;
		bool ok = pht_del(pht_check(target, NULL),
			rehash_str(strs[i], NULL), strs[i]);
		if(!ok) dels_ok = false;
		else if(i & 1) n_removed++;
	}
	pht_check(&ht, NULL); pht_check(&ht2, NULL);
	ok1(dels_ok);
	ok1(pht_count(&ht) == ARRAY_SIZE(strs) - n_removed);
	ok1(pht_count(&ht2) == ARRAY_SIZE(strs) - pht_count(&ht));
	ok1(disjoint(&ht, &ht2));
	pht_clear(&ht2);

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
				pht_delval(pht_check(&ht, NULL), &it);
			}
		}
		if(got != 1) {
			diag("failed to clean up `%s': got=%d", strs[i], got);
			clean_ok = false;
		}
	}
	pht_check(&ht, NULL);
	ok1(clean_ok);
	ok1(pht_count(&ht) == 0);

	pht_clear(&ht);
	pass("didn't crash in pht_clear()");

	return exit_status();
}
