# Multi-Threaded HTTP Proxy with Sharded LRU Cache

A C++17 HTTP/1.1 forward proxy built to hold 10,000 concurrent connections on 14
cores: one epoll thread, a fixed worker pool, and a sharded LRU cache in front of
the origin.

Every performance number in this README was measured by the harness in this
repository. See **[RESULTS.md](RESULTS.md)** for the full output, the test
environment, and the cases where the numbers are worse than they look.

```
                        +---------------------------------------------------+
   10,000 clients ----->| 1 epoll thread    EPOLLIN|EPOLLET|EPOLLONESHOT    |
                        | notices readiness, does no work                   |
                        +--------------------------+------------------------+
                                                   | push
                        +--------------------------v------------------------+
                        | task queue      mutex + condition_variable        |
                        | measured peak depth: 6,071                       |
                        +--------------------------+------------------------+
                                                   | pop
                        +--------------------------v------------------------+
                        | 14 workers  (= core count)                       |
                        |  parse -> ACL -> route -> cache -> serve -> log   |
                        +---------+-------------------------+---------------+
                                  | HIT 323us               | MISS 4,160us
                            +-----v-----+             +-----v-----+
                            |  16-way   |             |  origin   |
                            |  sharded  |             |  servers  |
                            |    LRU    |             +-----------+
                            +-----------+
```

## Headline measurements

| | |
|---|---|
| Concurrent connections | **10,000 / 10,000** established and held, 0 dropped (server's own `peak=10000`) |
| Throughput, keep-alive | **40,505 req/s** @ 400 threads, 0 failures, p99 24 ms |
| Throughput, new conn per request | **10,472 req/s** @ 200 threads, 0 failures |
| Cache hit rate | **92.1%** at Zipf alpha=1.2 with cache = 40% of working set |
| Cache latency win | **12.9x** -- 323 us hit vs 4,160 us miss |
| Sharding | 64 shards: -0.11 pp hit rate for **5.09x** concurrent throughput |
| Tests | 106 HTTP/ACL assertions + ThreadSanitizer-clean cache suite, 0 failures |

Client and server share one 14-core box, so throughput figures are lower bounds.

## Build and run

```bash
make all                 # server, origin, benchmark, concurrency test, unit tests
make test                # unit tests only
./run_benchmarks.sh      # sections 1-4 of RESULTS.md  (~10 min)
./run_concurrency.sh     # the 10k-connection test     (~4 min)
./run_leakcheck.sh       # connection-lifecycle leak check (~3 min)
make tsan                # ThreadSanitizer build of the cache tests
```

Run the proxy:

```bash
./server_phase5 8080 --acl access_control.conf --log access.log
```

| Flag | Meaning |
|---|---|
| `--workers <n>` | pool size (default: `hardware_concurrency`) |
| `--cache <n>` | total cache entries (default 2000) |
| `--shards <n>` | cache shards, rounded to a power of 2 (default 16) |
| `--acl <file>` | access-control config; a parse error **fails startup** |
| `--log <file>` | access log path (default stdout) |
| `--log-immediate` | flush per line instead of buffering (audit mode) |
| `--no-stats` | silence the periodic stats line |

## Design decisions, and why

**epoll over select/poll.** `select`/`poll` hand the kernel the whole fd set on
every call and it scans all of them: O(n). At 10,000 fds you rescan 10,000 to find
3 ready. `epoll` keeps the interest set in the kernel and returns only ready fds:
O(ready). (`select` also caps at `FD_SETSIZE`, usually 1024.)

**Edge-triggered clients, level-triggered listener.** ET means fewer syscalls but
obliges the handler to drain until `EAGAIN` -- read once and leave bytes buffered
and you are never notified again, so the connection hangs with data in it. The
listener stays level-triggered because being re-notified while the accept queue is
non-empty is exactly the desired behaviour.

**`EPOLLONESHOT`.** Two threads touch every connection: the epoll thread that
notices readiness, and the worker that handles it. Without one-shot a second
readiness event can arrive mid-request, two threads race on the same buffer, and
both may `close()` the fd. One-shot makes the kernel disarm the fd on delivery, so
exactly one thread owns a connection at a time -- single-owner semantics without a
per-connection lock. The worker re-arms with `EPOLL_CTL_MOD`.

**Thread pool over thread-per-connection.** 10,000 threads is ~80 GB of stack
address space and a scheduler with 10,000 runnable entities for 14 cores.
Connections are idle most of their lives. The queue absorbs bursts (6,071 deep at
peak) instead of the process spawning threads. `server_phase2.cpp` keeps the
thread-per-connection version deliberately, as the wall this design avoids.

**`std::list` + `unordered_map` for the LRU.** `list::splice` moves a node to the
front in O(1) *and does not invalidate iterators* -- which matters because the map
stores iterators into the list. A `vector` would invalidate them on every move and
memmove O(n) elements per access. The iterator-stability guarantee is the reason,
not the "list-ness".

**A plain mutex, not `shared_mutex`.** An LRU `get()` **mutates** recency order, so
it needs the exclusive path regardless; a reader-writer lock would be decoration.
Read concurrency is recovered the only way that actually works -- by having fewer
contended locks. Hence 16 shards.

**Sharding is a stated approximation.** Per-shard eviction means LRU is no longer
globally exact: a globally-hot key can be evicted from a busy shard while a colder
key survives in a quiet one. Measured cost 0.11 pp of hit rate for 5.09x
throughput. The mixing step in `shard_for()` is load-bearing -- `std::hash<int>` is
often the identity function, so masking its low bits without mixing first sends
every key to shard 0 and you ship a 1-shard cache without noticing.

**Access control runs before the cache.** A denial must cost nothing, and a cached
200 for `/admin/*` could otherwise be replayed to an unauthorised client. Rules are
first-match-wins so "allow these, deny everything else" is two lines. A malformed
config **fails startup** rather than degrading to allow-all.

**Only safe responses are cached.** GET/HEAD, status 200, no `Authorization` on the
request, no `Set-Cookie` / `no-store` / `private` on the response. A cached
`Set-Cookie` is one user's session handed to another.

**Upstream reads follow the message framing.** `Content-Length`, chunked, or
close-delimited -- not "read until EOF", which stalls for the whole socket timeout
on every request against a keep-alive origin. The chunked path walks chunk sizes
rather than searching for the terminator bytes, because those bytes can appear
inside chunk data and searching would truncate the body and then cache the
truncation.

**Buffered access logging.** One line per request, always -- hit, miss, denial, or
malformed. Flushed every 100 ms or 64 KB, because an `fflush` inside every
request's critical section measurably caps throughput (nginx buffers for the same
reason). `--log-immediate` restores per-line durability for audit use.

