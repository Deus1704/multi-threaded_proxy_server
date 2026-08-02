// Phase 5: full forward proxy
// epoll (ET + oneshot) -> task queue -> thread pool -> parse/ACL/cache/forward/log
//
// Notes to self on the tricky bits:
//  - EPOLLONESHOT: kernel disarms the fd on delivery so only one worker owns a
//    connection at a time. Without it two threads can race on the same buffer
//    and both call close(). Worker re-arms with EPOLL_CTL_MOD when done.
//  - Edge-triggered means drain until EAGAIN or you never get told again.
//  - One recv() is not a request. TCP is a stream, headers can split across
//    segments, so buffer until we see the terminator + Content-Length bytes.
//  - Upstream reads follow the response framing, not EOF. Reading to EOF stalls
//    for the whole 5s timeout on every request against a keep-alive origin.
//  - setrlimit at startup. 10k connections is an fd limit question first and the
//    default soft limit is lower than that.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "http.hpp"
#include "lru_cache.hpp"

using namespace std;
using namespace std::chrono;

// ============================================================================
// SYNCHRONISED TASK QUEUE
// ============================================================================
class TaskQueue {
    queue<function<void()>> tasks_;
    mutable mutex mtx_;
    condition_variable cv_;
    bool shutdown_ = false;
    size_t high_water_ = 0;

public:
    void push(function<void()> task) {
        {
            lock_guard<mutex> lock(mtx_);
            if (shutdown_) return;
            tasks_.push(move(task));
            if (tasks_.size() > high_water_) high_water_ = tasks_.size();
        }
        // Notify outside the lock: waking a worker that then immediately blocks
        // on a mutex we still hold is a wasted context switch.
        cv_.notify_one();
    }

    // Blocks until a task is available. Returns nullptr only on shutdown.
    function<void()> pop() {
        unique_lock<mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !tasks_.empty() || shutdown_; });
        if (tasks_.empty()) return nullptr; // shutting down and drained
        auto task = move(tasks_.front());
        tasks_.pop();
        return task;
    }

    void stop() {
        {
            lock_guard<mutex> lock(mtx_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    size_t depth() const {
        lock_guard<mutex> lock(mtx_);
        return tasks_.size();
    }
    size_t high_water() const {
        lock_guard<mutex> lock(mtx_);
        return high_water_;
    }
};

class ThreadPool {
    vector<thread> workers_;
    TaskQueue& queue_;

public:
    ThreadPool(TaskQueue& q, int n) : queue_(q) {
        for (int i = 0; i < n; i++) {
            workers_.emplace_back([this] {
                for (;;) {
                    auto task = queue_.pop();
                    if (!task) break; // shutdown
                    task();
                }
            });
        }
    }
    void join() {
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }
    size_t size() const { return workers_.size(); }
};

// ============================================================================
// GLOBAL STATE
// ============================================================================
struct Stats {
    atomic<uint64_t> total_connections{0};
    atomic<uint64_t> active_connections{0};
    atomic<uint64_t> peak_connections{0};
    atomic<uint64_t> total_requests{0};
    atomic<uint64_t> forwarded_requests{0};
    atomic<uint64_t> cache_served{0};
    atomic<uint64_t> denied_requests{0};
    atomic<uint64_t> bad_requests{0};
    atomic<uint64_t> upstream_errors{0};
} g_stats;

struct Options {
    int port = 8080;
    size_t cache_capacity = 2000;
    size_t cache_shards = 16;
    int workers = 0; // 0 = hardware_concurrency
    string acl_path;
    string log_path;
    bool log_stdout = false;
    bool log_immediate = false;
    bool quiet_stats = false;
    int upstream_timeout_sec = 5;
    size_t max_header_bytes = 64 * 1024;
    size_t max_body_bytes = 8 * 1024 * 1024;
} g_opt;

using Cache = ShardedLRUCache<string, string>;
static unique_ptr<Cache> g_cache;
static http::AccessControl g_acl;
static http::AccessLog g_log;
static atomic<bool> g_running{true};
static int g_epoll_fd = -1;

// ============================================================================
// PER-CONNECTION STATE
// ============================================================================
// Stored in epoll_event.data.ptr. EPOLLONESHOT guarantees only one thread owns
// a Conn at a time, so these fields need no locking.
struct Conn {
    int fd = -1;
    string client_ip;
    string inbuf;                  // bytes received, not yet consumed
    uint64_t requests_served = 0;
    steady_clock::time_point accepted_at = steady_clock::now();
};

static void close_conn(Conn* c) {
    if (!c) return;
    if (c->fd >= 0) {
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, c->fd, nullptr);
        close(c->fd);
        g_stats.active_connections--;
    }
    delete c;
}

// ============================================================================
// SOCKET HELPERS
// ============================================================================
static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Writes all n bytes or fails. send() may accept fewer bytes than asked; a
// single send() with an ignored return value silently truncates responses.
static bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    int spins = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            spins = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full. Wait for writability rather than spinning the
            // CPU; bounded so a stalled peer cannot pin a worker forever.
            if (++spins > 1000) return false;
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 5000) <= 0) return false;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool send_all(int fd, const string& s) { return send_all(fd, s.data(), s.size()); }

