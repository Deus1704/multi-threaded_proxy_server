# Multi-Threaded Proxy Server with LRU Cache

A progressive implementation of a high-performance TCP/HTTP proxy server in C++.

## Architecture Evolution

```
Phase 1 (base)    Phase 2           Phase 3           Phase 4           Phase 5
┌─────────┐       ┌─────────┐       ┌─────────┐       ┌─────────┐       ┌─────────┐
│ Single  │  -->  │ Thread  │  -->  │ epoll + │  -->  │ + LRU   │  -->  │ + Full  │
│ Thread  │       │ per     │       │ Thread  │       │ Cache   │       │ Proxy   │
│         │       │ Conn    │       │ Pool    │       │         │       │         │
└─────────┘       └─────────┘       └─────────┘       └─────────┘       └─────────┘
```

### Phase Breakdown

| Phase | File | What's Added | Handles |
|-------|------|--------------|---------|
| 1 | `server_base_version.cpp` | Basic socket: bind→listen→accept→recv/send | 1 client |
| 2 | `server_phase2.cpp` | Thread per connection, HTTP responses | Multiple, wasteful |
| 3 | `server_phase3.cpp` | epoll event loop + thread pool + task queue | 10k+ concurrent |
| 4 | `server_phase4.cpp` | Thread-safe LRU cache (hashmap + linked list) | Reduced latency |
| 5 | `server_phase5.cpp` | HTTP parsing, upstream forwarding, stats | Real proxy |

## Key Components

### Thread Pool + Task Queue
```
                    ┌─────────────────────────────────────┐
                    │           Task Queue                │
                    │  ┌───┬───┬───┬───┬───┐             │
   epoll events --> │  │ T │ T │ T │ T │...│ <-- mutex   │
                    │  └───┴───┴───┴───┴───┘   + cond_var│
                    └──────────┬──────────────────────────┘
                               │
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
    ┌──────────┐         ┌──────────┐        ┌──────────┐
    │ Worker 1 │         │ Worker 2 │   ...  │ Worker N │
    └──────────┘         └──────────┘        └──────────┘
```

- Workers block on `condition_variable` until tasks arrive
- `notify_one()` wakes a single worker per task
- No thread-per-connection overhead

### LRU Cache (Thread-Safe)
```
    HashMap (O(1) lookup)
    ┌─────────────────────────────────────┐
    │ key1 ──> node_ptr                   │
    │ key2 ──> node_ptr                   │
    │ ...                                 │
    └─────────────────────────────────────┘
                    │
                    ▼
    Doubly Linked List (access order)
    ┌──────┐   ┌──────┐   ┌──────┐   ┌──────┐
    │ MRU  │◄─►│      │◄─►│      │◄─►│ LRU  │
    │      │   │      │   │      │   │evict │
    └──────┘   └──────┘   └──────┘   └──────┘
```

- `shared_mutex`: multiple readers OR one writer
- `get()`: move to front (O(1) via splice)
- `put()`: evict LRU if at capacity (O(1))

### epoll Event Loop
```
Level-triggered for accept (simple), Edge-triggered for clients (efficient)

while (true) {
    n = epoll_wait(...)
    for each event:
        if listener:
            accept all pending connections
            add to epoll (EPOLLIN | EPOLLET)
        else:
            remove from epoll (one-shot)
            dispatch to thread pool
}
```

## Build & Run

```bash
# Compile (requires C++17 for optional, shared_mutex)
g++ -std=c++17 -O2 -pthread server_phase5.cpp -o proxy
g++ -std=c++17 -O2 -pthread benchmark.cpp -o benchmark
g++ -std=c++17 -O2 -pthread test_lru_cache.cpp -o test_cache

# Run server
./proxy 8080

# Run tests
./test_cache

# Benchmark (against running server)
./benchmark -p 8080 -t 50 -n 200 -u 100
```

## Benchmark Results

### Cache Hit Rate (Zipf distribution)
| Unique URLs | Cache Size | Expected Hit Rate |
|-------------|------------|-------------------|
| 50          | 1000       | ~95%              |
| 200         | 1000       | ~85%              |
| 1000        | 1000       | ~65%              |

### Throughput (local testing)
```
Config: 50 threads, 200 req/thread, 100 unique URLs
Typical: 15,000+ req/sec sustained
Cache hit rate: 60-80% depending on URL distribution
```

### Concurrent Connections
- epoll handles 10k+ simultaneous connections
- Thread pool prevents thread explosion
- Memory bounded by cache capacity

## Benchmark Options

```
./benchmark [options]
  -h <host>    Target host (default: 127.0.0.1)
  -p <port>    Target port (default: 8080)
  -t <threads> Concurrent clients (default: 10)
  -n <reqs>    Requests per client (default: 100)
  -u <urls>    Unique URLs (default: 50)
  -d <ms>      Think time between requests (default: 0)
```

Tuning:
- Fewer unique URLs (`-u`) → higher cache hit rate
- More threads (`-t`) → tests concurrency handling
- Think time (`-d`) → simulates real user behavior

## Why These Design Choices

**epoll over select/poll**: O(1) vs O(n) for large fd sets. Edge-triggered mode reduces syscalls.

**Thread pool over thread-per-connection**: Bounded resource usage, no thread creation overhead per request.

**LRU over LFU/FIFO**: Good balance of simplicity and hit rate for typical web workloads. Recent = likely to be requested again.

**shared_mutex over mutex**: High read/write ratio in caches. Multiple concurrent readers don't block each other.

## Files

```
├── server_base_version.cpp  # Phase 1: Single-threaded TCP
├── server_phase2.cpp        # Phase 2: Thread per connection
├── server_phase3.cpp        # Phase 3: epoll + thread pool
├── server_phase4.cpp        # Phase 4: + LRU cache
├── server_phase5.cpp        # Phase 5: Full proxy
├── benchmark.cpp            # Load generator
├── test_lru_cache.cpp       # Cache unit tests
└── readme.md
```
