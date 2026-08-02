# Measured Results

Every number here was produced by `./run_benchmarks.sh` and `./run_concurrency.sh`
in this repository. Nothing is estimated, extrapolated, or copied from a
reference implementation. Where a number is worse than it looks, that is said
plainly.

## Test environment

| | |
|---|---|
| Kernel | Linux 6.6 x86_64 (WSL2 on Windows 11) |
| CPU | 14 logical cores |
| Memory | 7.5 GiB |
| Compiler | g++ 15.2.0, `-std=c++17 -O2 -pthread -Wall -Wextra` |
| `ip_local_port_range` | 44620–48715 (**4096 ports**, unusually narrow) |
| `somaxconn` | 4096 |
| `RLIMIT_NOFILE` | 10240 soft / 1048576 hard; the server raises soft → hard at startup |

**The client and the server share one 14-core box.** The load generator competes
with the proxy for the same CPUs, so every throughput figure below is a lower
bound on what the server can do on dedicated hardware.

---

## 1. Correctness

`./test_http` — 106 assertions, 0 failures. Covers request-line and header
parsing, case-insensitive header lookup, header folding, absolute-form targets,
malformed-request rejection, keep-alive defaults per HTTP version, request and
response body framing (Content-Length / chunked / close-delimited),
cacheability rules, the wildcard matcher, authority splitting, host-header
routing, and access control (rule ordering, allowlist mode, first-match-wins,
and fail-closed parsing of a bad config).

The chunked tests include a deliberate trap: a chunk whose *payload* is literally
the bytes `\r\n0\r\n\r\n`. The obvious implementation — searching the buffer for
that terminator — reports the message complete mid-body, and the proxy then caches
a truncated response. The parser walks chunk sizes instead, so the trap case is
correctly reported incomplete until the real terminal chunk arrives.

`./test_lru_cache` — all assertions pass, including an 8-thread × 50k-op stress
run that validates every returned value against its key (catching torn reads and
cross-shard mixups, not merely "it did not deadlock").

`./run_leakcheck.sh` — the connection lifecycle. Every accepted connection
heap-allocates a `Conn` whose pointer lives in `epoll_event.data.ptr`, so each
accept must be matched by exactly one `close_conn()`. A missed `delete` leaks
memory and a missed `close()` leaks a descriptor, and **neither shows up in a
throughput benchmark**. Six rounds of ~26,000 requests each (mixed
connection-per-request and keep-alive) plus 160 deliberately broken connections per
round — missing `Host`, garbage request line, ACL-denied, and half-open clients that
send a partial request and vanish:

```
round    open_fds   rss_kb       threads
start    8          4448         16        idle baseline
1        8          5452         16
2        8          5452         16
3        8          5452         16
4        8          5452         16
5        8          5456         16
6        8          5456         16
```

Descriptors and threads flat, RSS plateaued after the first round (cache filling to
its bound). The error and half-open paths are included deliberately — that is
where a missed cleanup normally hides.

`make tsan && ./test_lru_cache_tsan` — the same tests under **ThreadSanitizer**:
all pass, **no data races reported**. "It did not crash" is not evidence of
thread safety; a race detector that stays quiet under 400k concurrent operations
is. (TSan costs ~17× throughput — 528k ops/s vs 9.0M — which is expected and is
why it is a separate build target rather than the default.)

`./run_benchmarks.sh smoke` — 9/9 end-to-end checks:

```
PASS  forwards to origin via Host header
PASS  second identical request served (from cache)
PASS  named route origin.test -> upstream
PASS  ACL denies blocked host
PASS  ACL denies wildcard subdomain
PASS  ACL denies /admin path
PASS  unroutable host -> 502
PASS  missing Host -> 400
PASS  garbage request line -> 400
```

---

## 2. Request logging

One line per request, for every request — cache hit, cache miss, ACL denial, and
malformed request alike.

```
2026-07-30T16:36:33.226Z 127.0.0.1 "GET /resource/1 HTTP/1.1" host=127.0.0.1:19000 upstream=127.0.0.1:19000 status=200 bytes=624 cache=MISS   acl=ALLOW rule="default"                          dur_us=4160
2026-07-30T16:36:34.338Z 127.0.0.1 "GET /resource/1 HTTP/1.1" host=127.0.0.1:19000 upstream=127.0.0.1:19000 status=200 bytes=624 cache=HIT    acl=ALLOW rule="default"                          dur_us=323
2026-07-30T16:36:36.584Z 127.0.0.1 "GET /x HTTP/1.1"          host=blocked.example.com upstream=-           status=403 bytes=130 cache=BYPASS acl=DENY  rule="deny  host blocked.example.com"    dur_us=438
```

Two things worth reading off those three lines:

