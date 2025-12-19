# RingMPSC C++23 (Header-Only)

This is a C++23 header-only port of [ringmpsc](https://github.com/boonzy00/ringmpsc), a lock-free multi-producer single-consumer (MPSC) channel where each producer has its own SPSC ring to avoid producer contention. You should read the original README. There is nothing new here.

## Features
- 128-byte alignment to reduce prefetcher false sharing
- Zero-copy reserve/commit API
- Batch consumption with a single head update
- Adaptive backoff (spin → yield)
- Optional metrics

## Layout
- `include/ringmpsc.hpp` — library header
- `tests/` — lightweight unit tests mirroring the Zig suite

## Build & Test
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target test_ringmpsc
ctest --output-on-failure --test-dir build
```

## Benchmarks
- `tests/bench_final`: scaled benchmark. Usage: `./build/tests/bench_final <msgs_per_producer> [batch_size]`. Defaults: msgs `1_000_000`, batch `8192` (override batch via `BENCH_BATCH`).
- `tests/bench_final_parity`: parity benchmark mirroring Zig setup. Usage: `./build/tests/bench_final_parity <msgs_per_producer>`.

## Usage
```cpp
#include <ringmpsc.hpp>
using namespace ringmpsc;

Channel<std::uint64_t> ch;
auto prod = ch.register_producer().value();
if (auto r = prod.reserve(2)) {
    r->slice[0] = 1;
    r->slice[1] = 2;
    prod.commit(2);
}
std::uint64_t out[4];
auto n = ch.recv(out); // n == 2
```