static string build_response(int status, const string& reason, const string& body,
                             bool keep_alive, const string& content_type = "text/plain") {
    string out = "HTTP/1.1 " + to_string(status) + " " + reason + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    out += "Content-Length: " + to_string(body.size()) + "\r\n";
    out += "Connection: " + string(keep_alive ? "keep-alive" : "close") + "\r\n";
    out += "\r\n";
    out += body;
    return out;
}

// ============================================================================
// UPSTREAM FETCH
// ============================================================================
struct UpstreamResult {
    string raw;
    int status = 0;
    bool ok = false;
    string error;
};

static UpstreamResult fetch_upstream(const http::Upstream& up, const string& request_bytes) {
    UpstreamResult res;

    // Resolve. getaddrinfo is thread-safe; gethostbyname (used before) is not,
    // and this runs on every worker thread concurrently.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* ai = nullptr;
    string port_str = to_string(up.port);
    int rc = getaddrinfo(up.host.c_str(), port_str.c_str(), &hints, &ai);
    if (rc != 0 || !ai) {
        res.error = string("dns: ") + gai_strerror(rc);
        return res;
    }

    int sock = socket(ai->ai_family, ai->ai_socktype, 0);
    if (sock < 0) {
        freeaddrinfo(ai);
        res.error = "socket: " + string(strerror(errno));
        return res;
    }

    timeval tv{g_opt.upstream_timeout_sec, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
        res.error = "connect: " + string(strerror(errno));
        freeaddrinfo(ai);
        close(sock);
        return res;
    }
    freeaddrinfo(ai);

    if (!send_all(sock, request_bytes)) {
        res.error = "send: " + string(strerror(errno));
        close(sock);
        return res;
    }

    // Read until the message framing says we are done, not until EOF.
    string buf;
    char tmp[16384];
    http::Response parsed;
    bool have_headers = false;
    for (;;) {
        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf.append(tmp, (size_t)n);
            if (!have_headers) {
                if (http::find_header_end(buf) != string::npos) {
                    parsed = http::parse_response_headers(buf);
                    if (!parsed.valid) {
                        res.error = "malformed upstream response";
                        close(sock);
                        return res;
                    }
                    have_headers = true;
                } else if (buf.size() > g_opt.max_header_bytes) {
                    res.error = "upstream headers too large";
                    close(sock);
                    return res;
                }
            }
            if (have_headers) {
                if (buf.size() > g_opt.max_body_bytes) {
                    res.error = "upstream body too large";
                    close(sock);
                    return res;
                }
                if (!parsed.close_delimited && http::response_complete(buf, parsed)) break;
            }
            continue;
        }
        if (n == 0) break; // EOF: correct terminator for a close-delimited body
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            res.error = "upstream timeout";
            close(sock);
            return res;
        }
        res.error = "recv: " + string(strerror(errno));
        close(sock);
        return res;
    }
    close(sock);

    if (buf.empty()) {
        res.error = "empty upstream response";
        return res;
    }
    if (!have_headers) {
        parsed = http::parse_response_headers(buf);
        if (!parsed.valid) {
            res.error = "malformed upstream response";
            return res;
        }
    }

    res.raw = move(buf);
    res.status = parsed.status;
    res.ok = true;
    return res;
}

