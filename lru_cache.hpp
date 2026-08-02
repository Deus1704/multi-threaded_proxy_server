// lru_cache.hpp
// Shared by the server and the tests so the tests actually cover what ships.
// (Previously the cache was copy-pasted into test_lru_cache.cpp, which meant
// the tests could pass while the server's copy was broken.)
//
//   LRUCache        - one mutex, exact LRU
//   ShardedLRUCache - N shards, key hashed to a shard. Approximate LRU, far
//                     less lock contention.
//
// list + unordered_map because list::splice is O(1) and doesn't invalidate
// iterators, and the map holds iterators into the list. A vector would
// invalidate them on every move.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

template <typename K, typename V>
class LRUCache {
    struct Node {
        K key;
        V value;
    };

    size_t capacity_;
    std::list<Node> items_;                                          // front = MRU, back = LRU
    std::unordered_map<K, typename std::list<Node>::iterator> index_; // key -> node
    mutable std::mutex mtx_;
    // Counters are plain integers guarded by mtx_, not atomics: every read and
    // write already holds the lock, so atomics would only add cost. Reading
    // them together under one lock also makes hits/misses/size a consistent
    // snapshot instead of three independently-racing values.
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    uint64_t evictions_ = 0;

public:
    explicit LRUCache(size_t cap) : capacity_(cap == 0 ? 1 : cap) {}

    // O(1). Takes an exclusive lock because a read MUTATES recency order.
    // (A shared_mutex would buy nothing here, see ShardedLRUCache for how
    // read concurrency is actually recovered.)
    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(key);
        if (it == index_.end()) {
            misses_++;
            return std::nullopt;
        }
        hits_++;
        items_.splice(items_.begin(), items_, it->second); // O(1), iterator stays valid
        return it->second->value;
    }

    // O(1) amortised. Evicts from the back when at capacity.
    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = value;
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        if (items_.size() >= capacity_) {
            index_.erase(items_.back().key);
            items_.pop_back();
            evictions_++;
        }
        items_.push_front({key, value});
        index_[key] = items_.begin();
    }

    bool erase(const K& key) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        items_.erase(it->second);
        index_.erase(it);
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return items_.size();
    }

    size_t capacity() const { return capacity_; }

    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        size_t size = 0;
        double hit_rate_pct = 0.0;
    };

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mtx_);
        Stats s;
        s.hits = hits_;
        s.misses = misses_;
        s.evictions = evictions_;
        s.size = items_.size();
        uint64_t total = hits_ + misses_;
        s.hit_rate_pct = total ? (double)hits_ / (double)total * 100.0 : 0.0;
        return s;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mtx_);
        items_.clear();
        index_.clear();
        hits_ = misses_ = evictions_ = 0;
    }
};

// N-way sharded cache. Each shard is an independent exact-LRU with its own
// mutex, so P threads touching different shards never serialise.
//
// Trade-off, stated plainly: eviction is per-shard LRU, not global LRU. A
// globally-hot key can be evicted while a globally-cold key survives in a
// quieter shard. With a decent hash the shards stay balanced and the hit-rate
// cost is small, test_lru_cache.cpp measures exactly how small rather than
// assuming it.
template <typename K, typename V, typename Hash = std::hash<K>>
class ShardedLRUCache {
    std::vector<std::unique_ptr<LRUCache<K, V>>> shards_;
    size_t shard_mask_;
    Hash hasher_;

    static size_t round_up_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    LRUCache<K, V>& shard_for(const K& key) {
        // Mixing step: std::hash on integers is often the identity, which would
        // send every key to shard 0 once we mask off the low bits.
        size_t h = hasher_(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return *shards_[h & shard_mask_];
    }

public:
    // total_capacity is split evenly across shards.
    ShardedLRUCache(size_t total_capacity, size_t num_shards = 16) {
        size_t n = round_up_pow2(num_shards == 0 ? 1 : num_shards);
        shard_mask_ = n - 1;
        size_t per_shard = total_capacity / n;
        if (per_shard == 0) per_shard = 1;
        shards_.reserve(n);
        for (size_t i = 0; i < n; i++)
            shards_.push_back(std::make_unique<LRUCache<K, V>>(per_shard));
    }

    std::optional<V> get(const K& key) { return shard_for(key).get(key); }
    void put(const K& key, const V& value) { shard_for(key).put(key, value); }
    bool erase(const K& key) { return shard_for(key).erase(key); }
    size_t num_shards() const { return shards_.size(); }

    using Stats = typename LRUCache<K, V>::Stats;

    Stats stats() const {
        Stats agg;
        for (auto& s : shards_) {
            auto st = s->stats();
            agg.hits += st.hits;
            agg.misses += st.misses;
            agg.evictions += st.evictions;
            agg.size += st.size;
        }
        uint64_t total = agg.hits + agg.misses;
        agg.hit_rate_pct = total ? (double)agg.hits / (double)total * 100.0 : 0.0;
        return agg;
    }

    size_t size() const { return stats().size; }

    size_t capacity() const {
        size_t c = 0;
        for (auto& s : shards_) c += s->capacity();
        return c;
    }

    void reset() {
        for (auto& s : shards_) s->reset();
    }
};
