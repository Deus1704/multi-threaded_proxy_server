// concurrency_test.cpp
// Opens N sockets, holds them all open, then drives a request over each one.
// "Concurrent" has to mean simultaneously ESTABLISHED, otherwise you're just
// measuring requests-over-time and a server that quietly drops early
// connections would still pass.
//
// Two client-side walls show up before any server limit:
//  - RLIMIT_NOFILE, raised to the hard limit here.
//  - the 4-tuple has to be unique, so one source IP against one destination
//    caps at the ephemeral range (~4096 on WSL). Rotate over 127.0.0.x to get
//    past it. Worth reporting which wall you hit and whose it is.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace std::chrono;

struct Cfg {
    string host = "127.0.0.1";
    int port = 8080;
    string host_header;
    int target = 10000;
    int source_ips = 8;
    int hold_seconds = 3;
    bool request_on_each = true;
};

int main(int argc, char** argv) {
    Cfg cfg;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(1); }
            return argv[++i];
        };
        if (a == "-h") cfg.host = val();
        else if (a == "-p") cfg.port = atoi(val());
        else if (a == "-H") cfg.host_header = val();
        else if (a == "-n") cfg.target = atoi(val());
        else if (a == "-i") cfg.source_ips = atoi(val());
        else if (a == "-s") cfg.hold_seconds = atoi(val());
        else if (a == "--no-traffic") cfg.request_on_each = false;
        else {
            printf("Usage: %s [-h host] [-p port] [-H host_header] [-n conns]"
                   " [-i source_ips] [-s hold_seconds] [--no-traffic]\n", argv[0]);
            return 1;
        }
    }
    if (cfg.host_header.empty()) cfg.host_header = cfg.host + ":" + to_string(cfg.port);

    // --- raise our own fd limit; N sockets need N descriptors
    rlimit rl{};
    getrlimit(RLIMIT_NOFILE, &rl);
    rlim_t before = rl.rlim_cur;
    if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    printf("client fd limit : %llu -> %llu\n", (unsigned long long)before,
           (unsigned long long)rl.rlim_cur);

    FILE* f = fopen("/proc/sys/net/ipv4/ip_local_port_range", "r");
    int lo = 0, hi = 0;
    if (f) { if (fscanf(f, "%d %d", &lo, &hi) != 2) { lo = hi = 0; } fclose(f); }
    int ports_per_ip = (hi > lo) ? (hi - lo + 1) : 0;
    printf("ephemeral ports : %d..%d (%d per source IP)\n", lo, hi, ports_per_ip);
    printf("source IPs      : %d  => theoretical ceiling %d connections\n", cfg.source_ips,
           ports_per_ip * cfg.source_ips);
    printf("target          : %d concurrent connections\n\n", cfg.target);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.port);
    inet_pton(AF_INET, cfg.host.c_str(), &dst.sin_addr);

    vector<int> socks;
    socks.reserve(cfg.target);
    string first_failure;
    int failure_at = -1;

    auto t0 = steady_clock::now();
    for (int i = 0; i < cfg.target; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            first_failure = string("socket: ") + strerror(errno);
            failure_at = i;
            break;
        }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (cfg.source_ips > 1) {
            string src = "127.0.0." + to_string(1 + (i % cfg.source_ips));
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port = 0;
            inet_pton(AF_INET, src.c_str(), &sa.sin_addr);
            // Defer source-port selection to connect(), so the kernel picks it
            // knowing the full 4-tuple. Binding the address alone would make it
            // choose a port blind and refuse to reuse TIME_WAIT entries.
#ifdef IP_BIND_ADDRESS_NO_PORT
            setsockopt(s, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one));
#endif
            if (::bind(s, (sockaddr*)&sa, sizeof(sa)) < 0) {
                first_failure = "bind(" + src + "): " + strerror(errno);
                failure_at = i;
                close(s);
                break;
            }
        }
        if (connect(s, (sockaddr*)&dst, sizeof(dst)) < 0) {
            first_failure = string("connect: ") + strerror(errno);
            failure_at = i;
            close(s);
            break;
        }
        socks.push_back(s);
        if ((i + 1) % 2000 == 0)
            printf("  ... %d connections established\n", i + 1);
    }
    double ramp = duration_cast<milliseconds>(steady_clock::now() - t0).count() / 1000.0;

    printf("\nESTABLISHED     : %zu / %d  (ramp %.2fs, %.0f conn/s)\n", socks.size(), cfg.target,
           ramp, ramp > 0 ? socks.size() / ramp : 0.0);
    if (failure_at >= 0)
        printf("first failure   : at #%d -- %s\n", failure_at, first_failure.c_str());

    // --- hold them all open, then confirm they are still usable
    printf("holding %d s ...\n", cfg.hold_seconds);
    this_thread::sleep_for(seconds(cfg.hold_seconds));

    // Count how many are still alive: POLLIN with zero-byte read means the peer
    // closed. A server that "handles 10k" by dropping the oldest fails here.
    int still_open = 0, closed_by_peer = 0;
    for (int s : socks) {
        pollfd p{s, POLLIN | POLLRDHUP, 0};
        int r = poll(&p, 1, 0);
        if (r > 0 && (p.revents & (POLLHUP | POLLRDHUP | POLLERR))) closed_by_peer++;
        else still_open++;
    }
    printf("after hold      : %d still open, %d closed by peer\n", still_open, closed_by_peer);

    long long served = 0, failed = 0;
    if (cfg.request_on_each && !socks.empty()) {
        printf("issuing one request on each held connection ...\n");
        string req = "GET /resource/0 HTTP/1.1\r\nHost: " + cfg.host_header +
                     "\r\nConnection: keep-alive\r\n\r\n";
        auto t1 = steady_clock::now();
        for (int s : socks) {
            if (send(s, req.data(), req.size(), MSG_NOSIGNAL) < 0) { failed++; continue; }
        }
        // Drain responses. Deadline-bounded so a stuck connection cannot hang
        // the test forever.
        auto deadline = steady_clock::now() + seconds(30);
        for (int s : socks) {
            string buf;
            char tmp[8192];
            bool got = false;
            while (steady_clock::now() < deadline) {
                pollfd p{s, POLLIN, 0};
                if (poll(&p, 1, 200) <= 0) break;
                ssize_t n = recv(s, tmp, sizeof(tmp), 0);
                if (n <= 0) break;
                buf.append(tmp, (size_t)n);
                if (buf.find("\r\n\r\n") != string::npos) { got = true; break; }
            }
            if (got) served++; else failed++;
        }
        double dt = duration_cast<milliseconds>(steady_clock::now() - t1).count() / 1000.0;
        printf("requests served : %lld ok, %lld failed over %.2fs\n", served, failed, dt);
    }

    for (int s : socks) close(s);

    bool pass = (int)socks.size() >= cfg.target && closed_by_peer == 0 &&
                (!cfg.request_on_each || failed == 0);
    printf("\nRESULT          : %s (%zu concurrent connections sustained)\n",
           pass ? "PASS" : "PARTIAL", socks.size());
    return pass ? 0 : 1;
}