// ============================================================================
// REQUEST HANDLING
// ============================================================================
// Returns true if the connection should stay open for another request.
static bool serve_one_request(Conn* c, const http::Request& req, size_t consumed) {
    auto t0 = steady_clock::now();
    g_stats.total_requests++;

    http::AccessLog::Entry log;
    log.client_ip = c->client_ip;
    log.method = req.method;
    log.path = req.path;
    log.version = req.version;
    log.host = req.host;

    auto finish = [&](int status, size_t bytes, const char* cache_state, bool keep_alive) {
        log.status = status;
        log.bytes = bytes;
        log.cache = cache_state;
        log.duration_us = duration_cast<microseconds>(steady_clock::now() - t0).count();
        g_log.write(log);
        return keep_alive;
    };

    // ---- access control, before any upstream work happens
    http::AclResult acl = g_acl.check(req);
    log.acl = (acl.decision == http::Decision::Allow) ? "ALLOW" : "DENY";
    log.rule = acl.matched_rule;
    if (acl.decision == http::Decision::Deny) {
        g_stats.denied_requests++;
        string body = "403 Forbidden: blocked by proxy policy\n";
        string resp = build_response(403, "Forbidden", body, req.keep_alive);
        bool sent = send_all(c->fd, resp);
        return finish(403, resp.size(), "BYPASS", sent && req.keep_alive);
    }

    // ---- host-header routing
    http::Upstream up = g_acl.routes().resolve(req.host);
    log.upstream = up.host + ":" + to_string(up.port);

    // Cache key must include the authority: /index.html on two different hosts
    // are different resources. Method is included so HEAD cannot serve GET.
    const string cache_key = req.method + " " + http::to_lower(req.host) + req.path;

    bool cacheable_request = (req.method == "GET" || req.method == "HEAD") &&
                             !req.header("authorization");

    if (cacheable_request) {
        if (auto hit = g_cache->get(cache_key)) {
            g_stats.cache_served++;
            bool sent = send_all(c->fd, *hit);
            http::Response r = http::parse_response_headers(*hit);
            return finish(r.status, hit->size(), "HIT", sent && req.keep_alive);
        }
    }

    // ---- forward
    // Forward exactly the bytes this request occupied, not the whole buffer:
    // with keep-alive the buffer may already hold the next pipelined request.
    string forward_bytes = req.raw.substr(0, consumed);
    UpstreamResult ur = fetch_upstream(up, forward_bytes);

    if (!ur.ok) {
        g_stats.upstream_errors++;
        string body = "502 Bad Gateway: " + ur.error + "\n";
        string resp = build_response(502, "Bad Gateway", body, false);
        send_all(c->fd, resp);
        log.rule = ur.error;
        return finish(502, resp.size(), cacheable_request ? "MISS" : "BYPASS", false);
    }

    g_stats.forwarded_requests++;

    http::Response parsed = http::parse_response_headers(ur.raw);
    if (cacheable_request && http::is_cacheable(req, parsed)) g_cache->put(cache_key, ur.raw);

    bool sent = send_all(c->fd, ur.raw);
    return finish(ur.status, ur.raw.size(), cacheable_request ? "MISS" : "BYPASS",
                  sent && req.keep_alive);
}

// Drains the socket, serves every complete request in the buffer, then either
// re-arms the fd in epoll or closes it.
static void handle_ready(Conn* c) {
    for (;;) {
        // 1. Drain readable bytes (edge-triggered: read until EAGAIN).
        bool peer_closed = false;
        char tmp[16384];
        for (;;) {
            ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                c->inbuf.append(tmp, (size_t)n);
                if (c->inbuf.size() > g_opt.max_header_bytes + g_opt.max_body_bytes) {
                    string resp = build_response(413, "Payload Too Large",
                                                 "413 Payload Too Large\n", false);
                    send_all(c->fd, resp);
                    close_conn(c);
                    return;
                }
                continue;
            }
            if (n == 0) { peer_closed = true; break; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(c);
            return;
        }

        // 2. Serve every complete request sitting in the buffer.
        bool served_any = false;
        for (;;) {
            size_t hend = http::find_header_end(c->inbuf);
            if (hend == string::npos) {
                if (c->inbuf.size() > g_opt.max_header_bytes) {
                    string resp = build_response(431, "Request Header Fields Too Large",
                                                 "431 Header Too Large\n", false);
                    send_all(c->fd, resp);
                    g_stats.bad_requests++;
                    close_conn(c);
                    return;
                }
                break; // need more bytes
            }

            http::Request req = http::parse_request(c->inbuf);
            if (!req.valid) {
                g_stats.bad_requests++;
                string body = "400 Bad Request: " + req.parse_error + "\n";
                string resp = build_response(400, "Bad Request", body, false);
                send_all(c->fd, resp);
                http::AccessLog::Entry e;
                e.client_ip = c->client_ip;
                e.method = req.method.empty() ? "-" : req.method;
                e.path = req.path.empty() ? "-" : req.path;
                e.version = req.version.empty() ? "-" : req.version;
                e.status = 400;
                e.bytes = resp.size();
                e.acl = "-";
                e.rule = req.parse_error;
                g_log.write(e);
                close_conn(c);
                return;
            }

            size_t need = http::expected_request_bytes(req);
            if (c->inbuf.size() < need) break; // body still arriving

            bool keep = serve_one_request(c, req, need);
            c->inbuf.erase(0, need);
            c->requests_served++;
            served_any = true;

            if (!keep) {
                close_conn(c);
                return;
            }
        }

        if (peer_closed) {
            close_conn(c);
            return;
        }

        // 3. If we served something, loop once more: bytes may have arrived
        //    while we were talking to upstream, and edge-triggered epoll will
        //    not re-notify us for data that was already readable.
        if (!served_any) break;
    }

    // 4. Re-arm. EPOLLONESHOT was cleared by delivery, so without this MOD the
    //    connection would never be reported readable again.
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.ptr = c;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) < 0) close_conn(c);
}

