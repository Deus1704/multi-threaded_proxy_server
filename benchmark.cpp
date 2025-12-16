// Benchmark: concurrent connections + cache hit rates
// Run against the proxy server to measure performance

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;
using namespace chrono;

// ============================================================================
// CONFIG
// ============================================================================
struct Config {
    string host = "127.0.0.1";
    int port = 8080;
    int num_threads = 10;           // concurrent clients
    int requests_per_thread = 100;  // requests each client makes
    int num_unique_urls = 50;       // smaller = higher cache hit rate
    int think_time_ms = 0;          // delay between requests
};

// ============================================================================
// STATS
// ============================================================================
struct BenchStats {
    atomic<int> successful{0};
    atomic<int> failed{0};
    atomic<int> connections_opened{0};
    atomic<long long> total_latency_us{0};  // microseconds
    atomic<int> latency_samples{0};
};

BenchStats stats;

// ============================================================================
// HTTP CLIENT - minimal, one request per connection
// ============================================================================
bool make_request(const string& host, int port, const string& path, int* latency_us) {
    auto start = high_resolution_clock::now();
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }
    
    stats.connections_opened++;
    
    // send request
    string request = "GET " + path + " HTTP/1.1\r\n"
                    "Host: " + host + "\r\n"
                    "Connection: close\r\n"
                    "\r\n";
    
    send(sock, request.c_str(), request.length(), 0);
    
    // read response
    char buffer[4096];
    int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);
    
    auto end = high_resolution_clock::now();
    *latency_us = duration_cast<microseconds>(end - start).count();
    
    return bytes > 0;
}

// ============================================================================
// WORKER THREAD - simulates one client
// ============================================================================
void worker(const Config& cfg, int thread_id) {
    // generate predictable URL sequence with some repetition
    random_device rd;
    mt19937 gen(rd() + thread_id);
    
    // zipf-like distribution: some URLs way more popular
    // weights favor lower indices
    vector<double> weights(cfg.num_unique_urls);
    for (int i = 0; i < cfg.num_unique_urls; i++) {
        weights[i] = 1.0 / (i + 1);  // zipf
    }
    discrete_distribution<> dist(weights.begin(), weights.end());
    
    for (int i = 0; i < cfg.requests_per_thread; i++) {
        int url_id = dist(gen);
        string path = "/resource/" + to_string(url_id);
        
        int latency_us = 0;
        if (make_request(cfg.host, cfg.port, path, &latency_us)) {
            stats.successful++;
            stats.total_latency_us += latency_us;
            stats.latency_samples++;
        } else {
            stats.failed++;
        }
        
        if (cfg.think_time_ms > 0) {
            this_thread::sleep_for(milliseconds(cfg.think_time_ms));
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    Config cfg;
    
    // parse args
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-h" && i + 1 < argc) cfg.host = argv[++i];
        else if (arg == "-p" && i + 1 < argc) cfg.port = stoi(argv[++i]);
        else if (arg == "-t" && i + 1 < argc) cfg.num_threads = stoi(argv[++i]);
        else if (arg == "-n" && i + 1 < argc) cfg.requests_per_thread = stoi(argv[++i]);
        else if (arg == "-u" && i + 1 < argc) cfg.num_unique_urls = stoi(argv[++i]);
        else if (arg == "-d" && i + 1 < argc) cfg.think_time_ms = stoi(argv[++i]);
        else {
            cout << "Usage: " << argv[0] << " [options]\n"
                 << "  -h <host>    Target host (default: 127.0.0.1)\n"
                 << "  -p <port>    Target port (default: 8080)\n"
                 << "  -t <threads> Concurrent clients (default: 10)\n"
                 << "  -n <reqs>    Requests per client (default: 100)\n"
                 << "  -u <urls>    Unique URLs, smaller = more cache hits (default: 50)\n"
                 << "  -d <ms>      Think time between requests (default: 0)\n";
            return 1;
        }
    }
    
    cout << "=== Benchmark Configuration ===" << endl;
    cout << "Target: " << cfg.host << ":" << cfg.port << endl;
    cout << "Threads: " << cfg.num_threads << endl;
    cout << "Requests/thread: " << cfg.requests_per_thread << endl;
    cout << "Unique URLs: " << cfg.num_unique_urls << endl;
    cout << "Total requests: " << cfg.num_threads * cfg.requests_per_thread << endl;
    cout << endl;
    
    auto start = high_resolution_clock::now();
    
    // spawn workers
    vector<thread> threads;
    for (int i = 0; i < cfg.num_threads; i++) {
        threads.emplace_back(worker, ref(cfg), i);
    }
    
    // wait for completion
    for (auto& t : threads) t.join();
    
    auto end = high_resolution_clock::now();
    double elapsed_sec = duration_cast<milliseconds>(end - start).count() / 1000.0;
    
    // results
    int total = stats.successful + stats.failed;
    double rps = total / elapsed_sec;
    double avg_latency_ms = stats.latency_samples > 0 
        ? (stats.total_latency_us / stats.latency_samples) / 1000.0 
        : 0;
    
    cout << "=== Results ===" << endl;
    cout << "Duration: " << elapsed_sec << "s" << endl;
    cout << "Requests: " << stats.successful << " ok, " << stats.failed << " failed" << endl;
    cout << "Throughput: " << rps << " req/s" << endl;
    cout << "Avg latency: " << avg_latency_ms << "ms" << endl;
    cout << "Connections opened: " << stats.connections_opened << endl;
    cout << endl;
    cout << "(Check server logs for cache hit rate)" << endl;
    
    return stats.failed > 0 ? 1 : 0;
}

