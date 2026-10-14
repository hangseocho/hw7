// bench_containers.cpp — Session 6 / HW 7 lab (labs/week07.md, Steps 1-5).
//
// One order book, five representations, three questions:
//   Q1  look up an order by id        (cancel-by-id)
//   Q2  look up a price level by price (add: "which level does this go into?")
//   Q3  know the best prices           (best_bid() + best_ask())
//
// Standalone: C++20, standard library only, no Makefile target. From the starter root:
//
//   g++ -std=c++20 -O2 starters/week07/bench_containers.cpp -o /tmp/bench_containers && /tmp/bench_containers
//
// Add -DSTUDENT_BOOK -Iinclude to also time YOUR include/order_book.hpp Book:
//
//   g++ -std=c++20 -O2 -DSTUDENT_BOOK -Iinclude starters/week07/bench_containers.cpp -o /tmp/bench_containers && /tmp/bench_containers
//
// Optional argument: a single N to run (default: 64 1024 16384).
//
// What is measured (per structure, per N):
//   add     build the book from empty with N orders, mean ns per add (avg size N/2)
//   cancel  cancel all N orders in random order, mean ns per cancel
//   bbo     best_bid() + best_ask() on a churned book of N resting orders, ns per call
//   p50/p99 of add and cancel, over batches of 32 consecutive ops (ns/op per batch;
//           the clock is too coarse on Apple Silicon to time one 10 ns op alone)
// Prices sit on a one-cent grid, 256 levels per side around $100 (bids 99.99 down,
// asks 100.01 up), qty 1..9, fixed seeds so two runs see the same orders.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <forward_list>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef STUDENT_BOOK
#include "order_book.hpp"
#endif

// ---------------------------------------------------------------- harness ----
template <class T> inline void keep(T&& v) { asm volatile("" : : "g"(v) : "memory"); }
using clk = std::chrono::steady_clock;
static inline double ns_since(clk::time_point t0) {
    return std::chrono::duration<double, std::nano>(clk::now() - t0).count();
}
static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    size_t k = static_cast<size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

struct Ord { uint64_t id; char side; double px; uint32_t qty; };

static constexpr int LEVELS = 256;   // price levels per side in the generated flow
static constexpr size_t BATCH = 32; // ops per timed batch for the p50/p99 columns

static std::vector<Ord> make_orders(size_t n, uint64_t seed, uint64_t first_id = 0) {
    std::mt19937_64 rng(seed);
    std::vector<Ord> v(n);
    for (size_t i = 0; i < n; ++i) {
        int k = static_cast<int>(rng() % LEVELS);
        bool bid = (i & 1) == 0;
        int tick = bid ? 9999 - k : 10001 + k;          // one-cent ticks around $100
        v[i] = {first_id + i, bid ? 'B' : 'S', tick * 0.01, static_cast<uint32_t>(1 + rng() % 9)};
    }
    return v;
}
static std::vector<uint64_t> shuffled_ids(const std::vector<Ord>& o, uint64_t seed) {
    std::vector<uint64_t> ids(o.size());
    for (size_t i = 0; i < o.size(); ++i) ids[i] = o[i].id;
    std::shuffle(ids.begin(), ids.end(), std::mt19937_64(seed));
    return ids;
}

// ------------------------------------------------- Step 1a: unsorted vector ----
// Q1 scan, Q2 nothing to look up (no levels), Q3 scan. Everything O(n), but contiguous.
struct VecBook {
    std::vector<Ord> o;
    void add(uint64_t id, char side, double px, uint32_t qty) { o.push_back({id, side, px, qty}); }
    void cancel(uint64_t id) {                        // Q1: linear scan by id
        for (size_t i = 0; i < o.size(); ++i)
            if (o[i].id == id) { o[i] = o.back(); o.pop_back(); return; }   // swap-and-pop
    }
    double best_bid() const {                         // Q3: linear scan by price
        double b = 0.0;
        for (const Ord& x : o) if (x.side == 'B' && x.px > b) b = x.px;
        return b;
    }
    double best_ask() const {
        double a = 0.0;
        for (const Ord& x : o) if (x.side == 'S' && (a == 0.0 || x.px < a)) a = x.px;
        return a;
    }
};

