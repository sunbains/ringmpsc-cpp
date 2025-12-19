// Basic unit tests for ringmpsc.hpp mirroring the Zig suite

#include <ringmpsc.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace ringmpsc;

namespace {

struct TestRunner {
    int failures = 0;

    template <typename Fn>
    void run(std::string_view name, Fn&& fn) {
        try {
            fn();
            std::cout << "[PASS] " << name << "\n";
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << name << " - " << e.what() << "\n";
            ++failures;
        } catch (...) {
            std::cout << "[FAIL] " << name << " - unknown exception\n";
            ++failures;
        }
    }
};

inline void expect(bool cond, std::string_view msg) {
    if (!cond) {
        throw std::runtime_error(std::string(msg));
    }
}

template <typename T>
bool all_equal(std::span<const T> lhs, std::span<const T> rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) return false;
    }
    return true;
}

} // namespace

int main() {
    TestRunner tr;

    tr.run("ring: basic reserve/commit/readable/advance", [] {
        Ring<std::uint64_t> ring;

        auto w = ring.reserve(4);
        expect(w.has_value(), "reservation should succeed");
        auto& slice = w->slice;
        slice[0] = 100;
        slice[1] = 200;
        slice[2] = 300;
        slice[3] = 400;
        ring.commit(4);

        expect(ring.len() == 4, "len should be 4 after commit");

        auto r = ring.readable();
        expect(r.has_value(), "readable should succeed");
        expect((*r)[0] == 100 && (*r)[3] == 400, "values should match");
        ring.advance(4);

        expect(ring.is_empty(), "ring should be empty");
    });

    tr.run("ring: batch consumption", [] {
        Ring<std::uint64_t> ring;

        for (std::uint64_t i = 0; i < 10; ++i) {
            auto w = ring.reserve(1);
            expect(w.has_value(), "reserve 1 should succeed");
            w->slice[0] = i * 10;
            ring.commit(1);
        }

        std::uint64_t sum = 0;
        struct Handler {
            std::uint64_t* sum;
            void process(const std::uint64_t* item) { *sum += *item; }
        };

        auto consumed = ring.consume_batch(Handler{.sum = &sum});
        expect(consumed == 10, "should consume 10 items");
        expect(sum == 0 + 10 + 20 + 30 + 40 + 50 + 60 + 70 + 80 + 90, "sum should match");
        expect(ring.is_empty(), "ring should be empty after batch");
    });

    tr.run("ring: backoff on full", [] {
        constexpr Config small_cfg{.ring_bits = 4};
        Ring<std::uint64_t, small_cfg> ring;

        for (std::uint64_t i = 0; i < Ring<std::uint64_t, small_cfg>::capacity(); ++i) {
            auto w = ring.reserve(1);
            expect(w.has_value(), "fill reserve should succeed");
            w->slice[0] = i;
            ring.commit(1);
        }

        expect(!ring.reserve(1).has_value(), "reserve should fail when full");
        expect(!ring.reserve_with_backoff(1).has_value(), "backoff reserve should fail when full");
    });

    tr.run("channel: multi-producer", [] {
        auto ch = std::make_unique<Channel<std::uint64_t>>();

        auto p1 = ch->register_producer();
        auto p2 = ch->register_producer();
        expect(p1.has_value(), "p1 should register");
        expect(p2.has_value(), "p2 should register");

        expect(p1->send(std::span<const std::uint64_t>{std::array<std::uint64_t, 2>{10, 11}}) == 2,
               "p1 send");
        expect(p2->send(std::span<const std::uint64_t>{std::array<std::uint64_t, 2>{20, 21}}) == 2,
               "p2 send");

        std::array<std::uint64_t, 10> out{};
        auto n = ch->recv(out);
        expect(n == 4, "should receive 4 items");
        std::array<std::uint64_t, 4> expected{10, 11, 20, 21};
        expect(all_equal<std::uint64_t>(std::span<const std::uint64_t>{out}.first(4),
                                        std::span<const std::uint64_t>{expected}),
               "values should match round-robin order");
    });

    tr.run("channel: consumeAll batch", [] {
        auto ch = std::make_unique<Channel<std::uint64_t>>();

        auto p1 = ch->register_producer();
        auto p2 = ch->register_producer();
        expect(p1 && p2, "producers should register");

        expect(p1->send(std::span<const std::uint64_t>{std::array<std::uint64_t, 3>{1, 2, 3}}) == 3, "p1 send");
        expect(p2->send(std::span<const std::uint64_t>{std::array<std::uint64_t, 3>{4, 5, 6}}) == 3, "p2 send");

        std::uint64_t sum = 0;
        struct Handler {
            std::uint64_t* sum;
            void process(const std::uint64_t* item) { *sum += *item; }
        };

        auto consumed = ch->consume_all(Handler{.sum = &sum});
        expect(consumed == 6, "should consume 6 items");
        expect(sum == 21, "sum should be 21");
    });

    tr.run("backoff: spin progression", [] {
        Backoff b;
        expect(!b.is_completed(), "initially not completed");
        b.spin();
        expect(!b.is_completed(), "should not complete after initial spin");
        while (!b.is_completed()) {
            b.snooze();
        }
        expect(b.is_completed(), "should eventually complete");
        b.reset();
        expect(!b.is_completed(), "should reset completion state");
    });

    if (tr.failures != 0) {
        std::cout << tr.failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All C++ tests passed\n";
    return 0;
}
