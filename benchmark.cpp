// benchmark.cpp
// Load generator. Fixes three things that made the old numbers meaningless:
//  - it sent Host: <the proxy itself>, so every request 502'd and nothing was
//    cacheable, but a 502 still counts as bytes received so it looked like 100%
//    success. -H now sets the Host header separately from the TCP target.
//  - one connection per request from one source IP hits EADDRNOTAVAIL once the
//    ephemeral range fills (only ~4096 wide on WSL), so you end up measuring the
//    client's port table. -i rotates over 127.0.0.x.
//  - no keep-alive, so the number was mostly handshake + TIME_WAIT cost. -k.
// Also reports percentiles; the mean hides the tail.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace std::chrono;

struct Config {
    string host = "127.0.0.1";     // TCP target (the proxy)
    int port = 8080;
    string host_header;            // what goes in the Host: header; default = host:port of origin
    int num_threads = 10;
    int requests_per_thread = 100;
    int num_unique_urls = 50;
    double zipf_alpha = 1.0;       // 0 = uniform, higher = more skewed
    int think_time_ms = 0;
    bool keep_alive = false;
    int source_ips = 1;            // rotate over 127.0.0.1 .. 127.0.0.<n>
    bool verbose_errors = false;
};

struct Totals {
    atomic<long long> ok{0};
    atomic<long long> failed{0};
    atomic<long long> conns{0};
    atomic<long long> bytes{0};
    atomic<long long> non_200{0};
} g;

static mutex g_lat_mtx;
static vector<long long> g_latencies; // microseconds
static mutex g_err_mtx;
static vector<string> g_errors;

static void note_error(const Config& cfg, const string& what) {
    if (!cfg.verbose_errors) return;
    lock_guard<mutex> l(g_err_mtx);
    if (g_errors.size() < 20) g_errors.push_back(what);
}

// ---------------------------------------------------------------------------
// Zipf: P(rank k) proportional to 1/k^alpha, k = 1..N.
// The CDF is precomputed once per thread; each draw is a binary search.
// alpha == 0 degenerates to uniform, which is the useful control case for
// cache measurements (a cache cannot beat the capacity/working-set ratio there).
// ---------------------------------------------------------------------------
class Zipf {
    vector<double> cdf_;

public:
    Zipf(int n, double alpha) {
        cdf_.resize(n);
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            sum += 1.0 / pow((double)(i + 1), alpha);
            cdf_[i] = sum;
        }
        for (auto& v : cdf_) v /= sum;
    }
    int operator()(mt19937_64& gen) const {
        uniform_real_distribution<double> u(0.0, 1.0);
        double r = u(gen);
        return (int)(lower_bound(cdf_.begin(), cdf_.end(), r) - cdf_.begin());
    }
};

// ---------------------------------------------------------------------------
static int connect_to(const Config& cfg, int thread_id, string* err) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (err) *err = string("socket: ") + strerror(errno);
        return -1;
    }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Spread connections across several loopback source addresses so the
    // ephemeral port range of any single source IP is not the binding limit.
    if (cfg.source_ips > 1) {
        string src = "127.0.0." + to_string(1 + (thread_id % cfg.source_ips));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = 0;
        inet_pton(AF_INET, src.c_str(), &sa.sin_addr);

        // Without this, binding the source ADDRESS also forces the kernel to
        // pick the source PORT right now, before connect() has told it the
        // destination. Not knowing the 4-tuple, it must find a port unused by
        // any socket on this IP and may not recycle TIME_WAIT ports, so once
        // the (here: 4096-entry) ephemeral range fills with TIME_WAIT, every
        // bind() degenerates into a scan-and-wait.
        //
        // Measured on this box: 94 req/s with the naive bind, 9724 req/s
        // without it, a 100x difference that is entirely the load generator's
        // fault and says nothing about the server.
        //
        // IP_BIND_ADDRESS_NO_PORT (Linux 4.2+) means "bind the address, defer
        // the port to connect()", restoring normal 4-tuple-aware reuse.
#ifdef IP_BIND_ADDRESS_NO_PORT
        setsockopt(sock, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one));
#endif
        if (::bind(sock, (sockaddr*)&sa, sizeof(sa)) < 0) {
            if (err) *err = "bind(" + src + "): " + strerror(errno);
            close(sock);
            return -1;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        if (err) *err = string("connect: ") + strerror(errno);
        close(sock);
        return -1;
    }
    g.conns++;
    return sock;
}

