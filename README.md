
tl;dr: Progressively rebuilt hash table. Interface similar to CCAN htable. Not
friendly to valgrind or libgc due to pointer munging. GPLv3-or-later.

A more thorough treatment of the design should appear here at a later date,
but for now just use the source. There are some tests and a big old benchmark
which produces meaningless results when not built under `-O2 -DNDEBUG` in
CFLAGS, or when run unlike how the script provided would.

The core idea, relayed by someone on Reddit years ago, is that of handling
hash table growth progressively to gain a superior maximum bound for adding
an entry to a hashed multiset. Due to cache and pipeline effects this also
amounts to 20-30% fewer total clock cycles spent in artificial "add"
benchmarks, while other statistics are somewhat worse due to an extra
indirection over CCAN htable. This comes at the expense of 1.5x space, the
correct tradeoff for interactive programs as of 2020, and more cycles spent in
some positive and all negative lookups to step through secondary tables.

The practical reason for this data structure is to demonstrate a low-latency
alternative to htables, so as to reduce knock-on sleeping in cases where a
mutex is contended more while htable runs a big loop. There is currently no
multi-threaded benchmark.

The fanciful reason is that even if my lock-free/wait-free hash table had a
sound migration mechanism (and it might not), its higher-level design is
nevertheless lacking in guarantees wrt space and time. So this project exists
also as a vehicle for exploring that, before lfht gets a proper refit, before
it is in turn applied in the transactional memory library.

Currently "pht" is mostly feature complete. Unless a significant new
optimization comes along, further development is limited to improving its test
and benchmarking suites, and its documentation.

The inter-table migration algorithm for this data structure, consisting of an
"inner" algorithm that avoids rehashing while migrating items and an "outer"
driver that increases cache utilization, is believed novel in its design and
implementation as of November 2020.

  -- Kalle A. Sandström <ksandstr@iki.fi>