## Files

```
lru_cache.hpp          LRUCache + ShardedLRUCache -- the ONLY copy; server and tests share it
http.hpp               parser, response framing, cacheability, routing, ACL, access log
server_phase5.cpp      the proxy
origin_server.cpp      test origin, so the cache path is measured against a real server
benchmark.cpp          load generator: Zipf, keep-alive, percentiles, source-IP rotation
concurrency_test.cpp   opens N connections, holds them, verifies they are still usable
test_http.cpp          106 assertions on parsing / routing / access control
test_lru_cache.cpp     cache correctness + hit-rate and sharding measurements
access_control.conf    sample policy
run_benchmarks.sh      reproduces RESULTS.md sections 1-4
run_concurrency.sh     reproduces RESULTS.md section 5
run_leakcheck.sh       fd/memory leak check over the connection lifecycle
RESULTS.md             every measured number, with its configuration

server_base_version.cpp   phase 1: single-threaded accept loop
server_phase2.cpp         phase 2: thread per connection (the wall)
server_phase3.cpp         phase 3: epoll + thread pool
server_phase4.cpp         phase 4: + LRU cache
```

## Access control config

```
default allow

deny  method connect          # a tunnel is opaque: cannot inspect, cache, or log it
deny  host   blocked.example.com
deny  host   *.doubleclick.net
deny  path   /admin/*
route origin.test 127.0.0.1 19000
```

Patterns: `*`, `*.suffix`, `prefix*`, or an exact literal. First match wins; if
nothing matches, `default` applies.

## Access log format

```
2026-07-30T16:36:34.338Z 127.0.0.1 "GET /resource/1 HTTP/1.1" host=127.0.0.1:19000 \
  upstream=127.0.0.1:19000 status=200 bytes=624 cache=HIT acl=ALLOW rule="default" dur_us=323
```

`cache` is one of `HIT` / `MISS` / `BYPASS`; `rule` records *which* ACL rule
matched, so a 403 is auditable rather than mysterious.

## Known limitations

- **No HTTPS.** `CONNECT` is explicitly denied rather than half-supported, since a
  tunnel defeats all three reasons this proxy exists (cache, policy, observe).
- **No cache stampede protection.** Concurrent misses on the same key each fetch
  upstream -- measured at 18.6% overfetch (1,186 upstream fetches for a 1,000-URL
  working set that should have needed 1,000). Needs a per-key in-flight map.
- **No TTL / `max-age` enforcement on read.** Entries leave only by LRU pressure.
- **Upstream connections are not pooled** -- every miss pays a fresh handshake.
- **On `EMFILE` the accept loop retries next iteration** rather than backing off,
  which would spin under fd exhaustion.
- **HTTP/1.1 only**, no pipelining beyond serving buffered requests in order.