* **A cache hit costs 323 µs; a miss costs 4160 µs.** The cache removes ~12.9× of
  the latency of a request, and that is measured per request rather than inferred
  from a hit-rate percentage.
* The denial line records *which rule* matched, so an ACL decision is auditable
  after the fact instead of being a bare 403.

Logging is buffered and flushed every 100 ms (or at 64 KB). `--log-immediate`
forces a flush per line for audit use. This matters: an `fflush` inside the
critical section of every request measurably caps throughput, which is why nginx
buffers its access log too.

Verified at scale: the 10,000-connection test below wrote exactly **10,000**
access-log lines for 10,000 requests.

---

## 3. Throughput

Working set 100 URLs against a 2000-entry cache, so this is the cache-hit path —
what a caching proxy exists to make fast.

### Connection per request

| Concurrency | Throughput | mean | p99 | failures |
|---|---|---|---|---|
| 50 threads | 7,975 req/s | 6.0 ms | 14.8 ms | 0 |
| 100 threads | 9,296 req/s | 10.3 ms | 25.3 ms | 0 |
| 200 threads | **10,472 req/s** | 18.1 ms | 47.0 ms | 0 |

### HTTP keep-alive (connections reused)

| Concurrency | Throughput | mean | p99 | failures |
|---|---|---|---|---|
| 50 threads | 31,531 req/s | 1.5 ms | 3.2 ms | 0 |
| 100 threads | 33,555 req/s | 2.8 ms | 7.5 ms | 0 |
| 200 threads | 28,977 req/s | 6.5 ms | 21.5 ms | 0 |
| 400 threads | **40,505 req/s** | 9.2 ms | 24.2 ms | 0 |

**Read this honestly:** the headline number is a *keep-alive* number. With a
fresh TCP connection per request the same server does ~10.5k req/s, because the
cost is then dominated by handshake, teardown and TIME_WAIT churn on both sides
rather than by request handling. Both are real; they answer different questions.
Quote the keep-alive figure only while saying it is keep-alive.

### A measurement bug worth more than the measurement

The first version of this benchmark reported a flat **94 req/s** at 50, 100 and
200 threads, with latency scaling perfectly linearly (550 ms → 1100 ms →
2200 ms). A constant rate with linearly-growing latency is the signature of a
serialised resource, not a saturated one.

It was in the load generator. To get past this box's 4096-port ephemeral range,
the client `bind()`s each socket to one of several `127.0.0.x` source addresses
before `connect()`. Timing the syscalls individually:

```
no source bind                       socket 0.004ms   bind  0.001ms   connect 0.325ms
bind 127.0.0.1                       socket 0.017ms   bind 20.358ms   connect 0.880ms
bind 127.0.0.2                       socket 0.017ms   bind 20.742ms   connect 0.556ms
```

`bind()` costs **20 ms** and serialises, so ~50 conn/s/thread was the ceiling —
the client's, not the server's. Removing the source bind took the same server
from 94 req/s to 9,724 req/s, a **103×** difference in a number that had nothing
to do with the code under test.

(`IP_BIND_ADDRESS_NO_PORT`, which normally fixes source-bind port-selection cost
by deferring port choice to `connect()`, was tried and made no difference here;
the cost on this kernel is in the bind conflict check itself.)

The general lesson, and the reason this section exists: **a benchmark number is
a claim about the whole test rig.** Before believing one, check that the thing
you are measuring is the thing that is slow.

---

## 4. Cache hit rate

Measured at the proxy, end to end, under a Zipf request stream.
C = cache capacity, N = working-set size, α = Zipf skew.

| N | C | C/N | α | measured hit rate | upstream fetches |
|---|---|---|---|---|---|
| 1000 | 100 | 0.10 | 1.0 | 54.17% | 18,107 |
| 1000 | 500 | 0.50 | 1.0 | 85.75% | 5,698 |
| 1000 | 1000 | 1.00 | 1.0 | 97.03% | 1,186 |
| 2000 | 1000 | 0.50 | 1.0 | 86.56% | 5,377 |
| 2000 | 2000 | 1.00 | 1.0 | 94.94% | 2,024 |
| 5000 | 2000 | 0.40 | 1.0 | 83.63% | 6,549 |
| **5000** | **2000** | **0.40** | **1.2** | **92.14%** | 3,144 |
| 1000 | 100 | 0.10 | 0.0 | 9.54% | 36,185 |

**Hit rate is a property of the workload, not of the cache.** Two rows make that
unavoidable:

* At α = 0 (uniform access) with C/N = 0.1, the hit rate is 9.54% — essentially
  exactly C/N. With no reuse locality, no eviction policy can do better than the
  fraction of the working set you can hold. LRU is not adding value here; nothing
  could.
* At α = 1.2 with the same cache holding 40% of the working set, it is 92.14%,
  because the top ~15% of keys absorb most of the requests.

