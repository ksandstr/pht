#!/bin/sh
set -e
[ -x ./bench ] || exit 1
# to overcome the vagaries of cross-CPU scheduling and its impact on cache
# access and things like that, we'll pin the benchmark to CPU#0.
exec taskset -c 0 ./bench
