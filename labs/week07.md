# Week 7 Lab — Data Structures for HFT (the order book)

**Format:** in-class, guided (~45–60 min), the rest is take-home.  **Repo:** the HFT starter.
**Session 6 covers decks weeks 7 and 8** in one data-structures session. **This lab IS HW 7**
("A fast order book + a fast symbol map", due **Sat Oct 24, 11:59 PM ET**, autograded by
`make book`). The week 8 half of tonight (ring buffer + online stats) is HW 8, take-home in
`labs/week08.md`, due Sat Oct 31. Midterm: **Wed Oct 28**.

## Goal
An order book has to answer three questions, and the whole session is about which data
structure answers which one:

| | Question | Where the book asks it |
|---|---|---|
| **Q1** | look up an **order by id** | every cancel / modify / fill report names an id |
| **Q2** | look up a **price level by price** | every add has to find "its" level |
| **Q3** | know the **best prices** | `best_bid()` / `best_ask()`, read thousands of times per tick |

You will build the book four ways, in the lecture's order — `std::vector`, `std::list`,
`std::map` + `std::unordered_map`, and the flat price-indexed book — measure every one on
your laptop with the same harness, and finish with the flat `Book` + open-addressing
`SymMap` that `make book` grades. Keep the session's anchor in front of you: **a cache miss
is ~100 ns, an add is ~1 ns.** Big-O counts the adds; your latency is the misses.

> Context: the live arena exchange (`shared/orderbook.py`) is a full CLOB with
> **price-time priority** and a `queue_position` per resting order. Tonight we
> build the *fast local mirror* a C++ bot keeps of that book so it can read the
> BBO on the hot path without asking the exchange. Price-time priority is why
> your FIFO queue position (surfaced by `on_ack(sym,id,queue_ahead,level_qty)`
> in the client) matters — we'll respect that structure here.

### Plan (minutes are in-class targets)
| Step | What | In class |
|---|---|---|
| 0 | Setup: read the contract, run the failing test, build the bench | 5 min |
| 1 | `std::vector` book, unsorted then sorted — Q1/Q2/Q3 all O(n), then Q2/Q3 fast | 10 min |
| 2 | `std::list` + id → iterator — Q1 O(1), Q3 a pointer chase; count cache lines | 5 min |
| 3 | `std::map<tick, Level>` + `std::unordered_map<id, Order>` — Q2 O(log L), Q3 `begin()`; the rehash spike | 7 min |
| 4 | The flat price-indexed `Book` + `SymMap` — **the HW 7 contract**, `make book` green | 25 min |
| 5 | Write-up: your ns/op table + one paragraph | 3 min to start, rest take-home |

If you are short on time, do Step 4 first: it is the graded part. Steps 1–3 are the
measurements your write-up compares against, and the bench already contains reference
versions of all of them.

