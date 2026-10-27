// 06: one publisher, several subscribers: overflow::wait vs overflow::drop_oldest.
//
// A broadcast<T> keeps a fixed ring of the latest items; each subscription is a
// source<T> with its own cursor that sees every item published after it
// subscribed. What happens when the ring is full is a policy:
//
//   overflow::wait         the publisher waits for the slowest subscriber
//                          (lossless, but one slow reader slows everyone);
//   overflow::drop_oldest  publishing never waits; a subscriber that falls more
//                          than `capacity` items behind skips the overwritten
//                          ones, and dropped() says how many.
//
// Everything runs on this thread, so the interleaving (and the output) is exact.
#include <stepwright/stepwright.hpp>

#include <cstdio>
#include <vector>

namespace example {
// Publishes 0, 1, ..., count - 1, waiting whenever the policy says so.
struct publisher : stepwright::machine<publisher> {
    publisher(stepwright::broadcast<int>& out, int count)
        : out_(&out)
        , count_(count)
        , publish_(out) {}
    step begin() {
        if (next_ == count_) {
            out_->close(); // subscribers drain what they have not read, then end
            return done();
        }
        return after<&publisher::begin>(publish_.with(next_++));
    }

private:
    stepwright::broadcast<int>* out_;
    int count_;
    int next_ = 0;
    stepwright::broadcast<int>::publish_op publish_;
};

// Read whatever a subscription has right now (it does not wait).
std::vector<int> drain(stepwright::broadcast<int>::subscription& sub) {
    std::vector<int> items;
    for (auto p = sub.try_pull(); p.has_item(); p = sub.try_pull()) {
        items.push_back(p.value());
    }
    return items;
}
void print(const char* who, const std::vector<int>& items) {
    std::printf("  %s got %zu items:", who, items.size());
    for (const int item : items) {
        std::printf(" %d", item);
    }
    std::printf("\n");
}
} // namespace example

int main() {
    constexpr int items = 10;

    // overflow::wait: a live consumer (for_each) and a slow reader that only reads
    // when the publisher is stuck. The publisher waits; nobody misses anything.
    std::printf("overflow::wait, 4 slots, %d items\n", items);
    stepwright::broadcast<int> lossless(4, stepwright::overflow::wait);
    auto fast = lossless.subscribe();
    auto slow = lossless.subscribe();
    std::vector<int> fast_items;
    auto reader = stepwright::for_each(fast, [&](int v) { fast_items.push_back(v); });
    bool read_ok = false;
    auto on_read = [&](const stepwright::error& e) { read_ok = !e; };
    reader.start(stepwright::completion::to(on_read));
    example::publisher send_all(lossless, items);
    bool published = false;
    bool publish_ok = false;
    auto on_published = [&](const stepwright::error& e) {
        publish_ok = !e;
        published = true;
    };
    send_all.start(stepwright::completion::to(on_published));
    // The ring is full of items `slow` has not read: the publisher is waiting.
    std::printf("  publisher waiting for the slow reader: %s\n", published ? "no" : "yes");
    std::vector<int> slow_items;
    while (slow_items.size() < static_cast<std::size_t>(items)) {
        for (const int v : example::drain(slow)) { // each read frees a slot
            slow_items.push_back(v);
        }
    }
    const bool lossless_ok = published && publish_ok && read_ok;
    example::print("fast", fast_items);
    example::print("slow", slow_items);

    // overflow::drop_oldest: the slow reader reads only at the end. Publishing
    // never waits, and the slow reader sees just the last 4 items.
    std::printf("overflow::drop_oldest, 4 slots, %d items\n", items);
    stepwright::broadcast<int> lossy(4, stepwright::overflow::drop_oldest);
    auto fresh = lossy.subscribe();
    auto lagging = lossy.subscribe();
    std::vector<int> fresh_items;
    auto live = stepwright::for_each(fresh, [&](int v) { fresh_items.push_back(v); });
    example::publisher send_lossy(lossy, items);
    stepwright::when_all both(live, send_lossy);
    const bool lossy_ok = !stepwright::sync_wait(both);
    const std::vector<int> late = example::drain(lagging);
    example::print("fresh", fresh_items);
    example::print("lagging", late);
    std::printf("  lagging skipped %llu items\n",
                static_cast<unsigned long long>(lagging.dropped()));

    const bool ok = lossless_ok && lossy_ok && fast_items.size() == 10 && slow_items.size() == 10 &&
                    late.size() == 4 && lagging.dropped() == 6;
    return ok ? 0 : 1;
}

// expect: overflow::wait, 4 slots, 10 items
// expect:   publisher waiting for the slow reader: yes
// expect:   fast got 10 items: 0 1 2 3 4 5 6 7 8 9
// expect:   slow got 10 items: 0 1 2 3 4 5 6 7 8 9
// expect: overflow::drop_oldest, 4 slots, 10 items
// expect:   fresh got 10 items: 0 1 2 3 4 5 6 7 8 9
// expect:   lagging got 4 items: 6 7 8 9
// expect:   lagging skipped 6 items
