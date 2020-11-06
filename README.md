
tl;dr: Progressively rebuilt hash table. Interface similar to CCAN htable. Not
friendly to valgrind or libgc due to pointer munging. GPLv3-or-later.

A more thorough treatment of the design should appear here at a later date,
but for now just use the source. There are some tests and a big old benchmark
which produces meaningful results only when built under `-O2 -DNDEBUG` in
CFLAGS, and run using the script provided.

The core idea, due to someone on Reddit years ago, is that of handling hash
table growth progressively to gain a far better maximum bound for adding an
entry to a hashed multiset. The statistical mode, mean, and median of clock
cycles spent per "add" are also improved, while the minimum is single digits
worse due to an extra indirection over CCAN htable. This comes at the expense
of 1.5x space (the correct tradeoff for all user-facing programs as of 2020)
and a need for some lookups to step through a second hash chain while a
secondary table remains.

The practical reason for this data structure is to demonstrate a low-latency
alternative to mutexed htables, so as to reduce knock-on sleeping in cases
where htable might run a big loop.

The fanciful reason is that even if my lock-free/wait-free hash table has a
sound migration mechanism (and it probably doesn't), its higher-level design
is nevertheless lacking in guarantees wrt space and time. So this project
exists also as a vehicle for exploring that, before lfht gets a proper refit,
before it gets applied in the transactional memory thing.

Further development, such as introduction of a smarter rehashing algorithm,
should put pht farther ahead of CCAN htable in the add benchmark, while
staying somewhat worse than parity on lookups and deletions.

  -- Kalle A. Sandström <ksandstr@iki.fi>