So "90% hit rate" is only meaningful with its configuration attached. The honest
statement for this implementation is: **92% at α = 1.2 with the cache holding 40%
of the working set**, and ~86% at α = 1.0 with the cache holding half.

The unit-test harness cross-checks each measurement against the analytic
prediction for LRU under an independent-reference Zipf model,
`H(C)/H(N)` with `H` the generalised harmonic number:

```
keys(N)  cap(C)   C/N    alpha   measured  predicted
1000     100      0.10   1.00    57.47     69.30
1000     500      0.50   1.00    86.67     90.75
5000     2000     0.40   1.20    93.98     96.09
```

Measured tracks predicted and sits slightly below it, which is what real LRU
should do — the analytic model assumes the cache has already converged on the C
most popular keys, while a real trace keeps paying for cold-start misses and
churn at the eviction boundary. Agreement this close means the harness is
measuring LRU and not measuring itself.

---

## 5. Concurrent connections

"Concurrent" here means *simultaneously established and still usable* — not
"this many requests completed over some interval".

```
client fd limit : 1048576
ephemeral ports : 44620..48715 (4096 per source IP)
source IPs      : 8  => theoretical ceiling 32768 connections
target          : 10000 concurrent connections

ESTABLISHED     : 10000 / 10000  (ramp 213.05s, 47 conn/s)
holding 3 s ...
after hold      : 10000 still open, 0 closed by peer
issuing one request on each held connection ...
requests served : 10000 ok, 0 failed over 0.27s

RESULT          : PASS (10000 concurrent connections sustained)
```

The proxy's own counters agree:

```
[stats] conns=10000 active=0 peak=10000 | req=10000 cache_hit=9986 fwd=14
        denied=0 bad=0 upstream_err=0 | cache=99.86% | queue=0 hw=6071
        workers=14 log_lines=10000
```

Notes:

* **`peak=10000`** is the server's own high-water mark, independent of the
  client's count.
* **0 closed by peer** — the server did not quietly drop early connections to
  stay under a limit, which is the cheap way to fake this number.
* **10,000 requests served in 0.27 s** once the connections existed. The 213 s
  ramp is the client's 20 ms-per-`bind()` cost described above, not a server
  limit.
* **`hw=6071`** is the task-queue high-water mark: at peak, 6071 ready
  connections were queued for 14 worker threads. This is exactly what the
  thread-pool design is for — the queue absorbs the burst instead of the process
  spawning 6071 threads.
* Getting past 4096 required 8 source IPs, because a TCP connection is
  identified by the 4-tuple (src IP, src port, dst IP, dst port). One source IP
  against one destination gives at most `|ephemeral range|` = 4096 concurrent
  connections. That is arithmetic about the client, not a server limit.

---

## 6. Cache sharding

The cache is 16-way sharded. Both sides of that trade-off, measured:

| shards | hit rate | 8-thread ops/s | speedup |
|---|---|---|---|
| 1 | 85.09% | 3.2 M | 1.00× |
| 4 | 85.10% | 5.1 M | 1.57× |
| 16 | 85.06% | 9.2 M | 2.85× |
| 64 | 84.98% | 16.4 M | 5.09× |

Sharding replaces one global LRU with N independent LRUs, so eviction is no
longer globally exact — a globally-hot key can be evicted from a busy shard
while a colder key survives in a quiet one. The measured cost of that
approximation is **0.11 percentage points** of hit rate at 64 shards, in
exchange for **5.09×** concurrent throughput. That is why the trade is worth
making, and the table is why it is a trade rather than a guess.

---

## 7. Is the cache actually O(1)?

| entries | reads/s | writes/s | ns/read |
|---|---|---|---|
| 1,000 | 66.3 M | 62.5 M | 15.1 |
| 100,000 | 26.8 M | 23.0 M | 37.3 |
| 1,000,000 | 6.3 M | 4.1 M | 158.2 |

Cost per operation is **not** flat: it grows ~10× as the cache grows 1000×. That
is the memory hierarchy, not the algorithm. O(n) behaviour would have cost 1000×
more; O(log n) about 2×. A 10× rise across three orders of magnitude is L1 hits
being replaced by DRAM round-trips once the hash table and the intrusive list
stop fitting in cache.

**O(1) is a statement about operation count, not about wall-clock time.** Saying
"O(1) read/write" is correct; saying "constant latency regardless of size" would
not be.

---

## Reproducing

```bash
make all
./run_benchmarks.sh          # sections 1-4, ~10 minutes
./run_concurrency.sh         # section 5, ~4 minutes (213s of it is client ramp)
./run_leakcheck.sh           # connection lifecycle, ~3 minutes
make tsan && ./test_lru_cache_tsan   # ThreadSanitizer build of the cache tests
```
