// Phase 3: epoll + thread pool
// Moving from thread-per-connection (wasteful) to event-driven + worker pool

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

using namespace std;

// ============================================================================
// TASK QUEUE - synchronized queue for thread pool
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
        cv.notify_one();  // wake up one waiting worker
    }

    // blocks until task available or shutdown
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
        cv.notify_all();  // wake all workers so they can exit
    }

    size_t size() {
        lock_guard<mutex> lock(mtx);
        return tasks.size();
    }
};

// ============================================================================
// THREAD POOL - fixed number of workers pulling from task queue
// ============================================================================
class ThreadPool {
    vector<thread> workers;
    TaskQueue& queue;
    atomic<bool> running{true};

public:
    ThreadPool(TaskQueue& q, int num_workers) : queue(q) {
        for (int i = 0; i < num_workers; i++) {
            workers.emplace_back([this, i] {
                cout << "[worker " << i << "] started\n";
                while (running) {
                    auto task = queue.pop();
                    if (!task) break;  // shutdown signal
                    task();
                }
                cout << "[worker " << i << "] exiting\n";
            });
        }
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
// UTILITIES
// ============================================================================

// make socket non-blocking (required for epoll edge-triggered mode)
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// simple HTTP response - will add caching later
string build_response(const string& body) {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/plain\r\n"
           "Content-Length: " + to_string(body.length()) + "\r\n"
           "Connection: keep-alive\r\n"
           "\r\n" + body;
}

// ============================================================================
// CONNECTION HANDLER - runs in thread pool
// ============================================================================
void handle_request(int client_fd, const string& client_ip) {
    char buffer[4096] = {0};
    
    // read request (non-blocking, so might get partial data)
    int bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes <= 0) {
        // client disconnected or error
        close(client_fd);
        return;
    }
    
    buffer[bytes] = '\0';
    
    // extract first line for logging
    string request(buffer);
    size_t first_line_end = request.find("\r\n");
    string first_line = (first_line_end != string::npos) 
                        ? request.substr(0, first_line_end) 
                        : request;
    
    cout << "[" << client_ip << "] " << first_line << "\n";
    
    // for now, echo back a simple response
    string response = build_response("Hello from thread pool server!\n");
    send(client_fd, response.c_str(), response.length(), 0);
    
    // keep-alive: don't close socket, let epoll handle it
    // close(client_fd);  // uncomment for HTTP/1.0 behavior
}

// ============================================================================
// MAIN - epoll event loop
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }
    
    int port = atoi(argv[1]);
    int num_workers = 4;  // tune based on CPU cores
    
    // setup task queue and thread pool
    TaskQueue task_queue;
    ThreadPool pool(task_queue, num_workers);
    
    // create listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket failed");
        return 1;
    }
    
    // allow port reuse (helpful during development)
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }
    
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        return 1;
    }
    
    set_nonblocking(listen_fd);
    
    // create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        return 1;
    }
    
    // add listening socket to epoll
    epoll_event ev{};
    ev.events = EPOLLIN;  // level-triggered for accept
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
    
    cout << "Server listening on port " << port << " with " << num_workers << " workers\n";
    
    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];
    
    // main event loop
    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            
            if (fd == listen_fd) {
                // new connection(s) - accept all pending
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // no more pending connections
                        }
                        perror("accept failed");
                        break;
                    }
                    
                    set_nonblocking(client_fd);
                    
                    // add client to epoll with edge-triggered mode
                    // EPOLLET = edge triggered, more efficient but trickier
                    epoll_event client_ev{};
                    client_ev.events = EPOLLIN | EPOLLET;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                    
                    string client_ip = inet_ntoa(client_addr.sin_addr);
                    cout << "[new connection] " << client_ip << ":" << ntohs(client_addr.sin_port) << "\n";
                }
            } else {
                // existing client has data - dispatch to thread pool
                // capture fd and ip for the lambda
                string client_ip = "client";  // simplified, could store in map
                task_queue.push([fd, client_ip] {
                    handle_request(fd, client_ip);
                });
            }
        }
    }
    
    close(listen_fd);
    close(epoll_fd);
    return 0;
}

