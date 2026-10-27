// 10: finding a hang with the live task registry.
//
// A machine that waits for something that never happens does not crash; it just
// never completes. With the CMake option STEPWRIGHT_TRACK_TASKS (this example
// enables it for its own program), every running machine is registered, and
// stepwright::debug::dump() prints one line per machine: its type, what it is
// doing (running, waiting_child, parked, deferred) and the step it will resume
// at, which usually names the missing event directly. Tracking costs a little
// per machine, so it is meant for debug builds.
//
// Here an order workflow hangs: it waits for a payment confirmation that is
// never sent, and a listener waits for a message that never arrives. dump()
// also prints each entry's age and starting thread; they differ from run to
// run, so this example cuts them off to keep its output reproducible.
#include <stepwright/debug/live_tasks.hpp>
#include <stepwright/stepwright.hpp>

#include <cstdio>
#include <sstream>
#include <string>

namespace example {
struct payment_listener : stepwright::machine<payment_listener> {
    step begin() { return park<&payment_listener::on_message>(); } // wake() never comes
    step on_message() { return done(); } // a failed wake skips this automatically
};

struct order_workflow : stepwright::machine<order_workflow> {
    explicit order_workflow(stepwright::event& paid) : payment_(paid.wait()) {}
    step begin() { return after<&order_workflow::on_paid>(payment_); }
    step on_paid() { return then<&order_workflow::ship>(); }
    step ship() {
        shipped = true;
        return done();
    }
    bool shipped = false;

private:
    stepwright::event::waiter payment_;
};

// dump(), minus the run-dependent " age=...us thread=..." suffix of each line.
std::string dump_without_timing() {
    std::ostringstream out;
    stepwright::debug::dump(out);
    std::istringstream lines(out.str());
    std::string result;
    for (std::string line; std::getline(lines, line);) {
        result += "  " + line.substr(0, line.find(" age=")) + "\n";
    }
    return result;
}
} // namespace example

int main() {
    static_assert(stepwright::debug::tracking_enabled, "built with STEPWRIGHT_TRACK_TASKS=1");
    stepwright::event payment_confirmed; // the bug: nobody calls set() on it
    example::order_workflow order(payment_confirmed);
    example::payment_listener listener;

    bool order_done = false;
    bool listener_done = false; // these callbacks run on this thread
    auto on_order = [&](const stepwright::error& e) { order_done = !e; };
    auto on_listener = [&](const stepwright::error& e) { listener_done = !e; };
    order.start(stepwright::completion::to(on_order));
    listener.start(stepwright::completion::to(on_listener));

    std::printf("order shipped: %s; live machines: %zu\n", order.shipped ? "yes" : "no",
                stepwright::debug::live_count());
    std::printf("dump():\n%s", example::dump_without_timing().c_str());

    // The dump points at both culprits. Deliver what they wait for:
    payment_confirmed.set();
    listener.wake();
    const bool completed = order_done && listener_done;
    std::printf("after the fix: order shipped: %s; live machines: %zu\n",
                order.shipped ? "yes" : "no", stepwright::debug::live_count());
    return completed && order.shipped && stepwright::debug::live_count() == 0 ? 0 : 1;
}

// expect: order shipped: no; live machines: 2
// expect: dump():
// expect: example::payment_listener state=parked step=&example::payment_listener::on_message
// expect: example::order_workflow state=waiting_child step=&example::order_workflow::on_paid
// expect: after the fix: order shipped: yes; live machines: 0