## Setup (Step 0, ~5 min)
```bash
cd hft-cpp-starter-columbia   # the root of YOUR clone of the starter (has Makefile, include/, tests/)
cat include/order_book.hpp     # the stub + the contract in the comments
sed -n '1,45p' tests/book_test.cpp   # read the tests you must pass (do NOT edit)
make book                      # compiles + runs; fails until we implement

# the comparison harness — standalone, C++20, no Makefile target:
g++ -std=c++20 -O2 starters/week07/bench_containers.cpp -o /tmp/bench_containers && /tmp/bench_containers
```
`make book` fails at first:
```
RESULT|book_bbo|fail|wrong best bid/ask after add/cancel
RESULT|symmap|fail|map returned wrong id
```
The contract (from the stub and `book_test.cpp`) — it does **not** change tonight:
```cpp
struct Book {
    void   add(uint64_t id, char side, double px, uint32_t qty); // 'B'=bid 'S'=ask
    void   cancel(uint64_t id);
    double best_bid() const;   // O(1)
    double best_ask() const;   // O(1)
};
struct SymMap {                // returns (uint64_t)-1 if absent
    void     put(const char* sym, uint64_t id);
    uint64_t get(const char* sym) const;
};
```
`bench_containers.cpp` gives every structure in Steps 1–4 that same `add / cancel /
best_bid / best_ask` interface and times it on the same order flow: one-cent ticks, 256
price levels a side around \$100, N = **64 / 1,024 / 16,384** resting orders, fixed seeds.
Its columns:
- `add ns`: build the book from empty with N orders, mean per add;
- `cancel ns`: cancel all N in random order, mean per cancel;
- `bbo ns`: `best_bid() + best_ask()` on a *churned* book (add 2N, cancel a random half,
  so list and tree nodes are scattered the way a live book's are);
- `p50/p99`: per-op percentiles over batches of 32 consecutive ops. (On Apple Silicon
  `steady_clock` only ticks every ~41 ns, too coarse to time one 10 ns op on its own; the
  bench prints the resolution it measured on the first line.)

Pass one N to run only that size: `/tmp/bench_containers 1024`.

## Step 1 — `std::vector`: everything is a scan (~10 min)

### 1a. Unsorted
The obvious book: one vector of `{id, side, px, qty}`.
```cpp
struct Ord { uint64_t id; char side; double px; uint32_t qty; };

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
    // best_ask: same scan, lowest 'S' price, 0.0 if none
};
```
Write it yourself first (it is ten lines), then compare with `VecBook` in the bench. Q1 is
a scan, Q3 is a scan, and Q2 does not even exist — there are no levels, so "how much rests
at \$100.02?" is a third scan. Add is the one thing it does well: `push_back` is O(1)
amortized and contiguous.

### 1b. Sorted by price
Keep each side sorted by price with the best at the **back** (bids ascending, asks
descending). Now Q2 is a `std::lower_bound` (O(log n)) and Q3 is `back()` (O(1)). But
insertion into the middle is an O(n) `memmove`, and **Q1 is still a scan**: a vector can be
sorted by price *or* by id, not both. Read `SortedVecBook` in the bench.

Run the bench and look at rows `1a` and `1b`:
- `1a` `bbo ns` grows **linearly** with N (it is a full scan every call), and so does
  `cancel ns`.
- `1b` `bbo ns` is flat and tiny, but `add ns` and `cancel ns` grow with N.

*Your turn:* `sizeof(Ord)` is 32, not 8+1+8+4 = 21. Why? Reorder the fields so it is 24.
How many more orders per 64-byte cache line does that buy a scan?

## Step 2 — `std::list` / `std::forward_list`: Q1 O(1), Q3 a pointer chase (~5 min)
A list can unlink a node in O(1) *if you already hold an iterator to it*. So keep an
`std::unordered_map<uint64_t, std::list<Ord>::iterator>`: cancel finds the node by hash
(Q1, O(1) average) and `erase`s it. The list stays in arrival order, so it is also a FIFO —
that is the queue a price level needs. But Q3 is now a walk of every node, and each
`next` pointer is only known after the previous load finishes: the CPU cannot prefetch
what it cannot predict. (`std::forward_list` saves one pointer per node but can only erase
*after* a node, so the map would have to store the predecessor — and the predecessor
changes whenever its neighbour is cancelled. That is why books use `list`, or an intrusive
doubly linked list.)

Count the cache lines — the bench's Step 2 block prints them:
```
  sizeof(Ord)                       = 32 bytes  -> 2 orders per line in a vector
  std::list<Ord> node  (Ord + 2 ptr) = 48 bytes  (+ malloc header/rounding)
  std::forward_list node (Ord + ptr) = 40 bytes
```
A scan of 16,384 orders in a vector touches 8,192 lines **in order**, which the prefetcher
streams. The same scan over a list touches up to 16,384 lines in an order only the pointers
know. Compare `bbo ns` for row `2` against row `1a`: same O(n), different constant — that
gap is memory access, not arithmetic.

## Step 3 — `std::map<tick, Level>` + `std::unordered_map<id, Order>` (~7 min)
Now one container per question: a red-black tree keyed by **integer tick** answers Q2 in
O(log L) for L levels and Q3 with `begin()` / `rbegin()`; a hash map answers Q1.
```cpp
struct Level { uint64_t qty = 0; uint32_t n = 0; };
std::map<int, Level> bids, asks;                  // tick -> level, ascending
std::unordered_map<uint64_t, Ord> ids;            // id -> order (nodes never move)

void add(uint64_t id, char side, double px, uint32_t qty) {
    Level& l = (side == 'B' ? bids : asks)[tick(px)];  // Q2: tree descent
    l.qty += qty; ++l.n;
    ids.emplace(id, Ord{id, side, px, qty});
}
double best_bid() const { return bids.empty() ? 0.0 : bids.rbegin()->first * 0.01; }  // Q3
```
(Storing the order *in* the hash node is the same as storing an `Order*` to it: an
`unordered_map` never moves its nodes, so the address is stable. That guarantee is exactly
why the standard forces it to chain — and charges you a pointer chase per lookup.)

Look at rows `3` in the bench. Q3 is O(1) here too — libc++ caches `begin()` — so the
tree's BBO is only a few times slower than the flat book's; its cost is on add and cancel,
where every new level is a node allocation and every descent is several dependent loads.

**The rehash spike.** The bench's Step 3 block inserts 1,048,576 ids into an
`unordered_map`, timing every insert, without and with `reserve(n)`. Look at the `max ns`
column: one insert that triggers a rehash re-buckets every element at once. "O(1) average"
is not a latency claim. On the hot path, `reserve` up front — or do not use it there.

## Step 4 — the flat, price-indexed `Book` + `SymMap` (HW 7, ~25 min)
This is the graded part: implement it in `include/order_book.hpp`. The flat `Book` is the
production shape — a price-indexed array of levels, an id table for cancel, and (in your
Phase 2 book) a FIFO per level for your own orders — and it is the only structure tonight
that makes all three questions cheap at once.

### 4.1 Why not just `std::map<double, Level>`?
A tree map gives you `O(log n)` add/cancel and pointer-chasing to find the best
price — every node is a separate heap allocation, so walking the book thrashes
the cache. In HFT, prices are **not arbitrary doubles**: they live on a fixed
**tick grid** (e.g. \$0.01). That lets us turn "price" into an **array index**
and get O(1) everything with contiguous memory. That is the whole trick.

### 4.2 Ticks and a price-indexed **band**
Convert price to an integer tick, then index a flat array of levels:
```cpp
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

static constexpr double TICK = 0.01;
static constexpr int    NLEV = 1 << 16;   // 65536 contiguous slots = a band

static inline int px_to_tick(double px) {
    return static_cast<int>(px / TICK + 0.5);   // round to nearest tick
}
```
`NLEV` slots of `uint32_t` qty is 256 KB a side, 512 KB for the book — and every
access is a single indexed load. *Why:* one array, no allocation per order, and
the hardware prefetcher loves the linear layout. The `+ 0.5` is not decoration:
`109.32 / 0.01` is `10931.999999999998` in binary floating point and truncates one tick
low. (The grader's prices — 100.00, 100.02, 100.04, 100.06 — happen to divide exactly, so
`make book` will *not* catch a missing `+ 0.5`. Check `px_to_tick(109.32) == 10932` yourself.)

**Read this paragraph before you write any code — it is the bug that CI will not
catch for you.** `NLEV` slots of one cent is a **$655.35-wide window**, so if you
index *absolutely* (slot = tick, slot 0 = $0.00) your book covers $0.00–$655.35
and nothing above it. The arena's securities are not all inside that window:
`plugins/securities/defaults.py` lists **NFLX at a base price of $720** and
**META at $580**, and once NFLX moves at all the second one is marginal too. On
the first NFLX snapshot `px_to_tick(720.00)` is `72000`, so you write about
6,500 slots past the end of a 65,536-entry array.

Now look at where that write actually *lands*. `bid_qty` and `ask_qty` are
adjacent members of the same `Book`, so running off the end of `bid_qty` walks
straight into `ask_qty`: a bid at $719.98 is tick 71998, and
`71998 - 65536 = 6462`, so it silently **adds its quantity to the ask side at
$64.62**. Your book now reports resting size that no venue ever sent, on the
wrong side, at a price nobody quoted — and your bot crosses a spread that does
not exist. It does not crash. Worse, **`-fsanitize=address` does not catch it
either**: ASan instruments the boundaries *between* allocations, not the
boundaries between two members inside one object, so the whole thing is a
silent wrong answer with a clean sanitizer run. And the autograder only
exercises prices near $100, so **CI stays green** too. That is the worst
failure shape there is: no crash, no sanitizer report, no failing test, just a
book that is quietly lying to you.

The fix is *not* "make the array bigger until it covers the S&P": an absolute
array wide enough for a $5,000 stock at one-cent ticks is 2 MB a side
of almost entirely empty slots, which throws away the only thing you bought with
the flat layout (a tiny working set). Instead keep `NLEV` small and make it a
**movable band**: store the absolute tick of slot 0 in a `base_tick_`, index
`slot = tick - base_tick_`, and centre the band on the first price you ever see
for that symbol. You get $327 of room on either side of where the market
actually is, a bounds check that tells you when the market has walked out of the
band, and 512 KB regardless of whether the stock trades at $22 or $720. This is
the same `base_` offset the deck's flat-book slide uses in `idx()`, and re-basing
when the market leaves the band belongs in your Phase 2 write-up.

### 4.3 The level array + aggregated quantity
Each side is an array indexed by tick; the value is the **total resting qty** at
that price. Adding is just `+= qty`, cancelling is `-= qty`.
```cpp
struct Book {
    std::array<uint32_t, NLEV> bid_qty{};   // qty resting at each bid SLOT
    std::array<uint32_t, NLEV> ask_qty{};   // qty resting at each ask SLOT
    int  base_tick_ = 0;                    // absolute tick of slot 0
    bool based_     = false;                // has the band been placed yet?
    int best_bid_slot = -1;                 // highest bid slot with qty>0
    int best_ask_slot = -1;                 // lowest  ask slot with qty>0

    // absolute tick -> slot. The first price we ever see centres the band, so
    // we get ~$327 of headroom on either side of the market. std::max keeps
    // base_tick_ >= 0 for cheap stocks, where the band just starts at $0.00.
    int slot_of(int tick) {
        if (!based_) { base_tick_ = std::max(0, tick - NLEV / 2); based_ = true; }
        return tick - base_tick_;
    }
    // slot -> price, for the BBO reads
    double px_of(int slot) const { return (slot + base_tick_) * TICK; }
};
```
We track the *best slot* explicitly so BBO is O(1) — we never scan on the read
path (the hot path). We pay a tiny cost on add/cancel to keep it fresh instead.
Note `slot_of` is non-`const` (it may set the base) while `px_of` is `const` —
only the write path can move the band. (`slot_of` is the lab's name for the deck's
`idx()`; the base offset is the fix — without it, slot = tick and NFLX scribbles.)

### 4.4 `add` — update qty, then nudge the best pointer
```cpp
void add(uint64_t id, char side, double px, uint32_t qty) {
    int i = slot_of(px_to_tick(px));
    if (i < 0 || i >= NLEV) return;         // outside the band: refuse, don't
                                            // scribble (see 4.2 / Phase 2)
    orders_[id] = {side, i, qty};           // remember for cancel (see 4.6)
    if (side == 'B') {
        bid_qty[i] += qty;
        if (i > best_bid_slot) best_bid_slot = i;   // new inside price
    } else {
        ask_qty[i] += qty;
        if (best_ask_slot < 0 || i < best_ask_slot) best_ask_slot = i;
    }
}
```
*Why O(1):* adding at or inside the top only ever *raises* the best pointer —
one comparison, no search. Q2 is now one subtract: price → slot. The two-sided
bounds check is the whole NFLX fix: one branch on the write path, and an
out-of-band price becomes a visible dropped order instead of silent memory
corruption. In production you would re-base here rather than return.

### 4.5 `best_bid` / `best_ask` — pure O(1) reads (Q3)
```cpp
double best_bid() const {
    return best_bid_slot < 0 ? 0.0 : px_of(best_bid_slot);
}
double best_ask() const {
    return best_ask_slot < 0 ? 0.0 : px_of(best_ask_slot);
}
```
This is the payoff: the read your strategy calls thousands of times per tick is
a compare + multiply. No tree walk, no branch mispredict storm.

### 4.6 `cancel` — the only place we may scan (Q1 via the id table)
Cancel removes qty; if it empties the *inside* level, we must find the new best.
Keep an `id → (side, slot, qty)` record so cancel knows what to subtract:
```cpp
struct Rec { char side; int slot; uint32_t qty; };
std::unordered_map<uint64_t, Rec> orders_;   // fine: cancel is off the read path

void cancel(uint64_t id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Rec r = it->second; orders_.erase(it);
    if (r.side == 'B') {
        bid_qty[r.slot] -= r.qty;
        if (r.slot == best_bid_slot && bid_qty[r.slot] == 0) {
            while (best_bid_slot >= 0 && bid_qty[best_bid_slot] == 0) --best_bid_slot;
        }
    } else {
        ask_qty[r.slot] -= r.qty;
        if (r.slot == best_ask_slot && ask_qty[r.slot] == 0) {
            while (best_ask_slot < NLEV && ask_qty[best_ask_slot] == 0) ++best_ask_slot;
            if (best_ask_slot >= NLEV) best_ask_slot = -1;   // side is empty
        }
    }
}
```
Both scans are bounded on *both* ends — `--best_bid_slot` stops at `-1` and
`++best_ask_slot` stops at `NLEV`, and the ask side resets to the `-1` sentinel
when it runs off. Drop either bound and "cancel everything, then read the touch"
reads a garbage slot or crashes. The grader does exactly that.
*Why this is still fast:* the scan only runs when the inside level empties, and
it walks *one tick at a time* over contiguous memory — the common case (cancel
away from the top) is a single subtraction. The bench shows you the exception: at
N = 64 the flat book's mean cancel is the *worst* in the table, because cancelling the
last order on a side walks the rest of the band (~32,000 empty slots) to discover the
side is empty. Bounded and correct — but a tail. How would you cap it? (A per-side count
of non-empty levels, or a bitmap of non-empty slots, are both Phase 2 answers.)

### 4.7 `SymMap` — open addressing, no per-key allocation
`std::unordered_map<std::string,...>` allocates a node and hashes a string every
lookup. We want a flat table probed linearly. Symbols are `<=16` chars (see the
test: `"BRK.B"`, `"AAPL"`).
```cpp
struct SymMap {
    static constexpr int CAP = 1 << 12;           // 4,096 slots, power of two -> mask, not %
    struct Slot { char key[16]{}; uint64_t id = (uint64_t)-1; bool used = false; };
    std::array<Slot, CAP> t_{};
    int size_ = 0;                                // keys stored, for the load-factor guard

    static uint64_t hash(const char* s) {         // FNV-1a
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        return h;
    }
    static bool keq(const char* a, const char* b) {
        for (int i = 0; i < 16; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
        return true;
    }
    void put(const char* sym, uint64_t id) {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {                // linear probe
            if (t_[i].used && keq(t_[i].key, sym)) { t_[i].id = id; return; }  // overwrite
            if (!t_[i].used) {
                if (size_ * 10 >= CAP * 7) return;        // load-factor guard: refuse a NEW
                                                          // key past 70% full
                std::snprintf(t_[i].key, 16, "%s", sym);  // copy (cold path only)
                t_[i].id = id; t_[i].used = true; ++size_; return;
            }
        }
    }
    uint64_t get(const char* sym) const {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {
            if (!t_[i].used)          return (uint64_t)-1; // empty slot => absent
            if (keq(t_[i].key, sym))  return t_[i].id;
        }
    }
};
```
*Why:* one contiguous table, mask instead of modulo, no allocation per lookup,
and probing stays in cache. Overwrite and "absent → -1" both fall out naturally
(the test checks all three). `CAP = 4096` for the arena's ten symbols is a load factor
of ~0.24%: you are deliberately wasting 64 KB so the first probe almost always hits.
*Why the guard:* both probe loops terminate only because an empty slot exists. Fill the
table and `get` of an absent key spins forever — a real outage shape. The grader never
fills it; write the guard anyway. (No `erase`: deleting from an open-addressed table
breaks every probe chain through that slot unless you use tombstones, which is why symbol
maps are write-once.)

### 4.8 Run it
```bash
make book
```
You want `RESULT|book_bbo|pass` and `RESULT|symmap|pass`, plus the
`METRIC|book_bbo_ns_per_op` and `METRIC|symmap_get_ns_per_op` lines — those are
your latency numbers. Expected output (Apple M4, Apple clang 21, measured Oct 14 2026;
your numbers will differ):
```
RESULT|book_bbo|pass|best bid/ask + cancel correct
RESULT|symmap|pass|put/get/overwrite/absent correct
METRIC|book_bbo_ns_per_op|1.25969
METRIC|symmap_get_ns_per_op|5.13504
```
Then time **your** book with the same harness as Steps 1–3 (it adds a row
`4  YOUR include/order_book.hpp`):
```bash
g++ -std=c++20 -O2 -DSTUDENT_BOOK -Iinclude starters/week07/bench_containers.cpp -o /tmp/bench_containers && /tmp/bench_containers
```
Your row should look like the `4  flat Book (lab, banded)` row. If it shows `0.0`
everywhere, your `include/order_book.hpp` is still the stub.

### 4.9 Your turn (HW 7)
1. Get `make book` green (both `book_bbo` and `symmap`).
2. **Handle the edge you'll hit in the arena:** what happens when a cancel makes
   a side completely empty? Confirm `best_ask()`/`best_bid()` return `0.0`
   (sentinel) rather than reading a stale slot.
3. **Prove the band holds — and see the bug it prevents.** First reproduce the
   corruption: on an absolute-from-zero book, put a real ask at $64.62, then add
   a bid at $719.98, and print the ask level again. It grew. Note that
   `-fsanitize=address,undefined` reports **nothing** — this is an intra-object
   overflow, so the sanitizer cannot see it, which is exactly why you have to
   reason about the range instead of relying on tools. Then switch to your
   banded book: on a *fresh* `Book`, `add(1,'B',719.98,10)` and
   `add(2,'S',720.02,10)` must give a BBO of `719.98 / 720.02` (that book's band
   is now $392.30–$1047.65), and `add(3,'B',100.00,10)` on the same book must be
   *refused*, not written. Finally write down in one line what your Phase 2 book
   will do instead of refusing (re-base the band and re-key the resting
   orders).
4. Stretch: store `level_qty` per inside slot so you can report your FIFO
   `queue_ahead` — this is exactly what `on_ack(...)` gives you against the real
   CLOB.

## Step 5 — the write-up (start in class, finish at home)
HW 7's write-up is one table and one paragraph, from **your** laptop:

1. **The table.** Run `/tmp/bench_containers` (built with `-DSTUDENT_BOOK` so your row is
   in it) **twice** and copy one run in full; say how much the two runs differed. Columns:
   structure × N for `add`, `cancel`, `bbo` ns/op, plus `p50/p99` for add and cancel. Add
   your two `make book` METRIC lines and the bench's Step 3 (rehash) and Step 4 (`SymMap`
   vs `unordered_map`) blocks. Name your CPU and compiler.
