# HW7 — Flat Order Book + Open-Addressing SymMap

## Results (Apple Silicon, -O2, median of 7 runs of `make book`)

| Metric | Before (lab version) | After (mine) | Baseline |
|---|---|---|---|
| `SymMap::get` | 4.17 ns | **3.02 ns** (~1.4x faster) | `unordered_map<std::string,uint64_t>::find` ≈ 12.3 ns |
| `best_bid()+best_ask()` | 0.69 ns | 0.72 ns (Book unchanged; difference is noise) | `std::map<tick,Level>` ≈ 3.2–4.7 ns |

Correctness: CI passes `Book: best bid/ask + cancel` (12/12) and `SymMap: put/get/absent` (8/8).
Cycles/op: Apple Silicon has no `__rdtsc()`, so I timed with `steady_clock` and convert as cycles/op ≈ ns/op × CPU clock (GHz).
Baseline numbers come from `starters/week07/bench_containers.cpp` on the same machine.

## Why the flat book beats `std::map`
`std::map` is a red-black tree whose nodes are allocated separately and scattered across the heap, so finding the best level means pointer chasing, and each hop can be a cache miss. The flat book stores quantities in a contiguous array indexed directly by price tick and caches the best bid/ask slot, so `best_bid()` is a single load from memory that is almost always already in cache.

## Why `SymMap` beats `std::unordered_map`
`unordered_map<std::string>` must build a temporary `std::string` from the `const char*` on every lookup, run a general-purpose string hash, follow a pointer from the bucket array into a separately allocated chain node, and then do a full string comparison. My open-addressing table keeps all slots in one contiguous array, so a lookup normally finishes in the first slot it probes.

## The single change that helped most
I pack each key (at most 16 chars) into two 64-bit integers `k0, k1`. The hash becomes a couple of integer multiply/xor operations instead of a byte-by-byte FNV loop, and key comparison becomes two integer compares instead of a byte-by-byte loop. This cut `get` from 4.17 ns to 3.02 ns. It also fixed a correctness bug in the lab version: `snprintf(key, 16, ...)` reserves one byte for the terminating NUL, so it stored only 15 characters. A 16-character symbol was silently truncated on `put` and could never be found by `get`. The packed keys use all 16 bytes with no terminator needed.

## What didn't work, and measurement notes
I tried replacing the copy loop in `pack()` with `strnlen` + `memcpy`. It was slower (~5.7 ns): with a variable length, both become library calls, and for 4–5 character symbols the call and branch overhead costs more than the short inline loop, so I reverted it.
I report the median of 7 runs because individual runs varied a lot, from OS background noise and because the Mac schedules the process on either a performance or an efficiency core. The `get` timings clustered around ~2.5–3 ns and ~5.7 ns, so a mean would be misleading.

## Cycles/op (Apple M2, performance-core clock ≈ 3.49 GHz)
cycles/op = ns/op × 3.49. Approximate: the clock is not fixed, and runs on an efficiency core (~2.4 GHz) take longer in ns.

| Metric | Before (lab) | After (mine) | Baseline |
|---|---|---|---|
| `SymMap::get` | ~14.6 cycles | **~10.5 cycles** | `unordered_map` find ~43 cycles |
| `best_bid()+best_ask()` | ~2.4 cycles | ~2.5 cycles | `std::map` ~11–16 cycles |
