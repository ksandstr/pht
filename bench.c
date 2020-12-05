
/* read in distinct words from /usr/share/dict/words, add them to a hash
 * table, lookup each word, and tally up the rdtsc latency of each operation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <ccan/htable/htable.h>
#include <ccan/hash/hash.h>
#include <ccan/tally/tally.h>
#include <ccan/str/str.h>
#include <ccan/array_size/array_size.h>
#include <ccan/read_write_all/read_write_all.h>
#include <ccan/darray/darray.h>

#include "pht.h"


struct ht_ops {
	const char *name;
	size_t size, iter_size;
	void (*init)(void *, size_t (*rehash)(const void *, void *), void *);
	void (*clear)(void *ht);
	bool (*add)(void *ht, size_t hash, const void *key);
	bool (*del)(void *ht, size_t hash, const void *key);
	void *(*firstval)(const void *, void *, size_t);
	void *(*nextval)(const void *, void *, size_t);
};


struct bmctx {
	void *ht;
	const struct ht_ops *ops;
	const char *wordbuf;
	size_t n_words;
	char name[40];
};


struct benchmark {
	const char *name;
	void (*run)(struct bmctx *ctx, int writefd);
	void (*report)(struct bmctx *ctx, int readfd);
};


static size_t n_cmp_str = 0;


static inline uint64_t rdtsc(void)
{
	uint64_t tsc;
#if defined(__i386__)
	asm volatile ("rdtsc" : "=A" (tsc));
#elif defined(__amd64__)
	uint32_t lo, hi;
	asm volatile ("rdtsc" : "=a" (lo), "=d" (hi));
	tsc = (uint64_t)hi << 32 | lo;
#else
#error "implement me for your architecture plz"
#endif
	return tsc;
}


static size_t rehash_str(const void *ptr, void *priv) {
	return hash(ptr, strlen(ptr), (uintptr_t)priv);
}


static bool cmp_str(const void *cand, void *key) {
	n_cmp_str++;
	return streq(cand, key);
}


static inline void *ht_ops_get(
	const struct ht_ops *ops, void *ht,
	void *iter, size_t hash,
	bool (*cmpfn)(const void *cand, void *key), const void *key)
{
	for(void *cand = (*ops->firstval)(ht, iter, hash);
		cand != NULL; cand = (*ops->nextval)(ht, iter, hash))
	{
		if((*cmpfn)(cand, (void *)key)) return cand;
	}
	return NULL;
}


static void send_array(int fd, size_t count, const uint32_t *samples)
{
	bool ok = write_all(fd, &count, sizeof count);
	if(!ok) {
		perror("write (header)");
		abort();
	}
	ok = write_all(fd, samples, sizeof *samples * count);
	if(!ok) {
		perror("write (data)");
		abort();
	}
}


static uint32_t *receive_array(int fd, size_t *done_p)
{
	bool ok = read_all(fd, done_p, sizeof *done_p);
	if(!ok) {
		perror("read_all (header)");
		abort();
	}
	uint32_t *samples = malloc(sizeof *samples * *done_p);
	if(samples == NULL) abort();
	ok = read_all(fd, samples, sizeof *samples * *done_p);
	if(!ok) {
		perror("read_all (data)");
		abort();
	}
	return samples;
}


static void print_tallied(
	FILE *stream, const char *header,
	size_t count, const uint32_t *samples)
{
	struct tally *t = tally_new(count * 5 / 4);
	for(size_t j=0; j < count; j++) tally_add(t, samples[j]);
	fprintf(stream, "%s: num=%zu, min=%zd, max=%zd, mean=%zd\n",
		header, tally_num(t), tally_min(t), tally_max(t), tally_mean(t));
	size_t err, val = tally_approx_median(t, &err);
	fprintf(stream, "\tmedian=%zd (+-%zu)", val, err);
	val = tally_approx_mode(t, &err);
	fprintf(stream, ", mode=%zd (+-%zu)", val, err);
	ssize_t over, total = tally_total(t, &over);
	fprintf(stream, ", total=%zd:%zd\n", over, total);
	free(t);
}


/* benchmark back-to-back adds. */
static void run_add(struct bmctx *ctx, int writefd)
{
	const struct ht_ops *ops = ctx->ops;
	const size_t n_words = ctx->n_words;

	uint32_t *samples = malloc(sizeof *samples * n_words);
	if(samples == NULL) abort();

	/* precompute rehash, strlen for proper b2b measurements */
	darray(const char *) strs = darray_new();
	darray(size_t) hashes = darray_new();
	const char *s = ctx->wordbuf;
	while(*s != '\0') {
		darray_push(strs, s);
		darray_push(hashes, rehash_str(s, NULL));
		s += strlen(s) + 1;
	}
	assert(strs.size == n_words);

	for(size_t i=0; i < n_words; i++) {
		uint64_t start = rdtsc();
		bool ok = (*ops->add)(ctx->ht, hashes.item[i], strs.item[i]);
		if(!ok) abort();
		uint64_t end = rdtsc();
		samples[i] = end - start;
	}

	send_array(writefd, n_words, samples);
	free(samples);
	darray_free(strs);
	darray_free(hashes);
}


