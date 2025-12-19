// RingMPSC - Lock-Free Multi-Producer Single-Consumer Channel (C++23, header-only)
//
// A ring-decomposed MPSC where every producer owns a dedicated SPSC ring.
// This removes producer contention and keeps the consumer in batch mode.
//
// Key points:
// - 128-byte alignment to avoid prefetcher false sharing
// - Batch consumption API (single head update for N items)
// - Adaptive backoff (spin -> yield)
// - Zero-copy reserve/commit API

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace ringmpsc {

struct Config {
    std::size_t ring_bits = 16;     // Ring size as power-of-two (default: 64K slots)
    std::size_t max_producers = 16; // Maximum number of producers
    bool enable_metrics = false;    // Collect counters

    friend constexpr bool operator==(const Config&, const Config&) = default;
};

inline constexpr Config default_config{};
inline constexpr Config low_latency_config{.ring_bits = 12};
inline constexpr Config high_throughput_config{.ring_bits = 18, .max_producers = 32};

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

namespace detail {

constexpr bool is_power_of_two(std::size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

template <typename T>
inline void prefetch(const T* ptr, bool write) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(static_cast<const void*>(ptr), write ? 1 : 0, 3);
#else
    (void)ptr;
    (void)write;
#endif
}

template <typename To, typename From>
constexpr To narrow_cast(From v) noexcept {
    return static_cast<To>(v);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Backoff (Crossbeam-style)
// ---------------------------------------------------------------------------

class Backoff {
public:
    constexpr Backoff() = default;

    void spin() {
        const auto spins = std::uint32_t{1} << std::min(step_, SPIN_LIMIT);
        for (std::uint32_t i = 0; i < spins; ++i) {
            detail::cpu_relax();
        }
        if (step_ <= SPIN_LIMIT) {
            ++step_;
        }
    }

    void snooze() {
        if (step_ <= SPIN_LIMIT) {
            spin();
        } else {
            std::this_thread::yield();
            if (step_ <= YIELD_LIMIT) {
                ++step_;
            }
        }
    }

    [[nodiscard]] bool is_completed() const noexcept {
        return step_ > YIELD_LIMIT;
    }

    void reset() noexcept { step_ = 0; }

private:
    static constexpr std::uint32_t SPIN_LIMIT = 6;  // 2^6 = 64 spins
    static constexpr std::uint32_t YIELD_LIMIT = 10;

    std::uint32_t step_ = 0;
};

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

struct Metrics {
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_received = 0;
    std::uint64_t reserve_spins = 0;
};

// ---------------------------------------------------------------------------
// Reservation handle (zero-copy)
// ---------------------------------------------------------------------------

template <typename T>
struct Reservation {
    std::span<T> slice;
    std::uint64_t pos = 0;
};

// ---------------------------------------------------------------------------
// SPSC Ring Buffer
// ---------------------------------------------------------------------------

template <typename T, Config config = default_config>
class Ring {
    static_assert(config.ring_bits < (sizeof(std::size_t) * 8), "ring_bits too large");
    static_assert(detail::is_power_of_two(std::size_t{1} << config.ring_bits), "ring size must be power of two");

public:
    static constexpr std::size_t capacity() noexcept { return CAPACITY; }
    static constexpr std::size_t mask() noexcept { return MASK; }

    Ring() = default;

    [[nodiscard]] std::size_t len() const noexcept {
        const auto t = tail_.load(std::memory_order_relaxed);
        const auto h = head_.load(std::memory_order_relaxed);
        return detail::narrow_cast<std::size_t>(t - h);
    }

    [[nodiscard]] bool is_empty() const noexcept {
        return tail_.load(std::memory_order_relaxed) == head_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_full() const noexcept { return len() >= CAPACITY; }

    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    // Reserve n slots for zero-copy writing. Returns empty optional if full/closed.
    [[nodiscard]] std::optional<Reservation<T>> reserve(std::size_t n) noexcept {
        if (n == 0 || n > CAPACITY) {
            return std::nullopt;
        }

        const auto tail = tail_.load(std::memory_order_relaxed);

        // Fast path: cached head
        auto space = CAPACITY - detail::narrow_cast<std::size_t>(tail - cached_head_);
        if (space >= n) {
            return make_reservation(tail, n);
        }

        cached_head_ = head_.load(std::memory_order_acquire);
        space = CAPACITY - detail::narrow_cast<std::size_t>(tail - cached_head_);
        if (space < n || is_closed()) {
            return std::nullopt;
        }

        return make_reservation(tail, n);
    }

    // Reserve with adaptive backoff. Spins then yields before giving up.
    [[nodiscard]] std::optional<Reservation<T>> reserve_with_backoff(std::size_t n) noexcept {
        Backoff backoff;
        while (!backoff.is_completed()) {
            if (auto r = reserve(n)) {
                return r;
            }
            if (is_closed()) {
                return std::nullopt;
            }
            backoff.snooze();
        }
        return std::nullopt;
    }

    void commit(std::size_t n) noexcept {
        tail_.fetch_add(detail::narrow_cast<std::uint64_t>(n), std::memory_order_release);

        if constexpr (config.enable_metrics) {
            metrics_.messages_sent += n;
            metrics_.batches_sent += 1;
        }
    }

    // Consumer API
    [[nodiscard]] std::optional<std::span<const T>> readable() noexcept {
        const auto head = head_.load(std::memory_order_relaxed);

        auto avail = cached_tail_ - head;
        if (avail == 0) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            avail = cached_tail_ - head;
            if (avail == 0) {
                return std::nullopt;
            }
        }

        const auto idx = head & MASK;
        const auto contiguous = std::min<std::uint64_t>(avail, CAPACITY - idx);

        const auto next_idx = (head + contiguous) & MASK;
        detail::prefetch(buffer_.data() + next_idx, false);

        return std::span<const T>{buffer_.data() + idx, buffer_.data() + idx + contiguous};
    }

    void advance(std::size_t n) noexcept {
        head_.store(head_.load(std::memory_order_relaxed) + detail::narrow_cast<std::uint64_t>(n),
                    std::memory_order_release);

        if constexpr (config.enable_metrics) {
            metrics_.messages_received += n;
            metrics_.batches_received += 1;
        }
    }

    template <typename Handler>
    std::size_t consume_batch(Handler&& handler) noexcept(noexcept(handler.process(static_cast<const T*>(nullptr)))) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);

        const auto avail = tail - head;
        if (avail == 0) {
            return 0;
        }

        auto pos = head;
        std::size_t count = 0;
        while (pos != tail) {
            const auto idx = pos & MASK;
            handler.process(buffer_.data() + idx);
            ++pos;
            ++count;
        }

        head_.store(tail, std::memory_order_release);

        if constexpr (config.enable_metrics) {
            metrics_.messages_received += count;
            metrics_.batches_received += 1;
        }

        return count;
    }

    template <void (*Callback)(const T*)>
    std::size_t consume_batch_fn() noexcept(noexcept(Callback(static_cast<const T*>(nullptr)))) {
        struct Handler {
            static void process(const T* item) { Callback(item); }
        };
        return consume_batch(Handler{});
    }

    // Convenience wrappers
    std::size_t send(std::span<const T> items) noexcept {
        const auto r = reserve(items.size());
        if (!r) {
            return 0;
        }
        std::ranges::copy(items.first(r->slice.size()), r->slice.begin());
        commit(r->slice.size());
        return r->slice.size();
    }

    std::size_t recv(std::span<T> out) noexcept {
        auto slice = readable();
        if (!slice) {
            return 0;
        }
        const auto n = std::min(slice->size(), out.size());
        std::ranges::copy(slice->first(n), out.begin());
        advance(n);
        return n;
    }

    void close() noexcept { closed_.store(true, std::memory_order_release); }

    [[nodiscard]] Metrics get_metrics() const noexcept {
        if constexpr (config.enable_metrics) {
            return metrics_;
        }
        return Metrics{};
    }

    void mark_active() noexcept { active_.store(true, std::memory_order_release); }

private:
    static constexpr std::size_t CAPACITY = std::size_t{1} << config.ring_bits;
    static constexpr std::size_t MASK = CAPACITY - 1;

    [[nodiscard]] std::optional<Reservation<T>> make_reservation(std::uint64_t tail, std::size_t n) noexcept {
        const auto idx = tail & MASK;
        const auto contiguous = std::min<std::size_t>(n, CAPACITY - idx);

        const auto next_idx = (tail + n) & MASK;
        detail::prefetch(buffer_.data() + next_idx, true);

        return Reservation<T>{
            .slice = std::span<T>{buffer_.data() + idx, contiguous},
            .pos = tail,
        };
    }

    alignas(128) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t cached_head_{0};

    alignas(128) std::atomic<std::uint64_t> head_{0};
    std::uint64_t cached_tail_{0};

    alignas(128) std::atomic<bool> active_{false};
    std::atomic<bool> closed_{false};
    [[no_unique_address]] std::conditional_t<config.enable_metrics, Metrics, std::monostate> metrics_{};

    alignas(64) std::array<T, CAPACITY> buffer_;
};

// ---------------------------------------------------------------------------
// Channel (MPSC)
// ---------------------------------------------------------------------------

template <typename T, Config config = default_config>
class Channel {
    static_assert(config.max_producers > 0, "max_producers must be positive");

    using RingType = Ring<T, config>;

public:
    struct Producer {
        RingType* ring = nullptr;
        std::size_t id = 0;

        [[nodiscard]] std::optional<Reservation<T>> reserve(std::size_t n) noexcept {
            return ring->reserve(n);
        }
        [[nodiscard]] std::optional<Reservation<T>> reserve_with_backoff(std::size_t n) noexcept {
            return ring->reserve_with_backoff(n);
        }
        void commit(std::size_t n) noexcept { ring->commit(n); }
        std::size_t send(std::span<const T> items) noexcept { return ring->send(items); }
    };

    enum class RegisterError { TooManyProducers, Closed };
    using RegisterResult = std::expected<Producer, RegisterError>;

    constexpr Channel() = default;

    [[nodiscard]] RegisterResult register_producer() noexcept {
        if (closed_.load(std::memory_order_acquire)) {
            return std::unexpected(RegisterError::Closed);
        }

        const auto id = producer_count_.fetch_add(1, std::memory_order_relaxed);
        if (id >= config.max_producers) {
            producer_count_.fetch_sub(1, std::memory_order_relaxed);
            return std::unexpected(RegisterError::TooManyProducers);
        }

        rings_[id].mark_active();
        return Producer{.ring = &rings_[id], .id = id};
    }

    std::size_t recv(std::span<T> out) noexcept {
        std::size_t total = 0;
        const auto count = producer_count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count && total < out.size(); ++i) {
            total += rings_[i].recv(out.subspan(total));
        }
        return total;
    }

    template <typename Handler>
    std::size_t consume_all(Handler&& handler) noexcept(noexcept(handler.process(static_cast<const T*>(nullptr)))) {
        std::size_t total = 0;
        const auto count = producer_count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i) {
            total += rings_[i].consume_batch(handler);
        }
        return total;
    }

    void close() noexcept {
        closed_.store(true, std::memory_order_release);
        const auto count = producer_count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i) {
            rings_[i].close();
        }
    }

    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::size_t producer_count() const noexcept { return producer_count_.load(std::memory_order_acquire); }

    [[nodiscard]] Metrics get_metrics() const noexcept {
        Metrics m{};
        const auto count = producer_count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i) {
            const auto rm = rings_[i].get_metrics();
            m.messages_sent += rm.messages_sent;
            m.messages_received += rm.messages_received;
            m.batches_sent += rm.batches_sent;
            m.batches_received += rm.batches_received;
        }
        return m;
    }

private:
    alignas(128) std::array<RingType, config.max_producers> rings_{};
    std::atomic<std::size_t> producer_count_{0};
    std::atomic<bool> closed_{false};
};

// Convenience aliases
using DefaultRing = Ring<std::uint64_t, default_config>;
using DefaultChannel = Channel<std::uint64_t, default_config>;

} // namespace ringmpsc