static bool send_all(int fd, const string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// Reads exactly one HTTP response: headers, then Content-Length bytes.
// Reading "whatever one recv() returns" would count a truncated response as a
// success and would desynchronise a keep-alive connection.
static bool read_one_response(int fd, string& buf, int* status, size_t* total, string* err) {
    char tmp[16384];
    for (;;) {
        size_t hend = buf.find("\r\n\r\n");
        if (hend != string::npos) {
            int st = 0;
            size_t sp = buf.find(' ');
            if (sp != string::npos) st = atoi(buf.c_str() + sp + 1);
            long clen = -1;
            // header search is case-insensitive on the name only
            string head = buf.substr(0, hend);
            string lower = head;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            size_t p = lower.find("content-length:");
            if (p != string::npos) clen = atol(head.c_str() + p + 15);
            size_t need = hend + 4 + (clen > 0 ? (size_t)clen : 0);
            if (clen < 0) {
                // No Content-Length: body is close-delimited. Drain to EOF.
                for (;;) {
                    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
                    if (n > 0) { buf.append(tmp, (size_t)n); continue; }
                    break;
                }
                *status = st;
                *total = buf.size();
                buf.clear();
                return true;
            }
            if (buf.size() >= need) {
                *status = st;
                *total = need;
                buf.erase(0, need);
                return true;
            }
        }
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) { buf.append(tmp, (size_t)n); continue; }
        if (n == 0) { if (err) *err = "peer closed mid-response"; return false; }
        if (errno == EINTR) continue;
        if (err) *err = string("recv: ") + strerror(errno);
        return false;
    }
}

