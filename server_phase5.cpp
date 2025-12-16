// Phase 5: Full proxy with forwarding logic
// Actually proxies requests to upstream servers

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <list>
#include <chrono>
#include <optional>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

using namespace std;

// ============================================================================
// LRU CACHE
// ============================================================================
template<typename K, typename V>
class LRUCache {
    struct Node { K key; V value; };
    size_t capacity;
    list<Node> items;
    unordered_map<K, typename list<Node>::iterator> index;
    mutable shared_mutex mtx;
    atomic<uint64_t> hits{0}, misses{0};

public:
    explicit LRUCache(size_t cap) : capacity(cap) {}

    optional<V> get(const K& key) {
        unique_lock<shared_mutex> lock(mtx);
        auto it = index.find(key);
        if (it == index.end()) { misses++; return nullopt; }
        hits++;
        items.splice(items.begin(), items, it->second);
        return it->second->value;
    }

    void put(const K& key, const V& value) {
        unique_lock<shared_mutex> lock(mtx);
        auto it = index.find(key);
        if (it != index.end()) {
            it->second->value = value;
            items.splice(items.begin(), items, it->second);
            return;
        }
        if (items.size() >= capacity) {
            index.erase(items.back().key);
            items.pop_back();
        }
        items.push_front({key, value});
        index[key] = items.begin();
    }

    double hit_rate() const {
        uint64_t h = hits, m = misses;
        return (h + m) > 0 ? (double)h / (h + m) * 100.0 : 0.0;
    }
    
    tuple<uint64_t, uint64_t, size_t> stats() const {
        shared_lock<shared_mutex> lock(mtx);
        return {hits.load(), misses.load(), items.size()};
    }
};

// ============================================================================
// TASK QUEUE + THREAD POOL
// ============================================================================
class TaskQueue {
    queue<function<void()>> tasks;
    mutex mtx;
    condition_variable cv;
    atomic<bool> shutdown{false};

public:
    void push(function<void()> task) {
        { lock_guard<mutex> lock(mtx); tasks.push(move(task)); }
        cv.notify_one();
    }

    function<void()> pop() {
        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [this] { return !tasks.empty() || shutdown; });
        if (shutdown && tasks.empty()) return nullptr;
        auto task = move(tasks.front());
        tasks.pop();
        return task;
    }

    void stop() { shutdown = true; cv.notify_all(); }
};

class ThreadPool {
    vector<thread> workers;
    TaskQueue& queue;
    atomic<bool> running{true};

public:
    ThreadPool(TaskQueue& q, int n) : queue(q) {
        for (int i = 0; i < n; i++) {
            workers.emplace_back([this] {
                while (running) {
                    auto task = queue.pop();
                    if (!task) break;
                    task();
                }
            });
        }
    }
    void shutdown() {
        running = false;
        queue.stop();
        for (auto& w : workers) if (w.joinable()) w.join();
    }
    ~ThreadPool() { shutdown(); }
};

// ============================================================================
// CONNECTION STATS
// ============================================================================
struct Stats {
    atomic<uint64_t> total_connections{0};
    atomic<uint64_t> active_connections{0};
    atomic<uint64_t> total_requests{0};
    atomic<uint64_t> forwarded_requests{0};
    atomic<uint64_t> errors{0};
} stats;

// ============================================================================
// GLOBAL CACHE
// ============================================================================
LRUCache<string, string> cache(2000);

// ============================================================================
// HTTP PARSING - minimal but functional
// ============================================================================
struct HttpRequest {
    string method;
    string path;
    string host;
    string full_request;
    bool valid = false;
};

HttpRequest parse_request(const string& raw) {
    HttpRequest req;
    req.full_request = raw;
    
    istringstream stream(raw);
    string line;
    
    // first line: GET /path HTTP/1.1
    if (getline(stream, line)) {
        // trim \r if present
        if (!line.empty() && line.back() == '\r') line.pop_back();
        
        size_t sp1 = line.find(' ');
        size_t sp2 = line.rfind(' ');
        if (sp1 != string::npos && sp2 != sp1) {
            req.method = line.substr(0, sp1);
            req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            req.valid = true;
        }
    }
    
    // parse headers for Host
    while (getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        
        if (line.substr(0, 5) == "Host:") {
            req.host = line.substr(6);
            // trim leading space
            while (!req.host.empty() && req.host[0] == ' ') 
                req.host = req.host.substr(1);
        }
    }
    
    return req;
}