2. **The paragraph.** For each structure, say which of Q1/Q2/Q3 it answered cheaply and
   *why* in terms of memory accesses, not just big-O; then say why the flat book wins at
   these N, and where it does not (look at its N = 64 cancel column). If a number is
   smaller than the slides promised, say so and explain it — that is worth more than a
   big claim you cannot defend.

The take-home half of tonight — the ring buffer, the sliding-window counter and Welford's
online mean/variance — is **HW 8: `labs/week08.md`** (`make rolling`, due Sat Oct 31).

### Reference numbers (this machine — not yours)
Apple M4 (macOS 26, Apple clang 21), `g++ -std=c++20 -O2`, measured Oct 14 2026. Two runs' means
agreed within ~5% except the smallest N, where the first run's vector row was 2× slower
(cold allocator):
```
bench_containers — Session 6 / HW 7.  steady_clock resolution here: ~41 ns
columns: add/cancel = mean ns per op building from empty / cancelling all in random order;
         bbo = best_bid()+best_ask() on a churned book (ns per call);
         p50/p99 = ns/op over batches of 32 consecutive ops

N = 64 resting orders (256 price levels per side)
  structure                                  add ns  cancel ns     bbo ns        add p50/p99    cancel p50/p99
  1a vector (unsorted)                          5.2        5.4      46.35       5.2 /     7.8      6.5 /     9.1
  1b vector (sorted by price)                  16.0        9.1       0.42      15.6 /    20.8     10.4 /    14.3
  2  list + unordered_map<id,it>               19.5       19.7      72.62      19.5 /    24.8     19.5 /    22.1
  3  map<tick,Level> + umap (no reserve)       23.2       26.8       2.22      23.4 /    27.3     26.1 /    28.7
  3  map<tick,Level> + umap (reserve)          18.4       26.7       2.19      18.2 /    23.4     28.6 /    33.9
  4  flat Book (lab, banded)                   13.9      262.6       0.67      14.3 /    19.5     18.2 /   595.1

N = 1024 resting orders (256 price levels per side)
  structure                                  add ns  cancel ns     bbo ns        add p50/p99    cancel p50/p99
  1a vector (unsorted)                          3.0       72.2    1145.94       2.6 /     7.8     71.6 /   141.9
  1b vector (sorted by price)                  67.3      118.0       0.42      71.6 /   117.2    119.8 /   243.5
  2  list + unordered_map<id,it>               16.8       23.7    1549.40      13.0 /    41.7     23.4 /    26.1
  3  map<tick,Level> + umap (no reserve)       21.0       34.5       2.95      20.8 /    66.4     33.8 /    54.7
  3  map<tick,Level> + umap (reserve)          19.1       37.5       2.97      18.2 /    44.3     35.2 /    56.0
  4  flat Book (lab, banded)                   10.7       33.4       0.66       9.1 /    40.4     18.2 /   528.6

N = 16384 resting orders (256 price levels per side)
  structure                                  add ns  cancel ns     bbo ns        add p50/p99    cancel p50/p99
  1a vector (unsorted)                          4.4     1069.3   33080.04       2.6 /    15.6   1071.6 /  2283.8
  1b vector (sorted by price)                 887.7     1996.5       0.42     824.2 /  2033.8   1974.0 /  4161.5
  2  list + unordered_map<id,it>               16.9       24.5   55506.79      13.0 /    53.4     24.7 /    36.4
  3  map<tick,Level> + umap (no reserve)       37.1       40.3       2.93      35.2 /    91.2     40.3 /    59.9
  3  map<tick,Level> + umap (reserve)          33.3       41.0       2.93      35.2 /    49.5     40.4 /    62.5
  4  flat Book (lab, banded)                   10.9       19.4       0.66       9.1 /    32.6     18.2 /    24.8

Step 3 — rehash spike: 1048576 inserts into unordered_map<uint64_t, Ord>
                 p50 ns   p99 ns  p99.9 ns       max ns   total ms  rehashes
  no reserve          0      250       917     30540208       86.0        20
  reserve(n)          0       84       667        25709       21.1         0

Step 4 — symbol lookup, 8 symbols (CAP 4096 -> load factor 0.20%)
  SymMap::get (open addressing)                2.71 ns/op
  unordered_map<std::string,uint64_t>::find    9.93 ns/op   (3.7x)
```
How to read it (the shape matters, not the digits):
- **Q3 at N = 16,384:** the unsorted vector scans in 33 µs and the list in 55 µs — the
  same O(n), but the list pays a dependent load per node. Every structure that *keeps* the
  best (sorted vector, map, flat) answers Q3 in under 3 ns.