// --------------------------------------------------- Step 1b: sorted vector ----
// Each side sorted by price with the best at the BACK. Q2 binary search, Q3 back(),
// Q1 still a scan: a vector can be sorted by price or by id, not both.
struct SortedVecBook {
    std::vector<Ord> bids, asks;   // bids ascending px, asks descending px -> best at back()
    void add(uint64_t id, char side, double px, uint32_t qty) {
        Ord x{id, side, px, qty};
        if (side == 'B') {
            auto it = std::lower_bound(bids.begin(), bids.end(), px,
                                       [](const Ord& a, double p) { return a.px < p; });
            bids.insert(it, x);                       // O(log n) find + O(n) memmove
        } else {
            auto it = std::lower_bound(asks.begin(), asks.end(), px,
                                       [](const Ord& a, double p) { return a.px > p; });
            asks.insert(it, x);
        }
    }
    static bool erase_id(std::vector<Ord>& v, uint64_t id) {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == id) { v.erase(v.begin() + static_cast<long>(i)); return true; }
        return false;
    }
    void cancel(uint64_t id) { if (!erase_id(bids, id)) erase_id(asks, id); }   // Q1: scan
    double best_bid() const { return bids.empty() ? 0.0 : bids.back().px; }      // Q3: O(1)
    double best_ask() const { return asks.empty() ? 0.0 : asks.back().px; }
};

// ------------------------------------- Step 2: std::list + id -> iterator ----
// Q1 O(1) average (the unordered_map hands back the node), Q3 a pointer chase over
// every node. Kept in arrival order, i.e. the list IS a FIFO.
struct ListBook {
    std::list<Ord> o;
    std::unordered_map<uint64_t, std::list<Ord>::iterator> where;
    void add(uint64_t id, char side, double px, uint32_t qty) {
        o.push_back({id, side, px, qty});
        where[id] = std::prev(o.end());
    }
    void cancel(uint64_t id) {
        auto it = where.find(id);
        if (it == where.end()) return;
        o.erase(it->second);                          // O(1): the list can unlink a node
        where.erase(it);
    }
    double best_bid() const {
        double b = 0.0;
        for (const Ord& x : o) if (x.side == 'B' && x.px > b) b = x.px;   // node -> next -> ...
        return b;
    }
    double best_ask() const {
        double a = 0.0;
        for (const Ord& x : o) if (x.side == 'S' && (a == 0.0 || x.px < a)) a = x.px;
        return a;
    }
};

// ------------------- Step 3: std::map<tick, Level> + unordered_map<id, Ord> ----
// Q2 O(log L) for L levels, Q3 begin()/rbegin(), Q1 O(1) average.
template <bool Reserve>
struct MapBook {
    struct Level { uint64_t qty = 0; uint32_t n = 0; };
    std::map<int, Level> bids, asks;                  // keyed by integer tick, ascending
    std::unordered_map<uint64_t, Ord> ids;            // id -> order (node-stable)
    explicit MapBook(size_t cap = 0) { if (Reserve) ids.reserve(cap); }
    static int tick(double px) { return static_cast<int>(px / 0.01 + 0.5); }
    void add(uint64_t id, char side, double px, uint32_t qty) {
        Level& l = (side == 'B' ? bids : asks)[tick(px)];  // Q2: tree descent
        l.qty += qty; ++l.n;
        ids.emplace(id, Ord{id, side, px, qty});
    }
    void cancel(uint64_t id) {
        auto it = ids.find(id);                       // Q1: hash
        if (it == ids.end()) return;
        auto& side = it->second.side == 'B' ? bids : asks;
        auto lv = side.find(tick(it->second.px));      // Q2 again
        lv->second.qty -= it->second.qty;
        if (--lv->second.n == 0) side.erase(lv);
        ids.erase(it);
    }
    double best_bid() const { return bids.empty() ? 0.0 : bids.rbegin()->first * 0.01; }  // Q3
    double best_ask() const { return asks.empty() ? 0.0 : asks.begin()->first * 0.01; }
};

// ------------------- Step 4: the flat, price-indexed Book from labs/week07.md ----
// Exactly the lab's guided implementation (band + base_tick_, cached best slots,
// the bounded cancel scan, unordered_map id table).
namespace flat {
static constexpr double TICK = 0.01;
static constexpr int    NLEV = 1 << 16;
static inline int px_to_tick(double px) { return static_cast<int>(px / TICK + 0.5); }

struct Book {
    std::array<uint32_t, NLEV> bid_qty{};
    std::array<uint32_t, NLEV> ask_qty{};
    int  base_tick_ = 0;
    bool based_     = false;
    int  best_bid_slot = -1;
    int  best_ask_slot = -1;
    struct Rec { char side; int slot; uint32_t qty; };
    std::unordered_map<uint64_t, Rec> orders_;