static void worker(const Config& cfg, int thread_id) {
    mt19937_64 gen((uint64_t)random_device{}() ^ (uint64_t)thread_id * 0x9e3779b97f4a7c15ULL);
    Zipf zipf(cfg.num_unique_urls, cfg.zipf_alpha);

    vector<long long> local_lat;
    local_lat.reserve(cfg.requests_per_thread);

    int sock = -1;
    string rbuf;

    for (int i = 0; i < cfg.requests_per_thread; i++) {
        int url_id = zipf(gen);
        string path = "/resource/" + to_string(url_id);
        string req = "GET " + path + " HTTP/1.1\r\n"
                     "Host: " + cfg.host_header + "\r\n" +
                     (cfg.keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n") +
                     "\r\n";

        auto start = steady_clock::now();
        string err;

        if (sock < 0) {
            sock = connect_to(cfg, thread_id, &err);
            if (sock < 0) {
                g.failed++;
                note_error(cfg, err);
                continue;
            }
            rbuf.clear();
        }

        if (!send_all(sock, req)) {
            g.failed++;
            note_error(cfg, "send failed");
            close(sock);
            sock = -1;
            continue;
        }

        int status = 0;
        size_t bytes = 0;
        if (!read_one_response(sock, rbuf, &status, &bytes, &err)) {
            g.failed++;
            note_error(cfg, err);
            close(sock);
            sock = -1;
            continue;
        }

        auto end = steady_clock::now();
        local_lat.push_back(duration_cast<microseconds>(end - start).count());
        g.ok++;
        g.bytes += (long long)bytes;
        if (status != 200) g.non_200++;

        if (!cfg.keep_alive) {
            close(sock);
            sock = -1;
        }
        if (cfg.think_time_ms > 0) this_thread::sleep_for(milliseconds(cfg.think_time_ms));
    }
    if (sock >= 0) close(sock);

    lock_guard<mutex> l(g_lat_mtx);
    g_latencies.insert(g_latencies.end(), local_lat.begin(), local_lat.end());
}

static double pct(const vector<long long>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p / 100.0 * (double)(sorted.size() - 1);
    size_t lo = (size_t)idx;
    size_t hi = min(lo + 1, sorted.size() - 1);
    double frac = idx - (double)lo;
    return ((double)sorted[lo] * (1 - frac) + (double)sorted[hi] * frac) / 1000.0; // ms
}

int main(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(1); }
            return argv[++i];
        };
        if (a == "-h") cfg.host = val();
        else if (a == "-p") cfg.port = atoi(val());
        else if (a == "-H") cfg.host_header = val();
        else if (a == "-t") cfg.num_threads = atoi(val());
        else if (a == "-n") cfg.requests_per_thread = atoi(val());
        else if (a == "-u") cfg.num_unique_urls = atoi(val());
        else if (a == "-a") cfg.zipf_alpha = atof(val());
        else if (a == "-d") cfg.think_time_ms = atoi(val());
        else if (a == "-i") cfg.source_ips = atoi(val());
        else if (a == "-k") cfg.keep_alive = true;
        else if (a == "-v") cfg.verbose_errors = true;
        else {
            printf("Usage: %s [options]\n"
                   "  -h <host>    TCP target, the proxy (default 127.0.0.1)\n"
                   "  -p <port>    proxy port (default 8080)\n"
                   "  -H <host>    Host: header value, i.e. the ORIGIN (default = -h:-p)\n"
                   "  -t <n>       concurrent client threads (default 10)\n"
                   "  -n <n>       requests per thread (default 100)\n"
                   "  -u <n>       unique URLs = working set (default 50)\n"
                   "  -a <f>       Zipf alpha; 0 = uniform (default 1.0)\n"
                   "  -d <ms>      think time between requests (default 0)\n"
                   "  -i <n>       rotate over n loopback source IPs (default 1)\n"
                   "  -k           reuse connections (HTTP keep-alive)\n"
                   "  -v           print first errors encountered\n",
                   argv[0]);
            return 1;
        }
    }
    if (cfg.host_header.empty()) cfg.host_header = cfg.host + ":" + to_string(cfg.port);
    if (cfg.num_unique_urls < 1) cfg.num_unique_urls = 1;

    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    long long total_requests = (long long)cfg.num_threads * cfg.requests_per_thread;
    printf("=== Benchmark ===\n");
    printf("target        : %s:%d   Host: %s\n", cfg.host.c_str(), cfg.port, cfg.host_header.c_str());
    printf("threads       : %d  (requests/thread %d, total %lld)\n", cfg.num_threads,
           cfg.requests_per_thread, total_requests);
    printf("working set   : %d unique URLs, zipf alpha %.2f\n", cfg.num_unique_urls, cfg.zipf_alpha);
    printf("connections   : %s, %d source IP(s)\n", cfg.keep_alive ? "keep-alive (reused)" : "one per request",
           cfg.source_ips);
    printf("\n");

    g_latencies.reserve((size_t)total_requests);

    auto start = steady_clock::now();
    vector<thread> threads;
    threads.reserve(cfg.num_threads);
    for (int i = 0; i < cfg.num_threads; i++) threads.emplace_back(worker, cref(cfg), i);
    for (auto& t : threads) t.join();
    auto end = steady_clock::now();

    double elapsed = duration_cast<microseconds>(end - start).count() / 1e6;
    long long done = g.ok + g.failed;
    double rps = elapsed > 0 ? (double)g.ok / elapsed : 0.0;

    vector<long long> lat;
    {
        lock_guard<mutex> l(g_lat_mtx);
        lat = g_latencies;
    }
    sort(lat.begin(), lat.end());
    double mean_ms = lat.empty() ? 0.0
                                 : (double)accumulate(lat.begin(), lat.end(), 0LL) /
                                       (double)lat.size() / 1000.0;

    printf("=== Results ===\n");
    printf("duration      : %.3f s\n", elapsed);
    printf("requests      : %lld ok, %lld failed (of %lld attempted)\n", (long long)g.ok,
           (long long)g.failed, done);
    printf("non-200       : %lld\n", (long long)g.non_200);
    printf("THROUGHPUT    : %.0f req/s\n", rps);
    printf("bytes         : %.2f MB (%.2f MB/s)\n", (double)g.bytes / 1e6,
           (double)g.bytes / 1e6 / (elapsed > 0 ? elapsed : 1));
    printf("connections   : %lld opened\n", (long long)g.conns);
    printf("latency ms    : mean %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f\n", mean_ms,
           pct(lat, 50), pct(lat, 95), pct(lat, 99), pct(lat, 100));
    printf("\ncache hit rate is reported by the proxy (see its [stats] line)\n");

    if (cfg.verbose_errors) {
        lock_guard<mutex> l(g_err_mtx);
        if (!g_errors.empty()) {
            printf("\nfirst errors:\n");
            for (auto& e : g_errors) printf("  %s\n", e.c_str());
        }
    }
    return g.failed > 0 ? 1 : 0;
}
