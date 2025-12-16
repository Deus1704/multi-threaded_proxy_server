// Phase 4: Adding thread-safe LRU cache
// Hash map for O(1) lookup + doubly linked list for O(1) eviction ordering

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <shared_mutex>  // C++17 for reader-writer lock
#include <condition_variable>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <list>
#include <chrono>
#include <optional>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

using namespace std;

// ============================================================================
// LRU CACHE - thread-safe with reader-writer lock
// ============================================================================
// Why doubly linked list + hash map?
// - List: O(1) insert/remove at any position, maintains access order
// - Map: O(1) key lookup to find node in list
// - Together: O(1) get, O(1) put, O(1) eviction

template<typename K, typename V>
class LRUCache {
    struct Node {
        K key;
        V value;
    };
    
    size_t capacity;
    list<Node> items;                                    // front = most recent
    unordered_map<K, typename list<Node>::iterator> index;  // key -> list iterator
    
    mutable shared_mutex mtx;  // allows multiple readers OR one writer
    
    // stats for benchmarking
    atomic<uint64_t> hits{0};
    atomic<uint64_t> misses{0};

public:
    explicit LRUCache(size_t cap) : capacity(cap) {}

    // returns nullopt on miss
    optional<V> get(const K& key) {
        // try read lock first (optimistic)
        {
            shared_lock<shared_mutex> read_lock(mtx);
            auto it = index.find(key);
            if (it == index.end()) {
                misses++;
                return nullopt;
            }
        }
        
        // found it - need write lock to move to front
        unique_lock<shared_mutex> write_lock(mtx);
        auto it = index.find(key);
        if (it == index.end()) {
            // someone else evicted it between locks (rare but possible)
            misses++;
            return nullopt;
        }
        
        hits++;
        
        // move to front (most recently used)
        items.splice(items.begin(), items, it->second);
        return it->second->value;
    }

    void put(const K& key, const V& value) {
        unique_lock<shared_mutex> lock(mtx);
        
        auto it = index.find(key);
        if (it != index.end()) {
            // update existing - move to front
            it->second->value = value;
            items.splice(items.begin(), items, it->second);
            return;
        }
        
        // evict if at capacity
        if (items.size() >= capacity) {
            auto& lru = items.back();  // least recently used
            index.erase(lru.key);
            items.pop_back();
        }
        
        // insert new at front
        items.push_front({key, value});
        index[key] = items.begin();
    }

    // for benchmarking
    double hit_rate() const {
        uint64_t h = hits.load();
        uint64_t m = misses.load();
        return (h + m) > 0 ? (double)h / (h + m) * 100.0 : 0.0;
    }

    void reset_stats() {
        hits = 0;
        misses = 0;
    }

    size_t size() const {
        shared_lock<shared_mutex> lock(mtx);
        return items.size();
    }

    pair<uint64_t, uint64_t> stats() const {
        return {hits.load(), misses.load()};
    }
};

// ============================================================================
// TASK QUEUE - same as phase 3
// ============================================================================
class TaskQueue {
    queue<function<void()>> tasks;
    mutex mtx;
    condition_variable cv;
    atomic<bool> shutdown{false};

public:
    void push(function<void()> task) {
        {
            lock_guard<mutex> lock(mtx);
            tasks.push(move(task));
        }
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

    void stop() {
        shutdown = true;
        cv.notify_all();
    }
};

// ============================================================================
// THREAD POOL
// ============================================================================
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
        cout << "[pool] " << n << " workers ready\n";
    }

    void shutdown() {
        running = false;
        queue.stop();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }

    ~ThreadPool() { shutdown(); }
};

// ============================================================================
// GLOBAL CACHE - shared across all workers
// ============================================================================
LRUCache<string, string> response_cache(1000);  // cache up to 1000 responses

// ============================================================================
// UTILITIES
// ============================================================================
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

string build_response(const string& body) {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/plain\r\n"
           "Content-Length: " + to_string(body.length()) + "\r\n"
           "Connection: keep-alive\r\n"
           "\r\n" + body;
}

// extract URL path from HTTP request
string extract_path(const string& request) {
    // GET /path HTTP/1.1
    size_t start = request.find(' ');
    if (start == string::npos) return "/";
    size_t end = request.find(' ', start + 1);
    if (end == string::npos) return "/";
    return request.substr(start + 1, end - start - 1);
}

// simulate fetching from upstream (expensive operation)
string fetch_from_upstream(const string& path) {
    // simulate network latency
    this_thread::sleep_for(chrono::milliseconds(50));
    return "Response for: " + path + " (fetched at " + 
           to_string(chrono::system_clock::now().time_since_epoch().count()) + ")\n";
}

// ============================================================================
// CONNECTION HANDLER - now with caching
// ============================================================================
void handle_request(int client_fd) {
    char buffer[4096] = {0};
    int bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes <= 0) {
        close(client_fd);
        return;
    }
    
    string request(buffer, bytes);
    string path = extract_path(request);
    
    string body;
    
    // check cache first
    auto cached = response_cache.get(path);
    if (cached.has_value()) {
        body = cached.value() + " [CACHE HIT]";
    } else {
        // cache miss - fetch and store
        body = fetch_from_upstream(path);
        response_cache.put(path, body);
        body += " [CACHE MISS]";
    }
    
    string response = build_response(body);
    send(client_fd, response.c_str(), response.length(), 0);
    
    // close after response (HTTP/1.0 style for simplicity)
    close(client_fd);
}

// ============================================================================
// STATS PRINTER - periodic cache stats
// ============================================================================
void stats_printer() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(5));
        auto [hits, misses] = response_cache.stats();
        cout << "[cache] hits: " << hits << ", misses: " << misses 
             << ", hit rate: " << response_cache.hit_rate() << "%"
             << ", size: " << response_cache.size() << "\n";
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
    int num_workers = thread::hardware_concurrency();
    if (num_workers == 0) num_workers = 4;
    
    TaskQueue task_queue;
    ThreadPool pool(task_queue, num_workers);
    
    // start stats printer in background
    thread stats_thread(stats_printer);
    stats_thread.detach();
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    listen(listen_fd, SOMAXCONN);
    set_nonblocking(listen_fd);
    
    int epoll_fd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
    
    cout << "Server on port " << port << " with " << num_workers << " workers\n";
    cout << "Cache capacity: 1000 entries\n\n";
    
    epoll_event events[64];
    
    while (true) {
        int n = epoll_wait(epoll_fd, events, 64, -1);
        
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            
            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client{};
                    socklen_t len = sizeof(client);
                    int client_fd = accept(listen_fd, (sockaddr*)&client, &len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        continue;
                    }
                    
                    set_nonblocking(client_fd);
                    
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
            } else {
                // remove from epoll before dispatching (one-shot behavior)
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                task_queue.push([fd] { handle_request(fd); });
            }
        }
    }
    
    return 0;
}

