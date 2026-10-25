// 08: structured cancellation.
//
// A cancel_source creates tokens; request_cancel() runs the callbacks registered
// on them. Every start() carries a token, and a machine passes its own token to
// every child it starts, so one request reaches a whole tree of work.
//
// What a machine does on cancellation:
//   - by default, a machine *parked* (waiting for wake()) is woken with
//     errc::cancelled; one doing anything else is left alone, since its children
//     see the same token and finish early on their own;
//   - with an on_cancel() hook it decides itself (on the requesting thread, so
//     the hook must be thread-safe): typically cancel external work, or call
//     wake_if_parked(), which wakes it only if it is parked right now.
// A cancel_callback registers any callable on a token, for code that is not a
// machine. The callbacks of one request run in no particular order, so this
// example records what happened and prints it afterwards.
#include "example_support.hpp"
#include <cstdio>

namespace example {
struct transfer;
// A stand-in for a socket: a request that some other component answers later.
struct fake_socket {
    transfer* waiting = nullptr;
    bool aborted = false;
};

// Parks until the socket answers; its hook aborts the socket.
struct transfer : stepwright::machine<transfer> {
    fake_socket* socket = nullptr;
    stepwright::error result;

    step begin() {
        socket->waiting = this;
        return park<&transfer::on_reply>(); // until someone calls wake(...)
    }
    step on_reply(const stepwright::error& e) {
        socket->waiting = nullptr;
        result = e;
        return fail(e);
    }
    void on_cancel() noexcept {
        socket->aborted = true; // abort the external work...
        // ...and stop waiting for it, but only if parked right now.
        wake_if_parked(stepwright::error::make(stepwright::errc::cancelled, "socket aborted"));
    }
};

// A heartbeat with no hook: the default wakes its park with errc::cancelled.
struct heartbeat : stepwright::machine<heartbeat> {
    stepwright::error result;
    step begin() { return park<&heartbeat::on_tick>(); }
    step on_tick(const stepwright::error& e) {
        result = e;
        return fail(e);
    }
};

// The parent runs both children side by side. It never looks at cancellation
// itself: its token reaches both children through when_all.
struct session : stepwright::machine<session> {
    explicit session(fake_socket& socket) : both_(transfer_, heartbeat_) {
        transfer_.socket = &socket;
    }
    const transfer& transfer_child() const noexcept { return transfer_; }
    const heartbeat& heartbeat_child() const noexcept { return heartbeat_; }

    step begin() { return after<&session::on_children>(both_); }
    step on_children(const stepwright::error& e) { return fail(e); }

private:
    transfer transfer_;
    heartbeat heartbeat_;
    stepwright::when_all<2> both_;
};

const char* outcome(const stepwright::error& e) {
    if (!e) {
        return "ok";
    }
    return e.is(stepwright::errc::cancelled) ? "cancelled" : "failed";
}
} // namespace example

int main() {
    example::fake_socket socket;
    example::session session(socket);

    stepwright::cancel_source source;
    bool noted = false;
    stepwright::cancel_callback note(source.token(), [&noted] { noted = true; });

    example::waiter done;
    session.start(done.callback(), source.token());
    std::printf("session started; transfer parked: %s\n", socket.waiting ? "yes" : "no");
    source.request_cancel(); // runs every callback here, on this thread
    const stepwright::error e = done.wait();

    const auto& transfer = session.transfer_child();
    std::printf("cancel_callback ran: %s\n", noted ? "yes" : "no");
    std::printf("transfer: %s (%.*s), socket aborted: %s\n", example::outcome(transfer.result),
                static_cast<int>(transfer.result.message().size()),
                transfer.result.message().data(), socket.aborted ? "yes" : "no");
    std::printf("heartbeat: %s\n", example::outcome(session.heartbeat_child().result));
    std::printf("session: %s\n", example::outcome(e));

    // A token that is already cancelled: a machine parks and is cancelled at once.
    example::heartbeat late;
    const stepwright::error late_result = example::run_blocking(late, source.token());
    std::printf("late heartbeat: %s\n", example::outcome(late_result));

    const bool ok = noted && socket.aborted && e.is(stepwright::errc::cancelled) &&
                    late_result.is(stepwright::errc::cancelled);
    return ok ? 0 : 1;
}

// expect: session started; transfer parked: yes
// expect: cancel_callback ran: yes
// expect: transfer: cancelled (socket aborted), socket aborted: yes
// expect: heartbeat: cancelled
// expect: session: cancelled
// expect: late heartbeat: cancelled
