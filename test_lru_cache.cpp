// LRU Cache stress test
// Verify thread safety and measure performance under concurrent access

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <cassert>
#include <unordered_map>
#include <list>
#include <shared_mutex>
#include <optional>

using namespace std;
using namespace chrono;

// ============================================================================
// LRU CACHE (same implementation as server)
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

    size_t size() const {
        shared_lock<shared_mutex> lock(mtx);
        return items.size();
    }

    double hit_rate() const {
        uint64_t h = hits, m = misses;
        return (h + m) > 0 ? (double)h / (h + m) * 100.0 : 0.0;
    }

    tuple<uint64_t, uint64_t> stats() const {
        return {hits.load(), misses.load()};
    }

    void reset() {
        unique_lock<shared_mutex> lock(mtx);
        items.clear();
        index.clear();
        hits = misses = 0;
    }
};

// ============================================================================
// TEST 1: Basic functionality
// ============================================================================
void test_basic() {
    cout << "Test 1: Basic operations... ";
    
    LRUCache<int, string> cache(3);
    
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    
    assert(cache.get(1).value() == "one");
    assert(cache.get(2).value() == "two");
    assert(cache.size() == 3);
    
    // add 4th, should evict 3 (least recently used after we accessed 1,2)
    cache.put(4, "four");
    assert(cache.size() == 3);
    assert(!cache.get(3).has_value());  // should be evicted
    assert(cache.get(4).value() == "four");
    
    cout << "PASS" << endl;
}

// ============================================================================
// TEST 2: LRU ordering
// ============================================================================
void test_lru_ordering() {
    cout << "Test 2: LRU eviction order... ";
    
    LRUCache<int, int> cache(3);
    
    cache.put(1, 100);
    cache.put(2, 200);
    cache.put(3, 300);
    
    // access 1, making it most recent
    cache.get(1);
    
    // add 4, should evict 2 (now least recent)
    cache.put(4, 400);
    
    assert(cache.get(1).has_value());  // still there
    assert(!cache.get(2).has_value()); // evicted
    assert(cache.get(3).has_value());  // still there
    assert(cache.get(4).has_value());  // just added
    
    cout << "PASS" << endl;
}

// ============================================================================
// TEST 3: Update existing key
// ============================================================================
void test_update() {
    cout << "Test 3: Update existing... ";
    
    LRUCache<string, int> cache(2);
    
    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("a", 10);  // update, not insert
    
    assert(cache.size() == 2);  // still 2, not 3
    assert(cache.get("a").value() == 10);
    
    cout << "PASS" << endl;
}

// ============================================================================
// TEST 4: Concurrent stress test
// ============================================================================
void test_concurrent() {
    cout << "Test 4: Concurrent access (" << flush;
    
    LRUCache<int, string> cache(100);
    atomic<int> operations{0};
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 10000;
    
    auto worker = [&](int id) {
        random_device rd;
        mt19937 gen(rd() + id);
        uniform_int_distribution<> key_dist(0, 200);  // more keys than capacity
        
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            int key = key_dist(gen);
            
            if (gen() % 3 == 0) {
                // 33% writes
                cache.put(key, "value_" + to_string(key));
            } else {
                // 67% reads
                cache.get(key);
            }
            operations++;
        }
    };
    
    auto start = high_resolution_clock::now();
    
    vector<thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();
    
    auto end = high_resolution_clock::now();
    double ms = duration_cast<milliseconds>(end - start).count();
    double ops_per_sec = operations / (ms / 1000.0);
    
    cout << operations << " ops, " << ops_per_sec << " ops/sec)... ";
    
    // if we got here without crash/deadlock, it's thread-safe
    assert(cache.size() <= 100);  // never exceeded capacity
    
    cout << "PASS" << endl;
}

// ============================================================================
// TEST 5: Hit rate under zipf distribution
// ============================================================================
void test_hit_rate_zipf() {
    cout << "Test 5: Hit rate (zipf distribution)... ";
    
    const int CACHE_SIZE = 100;
    const int NUM_KEYS = 1000;
    const int NUM_OPS = 50000;
    
    LRUCache<int, int> cache(CACHE_SIZE);
    
    random_device rd;
    mt19937 gen(rd());
    
    // zipf-like: favor lower keys
    vector<double> weights(NUM_KEYS);
    for (int i = 0; i < NUM_KEYS; i++) {
        weights[i] = 1.0 / (i + 1);
    }
    discrete_distribution<> dist(weights.begin(), weights.end());
    
    for (int i = 0; i < NUM_OPS; i++) {
        int key = dist(gen);
        auto val = cache.get(key);
        if (!val) {
            cache.put(key, key * 10);
        }
    }
    
    auto [hits, misses] = cache.stats();
    double rate = cache.hit_rate();
    
    cout << "hits=" << hits << " misses=" << misses 
         << " rate=" << rate << "%... ";
    
    // with zipf and 10% cache size, expect decent hit rate
    assert(rate > 50.0);  // should get at least 50%
    
    cout << "PASS" << endl;
}

// ============================================================================
// TEST 6: Throughput benchmark
// ============================================================================
void test_throughput() {
    cout << "Test 6: Throughput benchmark... " << flush;
    
    LRUCache<int, string> cache(1000);
    
    // pre-populate
    for (int i = 0; i < 1000; i++) {
        cache.put(i, string(100, 'x'));  // 100-byte values
    }
    
    const int ITERS = 100000;
    
    // read-heavy workload
    auto start = high_resolution_clock::now();
    for (int i = 0; i < ITERS; i++) {
        cache.get(i % 1000);
    }
    auto end = high_resolution_clock::now();
    
    double us = duration_cast<microseconds>(end - start).count();
    double reads_per_sec = ITERS / (us / 1e6);
    
    // write workload
    start = high_resolution_clock::now();
    for (int i = 0; i < ITERS; i++) {
        cache.put(i % 1000, string(100, 'y'));
    }
    end = high_resolution_clock::now();
    
    us = duration_cast<microseconds>(end - start).count();
    double writes_per_sec = ITERS / (us / 1e6);
    
    cout << endl;
    cout << "       Single-thread reads:  " << reads_per_sec << " ops/sec" << endl;
    cout << "       Single-thread writes: " << writes_per_sec << " ops/sec" << endl;
    
    cout << "       PASS" << endl;
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    cout << "=== LRU Cache Tests ===" << endl << endl;
    
    test_basic();
    test_lru_ordering();
    test_update();
    test_concurrent();
    test_hit_rate_zipf();
    test_throughput();
    
    cout << endl << "All tests passed!" << endl;
    return 0;
}