- **The sorted vector has the fastest BBO** (0.42 ns, a load of `back().px`) and the
  slowest add and cancel (0.9–2 µs at 16k): it bought Q3 with Q1 and with every insert.
- **The tree is the honest all-rounder**: 20–40 ns per add/cancel, ~3 ns BBO. It loses to
  the flat book on add/cancel by 2–3×, not 100×.
- **The flat book** is the only row that is cheap on all three at every N — *except* the
  N = 64 cancel mean (263 ns): emptying a side walks the band. A real result to explain,
  not hide.
- **`reserve` vs not:** the p50 is the same, the **max** is 30 ms vs 26 µs. One unlucky
  insert costs more than a million normal ones. That is a p99.9 event on the hot path.
- **`SymMap` vs `unordered_map<std::string>`:** ~3.7× — not 100×.

## Likeliest failures (check these first)
1. **Prices one tick off — and `make book` still green.** Missing `+ 0.5` in `px_to_tick`:
   `109.32 / 0.01 = 10931.999999999998` truncates to 10931, as do $0.29, $1.15, $4.35 and
   thousands of other cent prices. The grader's four prices divide exactly, so it cannot see
   this; assert `px_to_tick(109.32) == 10932` yourself. Fix: round, don't truncate. Long-term
   fix: carry integer ticks from the wire boundary on and never divide a price again.