static void report_add(struct bmctx *ctx, int readfd)
{
	size_t done;
	uint32_t *samples = receive_array(readfd, &done);
	print_tallied(stdout, ctx->name, done, samples);
	free(samples);
}


/* add all words to the hash table, then do back-to-back gets of every word
 * and a nonexistent word derived therefrom. result is three arrays: rdtsc
 * latency of positive and negative form, and total # of comparisons on the
 * positive case.
 */
static void run_get(struct bmctx *ctx, int writefd)
{
	const struct ht_ops *ops = ctx->ops;
	const size_t n_words = ctx->n_words;

	uint32_t *cyc_pos = malloc(sizeof(uint32_t) * n_words),
		*cyc_neg = malloc(sizeof(uint32_t) * n_words),
		*ncmp = malloc(sizeof(uint32_t) * n_words);
	if(cyc_pos == NULL || cyc_neg == NULL || ncmp == NULL) abort();

	/* prepare the hash table. */
	for(const char *s = ctx->wordbuf; *s != '\0'; s += strlen(s) + 1) {
		size_t hash = rehash_str(s, NULL);
		bool ok = (*ops->add)(ctx->ht, hash, s);
		if(!ok) abort();
	}

	/* actual benchmarking */
	void *iter = malloc(ctx->ops->iter_size);
	if(iter == NULL) abort();
	size_t n = 0;
	for(const char *s = ctx->wordbuf; *s != '\0'; s += strlen(s) + 1, n++) {
		size_t hash = rehash_str(s, NULL);
		char oth[100];
		snprintf(oth, sizeof oth, "X%sX", s);
		size_t oth_hash = rehash_str(oth, NULL);

		n_cmp_str = 0;
		uint64_t start = rdtsc();
		void *val = ht_ops_get(ops, ctx->ht, iter, hash, &cmp_str, s);
		uint64_t end = rdtsc();
		assert(val != NULL && streq(val, s));
		cyc_pos[n] = end - start;

		start = rdtsc();
		val = ht_ops_get(ops, ctx->ht, iter, oth_hash, &cmp_str, oth);
		end = rdtsc();
		assert(val == NULL);
		cyc_neg[n] = end - start;
		ncmp[n] = n_cmp_str;
	}

	send_array(writefd, n_words, cyc_pos); free(cyc_pos);
	send_array(writefd, n_words, cyc_neg); free(cyc_neg);
	send_array(writefd, n_words, ncmp); free(ncmp);
}


static void report_get(struct bmctx *ctx, int readfd)
{
	static const char *names[] = { "cyc+", "cyc-", "#cmp" };
	for(int i=0; i < ARRAY_SIZE(names); i++) {
		size_t length;
		uint32_t *data = receive_array(readfd, &length);
		assert(length == ctx->n_words);
		char hdr[100];
		snprintf(hdr, sizeof hdr, "%s/%s", ctx->name, names[i]);
		print_tallied(stdout, hdr, length, data);
		free(data);
	}
}


static void run_mixed(struct bmctx *ctx, int writefd)
{
	const struct ht_ops *ops = ctx->ops;
	const size_t n_words = ctx->n_words;

	uint32_t *cyc_add = malloc(sizeof(uint32_t) * n_words);
	darray(uint32_t) cyc_del = darray_new();
	if(cyc_add == NULL) abort();

	size_t n = 0;
	const char *d = ctx->wordbuf;
	for(const char *s = ctx->wordbuf; *s != '\0'; s += strlen(s) + 1) {
		size_t hash = rehash_str(s, NULL);
		uint64_t start = rdtsc();
		bool ok = (*ops->add)(ctx->ht, hash, s);
		uint64_t end = rdtsc();
		cyc_add[n++] = end - start;
		if(!ok) abort();

		if(n % 3 == 0) {
			assert(*d != '\0');
			hash = rehash_str(d, NULL);
			start = rdtsc();
			ok = (*ops->del)(ctx->ht, hash, d);
			end = rdtsc();
			darray_push(cyc_del, (uint32_t)(end - start));
			if(!ok) {
				if(ops->del == (typeof(ops->del))&pht_del) pht_check(ctx->ht, "missed del");
				printf("%s: missed del on `%s'\n", __func__, d);
				abort();
			}
			d += strlen(d) + 1;
		}
	}

	send_array(writefd, n_words, cyc_add); free(cyc_add);
	send_array(writefd, cyc_del.size, cyc_del.item);
	darray_free(cyc_del);
}


/* TODO: de-copypasta this one wrt report_get, report_add */
static void report_mixed(struct bmctx *ctx, int readfd)
{
	static const char *names[] = { "add", "del" };
	for(int i=0; i < ARRAY_SIZE(names); i++) {
		size_t length;
		uint32_t *data = receive_array(readfd, &length);
		assert(length == ctx->n_words || i > 0);
		char hdr[100];
		snprintf(hdr, sizeof hdr, "%s/%s", ctx->name, names[i]);
		print_tallied(stdout, hdr, length, data);
		free(data);
	}
}