    int slot_of(int tick) {
        if (!based_) { base_tick_ = std::max(0, tick - NLEV / 2); based_ = true; }
        return tick - base_tick_;
    }
    double px_of(int slot) const { return (slot + base_tick_) * TICK; }

    void add(uint64_t id, char side, double px, uint32_t qty) {
        int i = slot_of(px_to_tick(px));
        if (i < 0 || i >= NLEV) return;
        orders_[id] = {side, i, qty};
        if (side == 'B') { bid_qty[i] += qty; if (i > best_bid_slot) best_bid_slot = i; }
        else { ask_qty[i] += qty; if (best_ask_slot < 0 || i < best_ask_slot) best_ask_slot = i; }
    }
    void cancel(uint64_t id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        Rec r = it->second; orders_.erase(it);
        if (r.side == 'B') {
            bid_qty[r.slot] -= r.qty;
            if (r.slot == best_bid_slot && bid_qty[r.slot] == 0)
                while (best_bid_slot >= 0 && bid_qty[best_bid_slot] == 0) --best_bid_slot;
        } else {
            ask_qty[r.slot] -= r.qty;
            if (r.slot == best_ask_slot && ask_qty[r.slot] == 0) {
                while (best_ask_slot < NLEV && ask_qty[best_ask_slot] == 0) ++best_ask_slot;
                if (best_ask_slot >= NLEV) best_ask_slot = -1;
            }
        }
    }
    double best_bid() const { return best_bid_slot < 0 ? 0.0 : px_of(best_bid_slot); }
    double best_ask() const { return best_ask_slot < 0 ? 0.0 : px_of(best_ask_slot); }
};

// The lab's SymMap plus the load-factor guard (refuse a NEW key past 70% full,
// so the probe loops always find an empty slot and terminate).
struct SymMap {
    static constexpr int CAP = 1 << 12;
    struct Slot { char key[16]{}; uint64_t id = (uint64_t)-1; bool used = false; };
    std::array<Slot, CAP> t_{};
    int size_ = 0;
    static uint64_t hash(const char* s) {
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        return h;
    }
    static bool keq(const char* a, const char* b) {
        for (int i = 0; i < 16; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
        return true;
    }
    void put(const char* sym, uint64_t id) {
        int i = static_cast<int>(hash(sym) & (CAP - 1));
        for (;; i = (i + 1) & (CAP - 1)) {
            if (t_[i].used && keq(t_[i].key, sym)) { t_[i].id = id; return; }   // overwrite
            if (!t_[i].used) {
                if (size_ * 10 >= CAP * 7) return;                                // guard
                std::snprintf(t_[i].key, 16, "%s", sym);
                t_[i].id = id; t_[i].used = true; ++size_; return;
            }
        }
    }
    uint64_t get(const char* sym) const {
        int i = static_cast<int>(hash(sym) & (CAP - 1));
        for (;; i = (i + 1) & (CAP - 1)) {
            if (!t_[i].used)         return (uint64_t)-1;
            if (keq(t_[i].key, sym)) return t_[i].id;
        }
    }
};
}  // namespace flat

// ----------------------------------------------------------- measurement ----
struct Row { double add, cancel, bbo, add50, add99, can50, can99; };

template <class B, class Make>
static Row measure(size_t n, Make make) {
    const auto orders = make_orders(n, 42);
    const auto cancel_ids = shuffled_ids(orders, 7);
    Row r{};

    // add + cancel means: repeat whole build/teardown rounds until >= 30 ms of adds
    double add_ns = 0, can_ns = 0; size_t rounds = 0;
    while (add_ns + can_ns < 3e7 || rounds < 3) {
        std::unique_ptr<B> b = make(n);
        auto t0 = clk::now();
        for (const Ord& o : orders) b->add(o.id, o.side, o.px, o.qty);
        add_ns += ns_since(t0);
        keep(b->best_bid());
        auto t1 = clk::now();
        for (uint64_t id : cancel_ids) b->cancel(id);
        can_ns += ns_since(t1);
        keep(b->best_ask());
        ++rounds;
        if (rounds > 2000) break;
    }
    r.add = add_ns / (rounds * n);
    r.cancel = can_ns / (rounds * n);

    // percentiles: steady_clock ticks every ~42 ns on Apple Silicon, too coarse for a
    // 10 ns op, so time BATCHES of K consecutive ops and record ns/op per batch.
    // p99 of the batch means still shows the spikes (a rehash, an emptied-side scan).
    const size_t K = std::min<size_t>(BATCH, n);
    std::vector<double> sa, sc;
    do {
        std::unique_ptr<B> b = make(n);
        for (size_t i = 0; i + K <= n; i += K) {
            auto t = clk::now();
            for (size_t j = i; j < i + K; ++j) b->add(orders[j].id, orders[j].side, orders[j].px, orders[j].qty);
            sa.push_back(ns_since(t) / K);
        }
        for (size_t i = 0; i + K <= n; i += K) {
            auto t = clk::now();
            for (size_t j = i; j < i + K; ++j) b->cancel(cancel_ids[j]);
            sc.push_back(ns_since(t) / K);
        }
    } while (sa.size() < 2000);
    r.add50 = pct(sa, 0.50); r.add99 = pct(sa, 0.99);
    r.can50 = pct(sc, 0.50); r.can99 = pct(sc, 0.99);

    // bbo on a CHURNED book: add 2N, cancel a random half, so list/tree nodes are
    // scattered the way a live book's are. Then time best_bid()+best_ask().
    {
        std::unique_ptr<B> b = make(2 * n);
        auto all = make_orders(2 * n, 99, 1'000'000);
        for (const Ord& o : all) b->add(o.id, o.side, o.px, o.qty);
        auto ids = shuffled_ids(all, 11);
        for (size_t i = 0; i < n; ++i) b->cancel(ids[i]);
        long iters = 1000; double t = 0;
        for (;;) {
            auto t0 = clk::now();
            for (long i = 0; i < iters; ++i) { double x = b->best_bid() + b->best_ask(); keep(x); }
            t = ns_since(t0);
            if (t > 2e7 || iters > (1L << 30)) break;
            iters *= 2;
        }
        r.bbo = t / iters;
    }
    return r;
}

static void print_row(const char* name, const Row& r) {
    std::printf("  %-40s %8.1f %10.1f %10.2f   %7.1f /%8.1f  %7.1f /%8.1f\n",
                name, r.add, r.cancel, r.bbo, r.add50, r.add99, r.can50, r.can99);
}

template <class B>
static auto heap_make() { return [](size_t) { return std::make_unique<B>(); }; }

static void run_n(size_t n) {
    std::printf("\nN = %zu resting orders (%d price levels per side)\n", n, LEVELS);
    std::printf("  %-40s %8s %10s %10s   %16s  %16s\n", "structure", "add ns", "cancel ns",
                "bbo ns", "add p50/p99", "cancel p50/p99");
    print_row("1a vector (unsorted)",              measure<VecBook>(n, heap_make<VecBook>()));
    print_row("1b vector (sorted by price)",       measure<SortedVecBook>(n, heap_make<SortedVecBook>()));
    print_row("2  list + unordered_map<id,it>",    measure<ListBook>(n, heap_make<ListBook>()));
    print_row("3  map<tick,Level> + umap (no reserve)", measure<MapBook<false>>(n, heap_make<MapBook<false>>()));
    print_row("3  map<tick,Level> + umap (reserve)",
              measure<MapBook<true>>(n, [](size_t c) { return std::make_unique<MapBook<true>>(c); }));
    print_row("4  flat Book (lab, banded)",        measure<flat::Book>(n, heap_make<flat::Book>()));
#ifdef STUDENT_BOOK
    print_row("4  YOUR include/order_book.hpp",    measure<::Book>(n, heap_make<::Book>()));
#endif
}

// Step 2: what one node costs in memory, i.e. how many cache lines a scan touches.
static void node_sizes() {
    const size_t p = sizeof(void*);
    std::printf("\nStep 2 — node sizes (64-byte cache lines)\n");
    std::printf("  sizeof(Ord)                       = %zu bytes  -> %zu orders per line in a vector\n",
                sizeof(Ord), 64 / sizeof(Ord));
    std::printf("  std::list<Ord> node  (Ord + 2 ptr) = %zu bytes  (+ malloc header/rounding)\n", sizeof(Ord) + 2 * p);
    std::printf("  std::forward_list node (Ord + ptr) = %zu bytes\n", sizeof(Ord) + p);
    std::printf("  scanning 16,384 orders: vector touches %zu lines in order (prefetched);\n"
                "  a list touches up to 16,384 lines, each address known only after the last load\n",
                (16384 * sizeof(Ord) + 63) / 64);
}

// Step 3: the rehash spike — unordered_map insert latency with and without reserve.
static void rehash_spike() {
    const size_t n = 1 << 20;
    std::printf("\nStep 3 — rehash spike: %zu inserts into unordered_map<uint64_t, Ord>\n", n);
    std::printf("  %-12s %8s %8s %9s %12s %10s %9s\n", "", "p50 ns", "p99 ns", "p99.9 ns", "max ns", "total ms", "rehashes");
    for (int reserve = 0; reserve < 2; ++reserve) {
        std::unordered_map<uint64_t, Ord> m;
        if (reserve) m.reserve(n);
        std::vector<double> s; s.reserve(n);
        size_t buckets = m.bucket_count(), rehashes = 0;
        double total = 0;
        for (uint64_t i = 0; i < n; ++i) {
            auto t = clk::now();
            m.emplace(i * 0x9E3779B97F4A7C15ull, Ord{i, 'B', 100.0, 1});
            double d = ns_since(t);
            total += d; s.push_back(d);
            if (m.bucket_count() != buckets) { buckets = m.bucket_count(); ++rehashes; }
        }
        double mx = *std::max_element(s.begin(), s.end());
        std::printf("  %-12s %8.0f %8.0f %9.0f %12.0f %10.1f %9zu\n", reserve ? "reserve(n)" : "no reserve",
                    pct(s, 0.50), pct(s, 0.99), pct(s, 0.999), mx, total / 1e6, rehashes);
    }
}

// Step 4: SymMap::get vs std::unordered_map<std::string, uint64_t> (same 8 keys as the grader).
static void symmap_vs_umap() {
    const char* syms[] = {"AAPL", "MSFT", "NVDA", "TSLA", "AMZN", "GOOGL", "META", "SPY"};
    auto sm = std::make_unique<flat::SymMap>();
    std::unordered_map<std::string, uint64_t> um;
    for (uint64_t i = 0; i < 8; ++i) { sm->put(syms[i], i); um[syms[i]] = i; }
    auto bench = [](auto&& f) {
        long iters = 1 << 16; double t = 0;
        for (;;) {
            auto t0 = clk::now();
            for (long i = 0; i < iters; ++i) f(i);
            t = ns_since(t0);
            if (t > 2e7) break;
            iters *= 2;
        }
        return t / iters;
    };
    double a = bench([&](long i) { uint64_t v = sm->get(syms[i & 7]); keep(v); });
    double b = bench([&](long i) { auto it = um.find(syms[i & 7]); uint64_t v = it->second; keep(v); });
    std::printf("\nStep 4 — symbol lookup, 8 symbols (CAP 4096 -> load factor %.2f%%)\n", 800.0 / 4096);
    std::printf("  SymMap::get (open addressing)              %6.2f ns/op\n", a);
    std::printf("  unordered_map<std::string,uint64_t>::find  %6.2f ns/op   (%.1fx)\n", b, b / a);
}

int main(int argc, char** argv) {
    // smallest nonzero clock step: the reason the percentiles are taken over batches
    double tick = 1e9;
    for (int i = 0; i < 100000; ++i) { auto t = clk::now(); double d = ns_since(t); if (d > 0 && d < tick) tick = d; }
    std::printf("bench_containers — Session 6 / HW 7.  steady_clock resolution here: ~%.0f ns\n", tick);
    std::printf("columns: add/cancel = mean ns per op building from empty / cancelling all in random order;\n"
                "         bbo = best_bid()+best_ask() on a churned book (ns per call);\n"
                "         p50/p99 = ns/op over batches of %zu consecutive ops\n", BATCH);

    std::vector<size_t> ns = {64, 1024, 16384};
    if (argc > 1) ns = {static_cast<size_t>(std::strtoull(argv[1], nullptr, 10))};
    for (size_t n : ns) run_n(n);
    node_sizes();
    rehash_spike();
    symmap_vs_umap();
    return 0;
}