// ============================================================================
// STARTUP
// ============================================================================
// The 10k-connection claim is a file-descriptor claim first. Raise the soft
// limit to the hard limit and report what we actually got.
static rlim_t raise_fd_limit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 0;
    rlim_t want = rl.rlim_max;
    if (rl.rlim_cur < want) {
        rlimit newrl = rl;
        newrl.rlim_cur = want;
        if (setrlimit(RLIMIT_NOFILE, &newrl) == 0) rl.rlim_cur = want;
    }
    return rl.rlim_cur;
}

static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) perror("SO_REUSEADDR");
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // These checks are the whole point: the previous version ignored bind()'s
    // return value, so a port already in use produced a server that started,
    // printed a banner, listened to nothing, and reported zero traffic while a
    // different process answered the benchmark.
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind(:%d) failed: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        fprintf(stderr, "listen(:%d) failed: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (!set_nonblocking(fd)) {
        perror("set_nonblocking(listener)");
        close(fd);
        return -1;
    }
    return fd;
}

static void report_stats(const TaskQueue& queue, const ThreadPool& pool) {
    while (g_running) {
        // Tick at 100ms to flush the buffered access log; report stats every 5s.
        for (int i = 0; i < 50 && g_running; i++) {
            this_thread::sleep_for(milliseconds(100));
            g_log.flush();
        }
        if (!g_running) break;
        if (g_opt.quiet_stats) continue; // still flushing the log above
        auto cs = g_cache->stats();
        fprintf(stderr,
                "[stats] conns=%llu active=%llu peak=%llu | req=%llu cache_hit=%llu "
                "fwd=%llu denied=%llu bad=%llu upstream_err=%llu | cache=%.2f%% "
                "(%llu/%llu) entries=%zu evict=%llu | queue=%zu hw=%zu workers=%zu log_lines=%llu\n",
                (unsigned long long)g_stats.total_connections,
                (unsigned long long)g_stats.active_connections,
                (unsigned long long)g_stats.peak_connections,
                (unsigned long long)g_stats.total_requests,
                (unsigned long long)g_stats.cache_served,
                (unsigned long long)g_stats.forwarded_requests,
                (unsigned long long)g_stats.denied_requests,
                (unsigned long long)g_stats.bad_requests,
                (unsigned long long)g_stats.upstream_errors, cs.hit_rate_pct,
                (unsigned long long)cs.hits, (unsigned long long)(cs.hits + cs.misses), cs.size,
                (unsigned long long)cs.evictions, queue.depth(), queue.high_water(), pool.size(),
                (unsigned long long)g_log.lines());
    }
}

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s <port> [options]\n"
            "  --workers <n>       thread pool size (default: hardware_concurrency)\n"
            "  --cache <n>         total cache entries (default: 2000)\n"
            "  --shards <n>        cache shards, rounded to a power of 2 (default: 16)\n"
            "  --acl <file>        access-control config (default: none => allow all)\n"
            "  --log <file>        access log path (default: stdout)\n"
            "  --log-stdout        also write the access log to stdout\n"
            "  --no-stats          suppress the periodic stats line\n"
            "  --upstream-timeout <sec>  (default: 5)\n",
            prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    g_opt.port = atoi(argv[1]);
    if (g_opt.port <= 0 || g_opt.port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", what);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--workers") g_opt.workers = atoi(need("--workers"));
        else if (a == "--cache") g_opt.cache_capacity = (size_t)atoll(need("--cache"));
        else if (a == "--shards") g_opt.cache_shards = (size_t)atoll(need("--shards"));
        else if (a == "--acl") g_opt.acl_path = need("--acl");
        else if (a == "--log") g_opt.log_path = need("--log");
        else if (a == "--log-stdout") g_opt.log_stdout = true;
        else if (a == "--log-immediate") g_opt.log_immediate = true;
        else if (a == "--no-stats") g_opt.quiet_stats = true;
        else if (a == "--upstream-timeout") g_opt.upstream_timeout_sec = atoi(need("--upstream-timeout"));
        else {
            usage(argv[0]);
            return 1;
        }
    }

    // A client that disconnects mid-response would otherwise kill the process
    // via SIGPIPE. MSG_NOSIGNAL covers send(), this covers everything else.
    signal(SIGPIPE, SIG_IGN);

    if (!g_opt.acl_path.empty()) {
        string err;
        if (!g_acl.load_file(g_opt.acl_path, &err)) {
            fprintf(stderr, "ACL load failed (%s): %s\n", g_opt.acl_path.c_str(), err.c_str());
            return 1; // fail closed: a broken ACL must not become "allow all"
        }
    }
    if (!g_log.open(g_opt.log_path, g_opt.log_stdout, g_opt.log_immediate)) {
        fprintf(stderr, "cannot open access log: %s\n", g_opt.log_path.c_str());
        return 1;
    }

    int workers = g_opt.workers > 0 ? g_opt.workers : (int)thread::hardware_concurrency();
    if (workers <= 0) workers = 4;

    g_cache = make_unique<Cache>(g_opt.cache_capacity, g_opt.cache_shards);

    rlim_t fd_limit = raise_fd_limit();

    int listen_fd = make_listener(g_opt.port);
    if (listen_fd < 0) return 1;

    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    // Listener stays level-triggered with data.ptr == nullptr as its tag: we
    // want to be told repeatedly while the accept queue is non-empty.
    epoll_event lev{};
    lev.events = EPOLLIN;
    lev.data.ptr = nullptr;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, listen_fd, &lev) < 0) {
        perror("epoll_ctl(listener)");
        return 1;
    }

    TaskQueue tasks;
    ThreadPool pool(tasks, workers);

    fprintf(stderr,
            "proxy listening on :%d\n"
            "  workers      : %d\n"
            "  cache        : %zu entries across %zu shards\n"
            "  fd limit     : %llu (soft, raised to hard limit)\n"
            "  acl          : %s (%zu rules, %zu routes, default %s)\n"
            "  access log   : %s\n",
            g_opt.port, workers, g_cache->capacity(), g_cache->num_shards(),
            (unsigned long long)fd_limit,
            g_opt.acl_path.empty() ? "(none)" : g_opt.acl_path.c_str(), g_acl.rule_count(),
            g_acl.routes().size(),
            g_acl.default_policy() == http::Decision::Allow ? "allow" : "deny",
            g_opt.log_path.empty() ? "(stdout)" : g_opt.log_path.c_str());

    // Always runs: it owns the periodic access-log flush, not just the stats
    // line. --no-stats silences the printing, it does not stop the flushing.
    thread stats_thread(report_stats, ref(tasks), ref(pool));

    vector<epoll_event> events(1024);

    while (g_running) {
        int n = epoll_wait(g_epoll_fd, events.data(), (int)events.size(), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == nullptr) {
                // Accept every pending connection; the listener is level-
                // triggered so a partial drain is safe, but draining fully
                // keeps the accept queue short under a burst.
                for (;;) {
                    sockaddr_in cli{};
                    socklen_t len = sizeof(cli);
                    int cfd = accept(listen_fd, (sockaddr*)&cli, &len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        if (errno == EMFILE || errno == ENFILE) {
                            fprintf(stderr, "accept: out of file descriptors\n");
                            break;
                        }
                        break;
                    }
                    if (!set_nonblocking(cfd)) {
                        close(cfd);
                        continue;
                    }
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    Conn* c = new Conn();
                    c->fd = cfd;
                    char ipbuf[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));
                    c->client_ip = ipbuf;

                    uint64_t active = ++g_stats.active_connections;
                    g_stats.total_connections++;
                    uint64_t prev = g_stats.peak_connections.load();
                    while (active > prev &&
                           !g_stats.peak_connections.compare_exchange_weak(prev, active)) {
                    }

                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
                    ev.data.ptr = c;
                    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
                        perror("epoll_ctl(add client)");
                        close(cfd);
                        g_stats.active_connections--;
                        delete c;
                    }
                }
            } else {
                Conn* c = static_cast<Conn*>(events[i].data.ptr);
                // One-shot already disarmed this fd, so dispatching to the pool
                // cannot race with another readiness event for the same conn.
                tasks.push([c] { handle_ready(c); });
            }
        }
    }

    g_running = false;
    tasks.stop();
    pool.join();
    if (stats_thread.joinable()) stats_thread.join();
    g_log.flush(); // do not lose the tail of the audit trail on shutdown
    close(listen_fd);
    close(g_epoll_fd);
    return 0;
}
