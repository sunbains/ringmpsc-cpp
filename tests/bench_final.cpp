// C++23 benchmark mirroring src/bench_final.zig (scaled for quick runs).
// Adjustable message count via argv[1] or env BENCH_MSG (default: 1_000_000).

#include <ringmpsc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace ringmpsc;

namespace {

std::uint64_t get_env_u64(const char* name, std::uint64_t def) {
    if (const char* v = std::getenv(name)) {
        return std::strtoull(v, nullptr, 10);
    }
    return def;
}

struct Result {
    double rate_billion_per_s = 0.0;
};

Result run_bench(std::size_t num_producers, std::uint64_t msgs_per_producer, std::uint64_t batch_override = 0) {
    const std::size_t BATCH = batch_override != 0 ? static_cast<std::size_t>(batch_override)
                                                  : static_cast<std::size_t>(get_env_u64("BENCH_BATCH", 8192));
    constexpr std::size_t RING_BITS = 16;
    constexpr std::size_t MAX_PRODUCERS = 8;
    constexpr Config cfg{.ring_bits = RING_BITS, .max_producers = MAX_PRODUCERS};

    using ChannelT = Channel<std::uint32_t, cfg>;
    ChannelT channel;

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(num_producers);
    consumers.reserve(num_producers);

    std::vector<std::uint64_t> consumed(num_producers, 0);

    // Register producers upfront
    std::vector<ChannelT::Producer> regs;
    regs.reserve(num_producers);
    for (std::size_t i = 0; i < num_producers; ++i) {
        auto p = channel.register_producer();
        if (!p) {
            throw std::runtime_error("producer registration failed");
        }
        regs.push_back(*p);
    }

    // Consumer threads (one per ring, consume directly from that ring to avoid contention)
    for (std::size_t i = 0; i < num_producers; ++i) {
        auto ring = regs[i].ring;
        consumers.emplace_back([ring, i, &consumed] {
            struct Handler {
                std::uint64_t* counter;
                inline void process(const std::uint32_t*) { ++(*counter); }
            } handler{.counter = &consumed[i]};

            Backoff backoff;
            while (true) {
                auto n = ring->consume_batch(handler);
                if (n == 0) {
                    if (ring->is_closed() && ring->is_empty()) break;
                    backoff.snooze();
                } else {
                    backoff.reset();
                }
            }
        });
    }

    // Producer threads
    for (std::size_t i = 0; i < num_producers; ++i) {
        producers.emplace_back([prod = regs[i], msgs_per_producer, BATCH] () mutable {
            const std::size_t BATCH_LOCAL = BATCH;
            std::uint64_t sent = 0;
            while (sent < msgs_per_producer) {
                const auto want = std::min<std::uint64_t>(BATCH_LOCAL, msgs_per_producer - sent);
                if (auto r = prod.reserve(static_cast<std::size_t>(want))) {
                    for (std::size_t j = 0; j < r->slice.size(); ++j) {
                        r->slice[j] = static_cast<std::uint32_t>(sent + j);
                    }
                    prod.commit(r->slice.size());
                    sent += r->slice.size();
                } else {
                    Backoff backoff;
                    backoff.spin();
                }
            }
        });
    }

    const auto start = std::chrono::steady_clock::now();

    for (auto& t : producers) t.join();
    // Signal closure: drop producers count and mark closed
    channel.close();

    for (auto& t : consumers) t.join();

    const auto end = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::uint64_t total = 0;
    for (auto c : consumed) total += c;

    const double rate = static_cast<double>(total) / static_cast<double>(ns); // msgs per ns
    return {.rate_billion_per_s = rate * 1e9 / 1e9}; // scale to billions/sec
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t msgs_per_producer = get_env_u64("BENCH_MSG", 1'000'000);
    std::uint64_t batch_override = get_env_u64("BENCH_BATCH", 0);
    if (argc >= 2) {
        msgs_per_producer = std::strtoull(argv[1], nullptr, 10);
        if (msgs_per_producer == 0) {
            msgs_per_producer = 1'000'000;
        }
    }
    if (argc >= 3) {
        batch_override = std::strtoull(argv[2], nullptr, 10);
    }

    const std::size_t producer_counts[] = {1, 2, 4, 6, 8};

    std::cout << "C++ bench (scaled): msgs/producer=" << msgs_per_producer << "\n";
    if (batch_override != 0) {
        std::cout << "Batch size override=" << batch_override << "\n";
    }
    std::cout << "Producers | Throughput (B msg/s)\n";
    std::cout << "-------------------------------\n";
    for (auto p : producer_counts) {
        auto r = run_bench(p, msgs_per_producer, batch_override);
        std::cout << p << "         | " << r.rate_billion_per_s << "\n";
    }
    return 0;
}