2. **Crash, or a silent wrong answer, on the second cancel.** The ask-side scan runs off
   the end (`best_ask_slot` reaching `NLEV`) without the
   `if (best_ask_slot >= NLEV) best_ask_slot = -1;` bail-out, or the bid scan goes below
   zero. Fix: bound both loops and set the sentinel. Test "cancel everything, then read the
   touch" yourself — the grader does.
3. **`symmap` hangs.** The probe loop has no empty-slot exit in `get`, or `put` writes a
   new key without comparing keys and the table fills. Fix: `if (!t_[i].used) return
   (uint64_t)-1;` first in `get`; in `put`, overwrite on `used && keq`, insert on `!used`,
   and keep the load-factor guard. If it spins for two minutes, add a probe counter and
   `assert` it under 100.
4. **NFLX prices silently land on the wrong side.** You indexed `slot = tick` (absolute)
   instead of `tick - base_tick_`, or dropped the `i < 0 || i >= NLEV` check. CI and ASan
   both stay green; only Step 4.9 item 3 catches it.
5. **Stack overflow / segfault the moment you make a `Book`.** It is 512 KB of arrays; a
   few of them as locals (or one `SymMap` + `Book` per symbol) blow the 8 MB stack. Make
   them heap-allocated (`std::make_unique<Book>()`) or members of a `static` object — the
   bench does exactly that.