static void run_benchmark_with_ops(
	const struct benchmark *bm, const struct ht_ops *ops,
	int pipefds[static 2], struct bmctx *bc, bool nofork)
{
	/* disable coredumps from the parent process. */
	int n = setrlimit(RLIMIT_CORE, &(struct rlimit){ 0, RLIM_INFINITY });
	if(n != 0) perror("setrlimit (disable coredumps)");

	int child = fork();
	if((child == 0) == !nofork) {
		/* reënable them in the benchmark side, regardless of who it is. */
		n = setrlimit(RLIMIT_CORE,
			&(struct rlimit){ RLIM_INFINITY, RLIM_INFINITY });
		if(n != 0) perror("setrlimit (reënable coredumps)");

		close(pipefds[0]);
		bc->ht = malloc(ops->size);
		if(bc->ht == NULL) abort();
		(*ops->init)(bc->ht, &rehash_str, NULL);
		(*bm->run)(bc, pipefds[1]);
		(*ops->clear)(bc->ht);
		free(bc->ht);
		close(pipefds[1]);
	} else {
		close(pipefds[1]);
		(*bm->report)(bc, pipefds[0]);
		close(pipefds[0]);
	}
	if(child == 0) exit(EXIT_SUCCESS);
	else {
		int st, n = waitpid(child, &st, 0);
		if(n != child) {
			perror("waitpid");
			abort();
		}
	}
}


int main(int argc, char *argv[])
{
	static const struct option opts[] = {
		/* "no-fork" runs the results-collecting on the child side, which
		 * means that next runs of the benchmark are affected by the detritus
		 * of the ones before. it doesn't produce comparable benchmark
		 * results.
		 */
		{ "no-fork", no_argument, 0, 'n' },
		{ "words", required_argument, 0, 'w' },
		{ },
	};
	const char *words_opt = "/usr/share/dict/words";
	bool nofork = false;
	for(;;) {
		int n = getopt_long(argc, argv, "nw:", opts, NULL);
		if(n < 0) break;
		switch(n) {
			case 'n': nofork = true; break;
			case 'w': words_opt = strdupa(optarg); break;
			default:
				fprintf(stderr, "unexpected n=%d (`%c') from getopt_long()\n",
					n, n);
				abort();
		}
	}

	FILE *words = fopen(words_opt, "r");
	if(words == NULL) {
		perror("fopen");
		return EXIT_FAILURE;
	}

	fseek(words, 0, SEEK_END);
	size_t length = ftell(words);
	fseek(words, 0, SEEK_SET);
	if(length < 10000) {
		fprintf(stderr, "length=%zu too small\n", length);
		return EXIT_FAILURE;
	}
	char *wordbuf = aligned_alloc(1024 * 1024, length + 2);
	if(wordbuf == NULL) {
		perror("malloc");
		abort();
	}

	char *s = wordbuf;
	size_t n_words = 0;
	while(fgets(s, (length + 2) - (s - wordbuf), words) != NULL) {
		int len = strlen(s);
		while(len > 0 && isspace(s[len - 1])) s[--len] = '\0';
		assert(len == strlen(s));
		if(len > 0) {
			//if(strends(s, "'s")) continue;
			s += len + 1;
			assert(s < wordbuf + length + 2);
			n_words++;
		}
	}
	*s++ = '\0';

	static const struct ht_ops variants[] = {
		{ .name = "pht",
		  .size = sizeof(struct pht), .iter_size = sizeof(struct pht_iter),
		  .init = (void *)&pht_init, .clear = (void *)&pht_clear,
		  .add = (void *)&pht_add, .del = (void *)&pht_del,
		  .firstval = (void *)&pht_firstval,
		  .nextval = (void *)&pht_nextval, },
		{ .name = "htable",
		  .size = sizeof(struct htable), .iter_size = sizeof(struct htable_iter),
		  .init = (void *)&htable_init, .clear = (void *)&htable_clear,
		  .add = (void *)&htable_add_, .del = (void *)&htable_del_,
		  .firstval = (void *)&htable_firstval_,
		  .nextval = (void *)&htable_nextval_, },
	};

	static const struct benchmark benchmarks[] = {
		{ .name = "add", .run = &run_add, .report = &report_add },
		{ .name = "get", .run = &run_get, .report = &report_get },
		{ .name = "mixed", .run = &run_mixed, .report = &report_mixed },
	};

	for(const struct benchmark *bm = &benchmarks[0];
		bm < &benchmarks[ARRAY_SIZE(benchmarks)]; bm++)
	{
		for(const struct ht_ops *ops = &variants[0];
			ops < &variants[ARRAY_SIZE(variants)]; ops++)
		{
			int fds[2], n = pipe(fds);
			if(n < 0) { perror("pipe"); abort(); }
			struct bmctx bc = { .ops = ops, .wordbuf = wordbuf,
				.n_words = n_words };
			snprintf(bc.name, sizeof bc.name, "%s[%s]", bm->name, ops->name);
			run_benchmark_with_ops(bm, ops, fds, &bc, nofork);
		}
	}

	free(wordbuf);
	fclose(words);

	return 0;
}
