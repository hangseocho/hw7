#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
// HW7 — flat, price-indexed order book + open-addressing symbol->id map.
// side 'B'=bid, 'S'=ask.  SymMap::get returns (uint64_t)-1 if absent.
// BEFORE version = lab guided implementation (labs/week07.md).

static constexpr double OB_TICK = 0.01;
static constexpr int    OB_NLEV = 1 << 16;
static inline int ob_px_to_tick(double px) { return static_cast<int>(px / OB_TICK + 0.5); }

struct Book {
    std::array<uint32_t, OB_NLEV> bid_qty{};
    std::array<uint32_t, OB_NLEV> ask_qty{};
    int  base_tick_ = 0;
    bool based_     = false;
    int  best_bid_slot = -1;
    int  best_ask_slot = -1;
    struct Rec { char side; int slot; uint32_t qty; };
    std::unordered_map<uint64_t, Rec> orders_;

    int slot_of(int tick) {
        if (!based_) { base_tick_ = std::max(0, tick - OB_NLEV / 2); based_ = true; }
        return tick - base_tick_;
    }
    double px_of(int slot) const { return (slot + base_tick_) * OB_TICK; }

    void add(uint64_t id, char side, double px, uint32_t qty) {
        int i = slot_of(ob_px_to_tick(px));
        if (i < 0 || i >= OB_NLEV) return;
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
                while (best_ask_slot < OB_NLEV && ask_qty[best_ask_slot] == 0) ++best_ask_slot;
                if (best_ask_slot >= OB_NLEV) best_ask_slot = -1;
            }
        }
    }
    double best_bid() const { return best_bid_slot < 0 ? 0.0 : px_of(best_bid_slot); }
    double best_ask() const { return best_ask_slot < 0 ? 0.0 : px_of(best_ask_slot); }
};

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
            if (t_[i].used && keq(t_[i].key, sym)) { t_[i].id = id; return; }
            if (!t_[i].used) {
                if (size_ * 10 >= CAP * 7) return;
                std::snprintf(t_[i].key, 16, "%s", sym);   // BUG: 16-char keys truncated (fix later)
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