// ============================================================================
// UPSTREAM CONNECTION - connect to origin server
// ============================================================================
string forward_to_upstream(const string& host, int port, const string& request) {
    // resolve hostname
    struct hostent* server = gethostbyname(host.c_str());
    if (!server) return "";
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    
    // set timeout
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    sockaddr_in upstream{};
    upstream.sin_family = AF_INET;
    upstream.sin_port = htons(port);
    memcpy(&upstream.sin_addr.s_addr, server->h_addr, server->h_length);
    
    if (connect(sock, (sockaddr*)&upstream, sizeof(upstream)) < 0) {
        close(sock);
        return "";
    }
    
    // send request
    send(sock, request.c_str(), request.length(), 0);
    
    // read response
    string response;
    char buffer[8192];
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, bytes);
    }
    
    close(sock);
    stats.forwarded_requests++;
    return response;
}

// ============================================================================
// REQUEST HANDLER
// ============================================================================
void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

string make_error_response(int code, const string& msg) {
    string body = to_string(code) + " " + msg;
    return "HTTP/1.1 " + body + "\r\n"
           "Content-Type: text/plain\r\n"
           "Content-Length: " + to_string(body.length()) + "\r\n"
           "\r\n" + body;
}

void handle_client(int fd) {
    stats.total_requests++;
    
    char buffer[8192] = {0};
    int bytes = recv(fd, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes <= 0) {
        close(fd);
        stats.active_connections--;
        return;
    }
    
    HttpRequest req = parse_request(string(buffer, bytes));
    
    if (!req.valid) {
        string resp = make_error_response(400, "Bad Request");
        send(fd, resp.c_str(), resp.length(), 0);
        close(fd);
        stats.active_connections--;
        stats.errors++;
        return;
    }
    
    // cache key = host + path
    string cache_key = req.host + req.path;
    
    // check cache
    auto cached = cache.get(cache_key);
    if (cached) {
        send(fd, cached->c_str(), cached->length(), 0);
        close(fd);
        stats.active_connections--;
        return;
    }
    
    // forward to upstream
    // extract port from host if present
    string upstream_host = req.host;
    int upstream_port = 80;
    size_t colon = upstream_host.find(':');
    if (colon != string::npos) {
        upstream_port = stoi(upstream_host.substr(colon + 1));
        upstream_host = upstream_host.substr(0, colon);
    }
    
    string response = forward_to_upstream(upstream_host, upstream_port, req.full_request);
    
    if (response.empty()) {
        string resp = make_error_response(502, "Bad Gateway");
        send(fd, resp.c_str(), resp.length(), 0);
        stats.errors++;
    } else {
        // cache successful responses
        cache.put(cache_key, response);
        send(fd, response.c_str(), response.length(), 0);
    }
    
    close(fd);
    stats.active_connections--;
}

// ============================================================================
// STATS REPORTER
// ============================================================================
void report_stats() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(5));
        auto [hits, misses, size] = cache.stats();
        cout << "\n=== Stats ===" << endl;
        cout << "Connections: " << stats.total_connections << " total, " 
             << stats.active_connections << " active" << endl;
        cout << "Requests: " << stats.total_requests << " total, "
             << stats.forwarded_requests << " forwarded, "
             << stats.errors << " errors" << endl;
        cout << "Cache: " << hits << " hits, " << misses << " misses, "
             << cache.hit_rate() << "% hit rate, " << size << " entries\n";
    }
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }
    
    int port = atoi(argv[1]);
    int workers = thread::hardware_concurrency();
    if (workers == 0) workers = 4;
    
    TaskQueue tasks;
    ThreadPool pool(tasks, workers);
    
    thread(report_stats).detach();
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, SOMAXCONN);
    set_nonblocking(listen_fd);
    
    int epoll_fd = epoll_create1(0);
    epoll_event ev{EPOLLIN, {.fd = listen_fd}};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
    
    cout << "Proxy server on :" << port << " with " << workers << " workers\n";
    cout << "Cache: 2000 entries max\n\n";
    
    epoll_event events[128];
    
    while (true) {
        int n = epoll_wait(epoll_fd, events, 128, -1);
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                while (true) {
                    sockaddr_in client{};
                    socklen_t len = sizeof(client);
                    int client_fd = accept(listen_fd, (sockaddr*)&client, &len);
                    if (client_fd < 0) break;
                    
                    stats.total_connections++;
                    stats.active_connections++;
                    set_nonblocking(client_fd);
                    
                    epoll_event cev{EPOLLIN | EPOLLET, {.fd = client_fd}};
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
            } else {
                int fd = events[i].data.fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                tasks.push([fd] { handle_client(fd); });
            }
        }
    }
}