6. **Bench row shows 0.0 / your book is "infinitely fast".** Either the stub is still in
   `include/order_book.hpp`, or the optimizer deleted your loop. The bench passes every
   result through `keep()` (an `asm volatile` barrier); keep that in any timing loop you
   write. A suspiciously good number is a bug until proven otherwise.
7. **Bench does not compile.** It needs C++20 (`-std=c++20`) and, for `-DSTUDENT_BOOK`,
   `-Iinclude` so it finds `order_book.hpp`. Run it from the starter root.
8. **Numbers jump around between runs.** Laptop on battery, other apps busy, or the first
   run warming the allocator. Plug in, close the browser, run twice, report both.

## Checkpoint
- `make book` → `book_bbo` and `symmap` both **pass**.
- You can say which of Q1/Q2/Q3 each of vector, sorted vector, list, map + hash, and the
  flat book answers cheaply — and why in memory accesses.
- You can state, in one sentence, why the flat array is O(1) on the read path
  and where the *only* scan can happen (an emptied inside level on cancel).
- `NLEV = 1 << 16` at one-cent ticks is a **$655.35-wide band**, so the array is
  indexed against `base_tick_`, not from $0.00 — and you can say why NFLX at
  $720 would otherwise write past the end of it with CI still green.
- You have numbers from **your** laptop: "flat book BBO is ~N× faster than `std::map`",
  "`SymMap` is ~N× faster than `unordered_map<std::string>`", and the rehash `max`.
- `make test` shows HW7 credit (`book_bbo` 12 pts, `symmap` 8 pts).

## Links
- Stub / contract: `include/order_book.hpp`
- Tests (read, don't edit): `tests/book_test.cpp`, `tests/bench.hpp`
- Comparison harness: `starters/week07/bench_containers.cpp`
- Reference CLOB (price-time priority, `queue_position`): `shared/orderbook.py`
- Client hot path + `on_ack` queue position: `docs/HFT_CPP_CLIENT.md`
- Take-home part of tonight (HW 8): `labs/week08.md`
- Build: `make book`, full grade `make test`
