// origin_server.cpp
// Minimal HTTP origin used as the upstream in benchmarks.
//
// It exists so the proxy's cache and forwarding paths are measured against a
// real server instead of against connection failures. Without an origin,
// fetch_upstream() fails, every response is a 502, nothing is cacheable, and the
// measured "cache hit rate" is 0% -- which is exactly what the original
// benchmark was silently reporting.
//
// Kept deliberately fast (epoll + thread pool, fixed-size body, no disk I/O) so
// that it is not the bottleneck when measuring the proxy.

#include <arpa/inet.h>
#include <fcntl.h>
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
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace std;

static int g_epoll_fd = -1;
static atomic<uint64_t> g_requests{0};
static atomic<uint64_t> g_connections{0};
static size_t g_body_size = 512;
static bool g_keep_alive = true;

struct Conn {
    int fd = -1;
    string inbuf;
};

class TaskQueue {
    queue<function<void()>> tasks_;
    mutex mtx_;
    condition_variable cv_;
    bool stop_ = false;

public:
    void push(function<void()> t) {
        {
            lock_guard<mutex> l(mtx_);
            if (stop_) return;
            tasks_.push(move(t));
        }
        cv_.notify_one();
    }
    function<void()> pop() {
        unique_lock<mutex> l(mtx_);
        cv_.wait(l, [this] { return !tasks_.empty() || stop_; });
        if (tasks_.empty()) return nullptr;
        auto t = move(tasks_.front());
        tasks_.pop();
        return t;
    }
    void stop() {
        {
            lock_guard<mutex> l(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
    }
};

static bool send_all(int fd, const char* d, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, d + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 5000) <= 0) return false;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static void close_conn(Conn* c) {
    if (!c) return;
    if (c->fd >= 0) {
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, c->fd, nullptr);
        close(c->fd);
    }
    delete c;
}

static void handle(Conn* c) {
    char tmp[8192];
    bool peer_closed = false;
    for (;;) {
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
        if (n > 0) { c->inbuf.append(tmp, (size_t)n); continue; }
        if (n == 0) { peer_closed = true; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_conn(c);
        return;
    }

    bool client_wants_close = false;
    for (;;) {
        size_t hend = c->inbuf.find("\r\n\r\n");
        if (hend == string::npos) break;
        string head = c->inbuf.substr(0, hend);
        c->inbuf.erase(0, hend + 4);

        // request-target, purely so the body differs per path (so caching a
        // response for /a and replaying it for /b would be visible)
        string path = "/";
        size_t sp1 = head.find(' ');
        if (sp1 != string::npos) {
            size_t sp2 = head.find(' ', sp1 + 1);
            if (sp2 != string::npos) path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        }
        string lower = head;
        for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
        if (lower.find("connection: close") != string::npos) client_wants_close = true;

        string body = "origin-response path=" + path + " ";
        while (body.size() < g_body_size) body += "x";
        body.resize(g_body_size);

        bool keep = g_keep_alive && !client_wants_close;
        string resp = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: " + to_string(body.size()) + "\r\n"
                      "Cache-Control: max-age=60\r\n"
                      "Connection: " + string(keep ? "keep-alive" : "close") + "\r\n"
                      "\r\n" + body;
        g_requests++;
        if (!send_all(c->fd, resp.data(), resp.size())) {
            close_conn(c);
            return;
        }
        if (!keep) {
            close_conn(c);
            return;
        }
    }

    if (peer_closed) {
        close_conn(c);
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.ptr = c;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) < 0) close_conn(c);
}

int main(int argc, char** argv) {
    int port = 19000;
    int workers = 8;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "-p" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "-w" && i + 1 < argc) workers = atoi(argv[++i]);
        else if (a == "-b" && i + 1 < argc) g_body_size = (size_t)atoll(argv[++i]);
        else if (a == "--close") g_keep_alive = false;
        else {
            fprintf(stderr, "Usage: %s [-p port] [-w workers] [-b body_bytes] [--close]\n", argv[0]);
            return 1;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(lfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(lfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "origin bind(:%d): %s\n", port, strerror(errno));
        return 1;
    }
    if (listen(lfd, SOMAXCONN) < 0) { perror("listen"); return 1; }
    int fl = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, fl | O_NONBLOCK);

    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) { perror("epoll_create1"); return 1; }
    epoll_event lev{};
    lev.events = EPOLLIN;
    lev.data.ptr = nullptr;
    epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, lfd, &lev);

    TaskQueue q;
    vector<thread> pool;
    for (int i = 0; i < workers; i++)
        pool.emplace_back([&q] {
            for (;;) {
                auto t = q.pop();
                if (!t) break;
                t();
            }
        });

    fprintf(stderr, "origin on :%d workers=%d body=%zuB keep-alive=%s\n", port, workers,
            g_body_size, g_keep_alive ? "yes" : "no");

    vector<epoll_event> evs(1024);
    for (;;) {
        int n = epoll_wait(g_epoll_fd, evs.data(), (int)evs.size(), -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (evs[i].data.ptr == nullptr) {
                for (;;) {
                    int cfd = accept(lfd, nullptr, nullptr);
                    if (cfd < 0) break;
                    int f = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, f | O_NONBLOCK);
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    Conn* c = new Conn();
                    c->fd = cfd;
                    g_connections++;
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
                    ev.data.ptr = c;
                    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
                        close(cfd);
                        delete c;
                    }
                }
            } else {
                Conn* c = static_cast<Conn*>(evs[i].data.ptr);
                q.push([c] { handle(c); });
            }
        }
    }
    q.stop();
    for (auto& t : pool) if (t.joinable()) t.join();
    return 0;
}
