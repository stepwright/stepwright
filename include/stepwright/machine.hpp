#pragma once

#include <stepwright/detail/live_state.hpp>
#include <stepwright/detail/type_name.hpp>
#include <stepwright/executor.hpp>
#include <stepwright/task.hpp>
#if STEPWRIGHT_TRACK_TASKS
#include <stepwright/detail/live_registry.hpp>
#endif

#include <atomic>
#include <cstdint>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Engine-produced transition, returned by a machine step. Use the protected
/// transition helpers rather than editing its fields; it is local to the driver.
struct step {
    /// Transition opcode, interpreted only by the owning driver.
    enum class kind : std::uint8_t {
        finish, ///< Deliver the terminal result.
        next,   ///< Invoke the next local step.
        child,  ///< Await the child task.
        park,   ///< Await an external wake.
        defer   ///< Resume through the home executor.
    };
    using call_type = step (*)(void*, const error&) noexcept; ///< Static step thunk.
    kind action = kind::finish;                               ///< Driver opcode.
    call_type call = nullptr;                                 ///< Next local step.
    void (*resume)(work_item*) noexcept = nullptr;            ///< Asynchronous resume thunk.
    task* child = nullptr;                                    ///< Borrowed child, for child opcode.
    error result;                                             ///< Terminal error (empty = success).
#if STEPWRIGHT_TRACK_TASKS
    const char* name = nullptr; ///< Static resume-step name (tracking builds only).
#endif
};

template <class D>
/// Restartable CRTP task whose begin()/step members execute exclusively, not
/// necessarily on the starting thread. Steps return transitions and must not
/// throw (escaping exceptions terminate). on_complete runs just before the
/// receiver; annotate adds failure context. on_cancel runs on the requesting
/// thread and may race with steps: prefer wake_if_parked() to cancel a current
/// park, or cancel external work which will eventually wake the machine. Ordinary
/// wake() intentionally buffers a result for the next park, including across work.
/// With an already-requested token the hook runs before begin(); custom hooks/
/// steps must handle that case, since no park exists yet for conditional delivery.
/// D and its children/home executor must remain alive until completion. The
/// receiver may destroy D; no engine access follows the completion callback.
class machine : public task, private work_item {

public:
    using step = stepwright::step; ///< Convenient step return type for derived classes.

    /// Destroy only while idle; Debug asserts rather than cancelling active work.
    ~machine() override {
        STEPWRIGHT_ASSERT(state_.load(std::memory_order_acquire) == 0,
                          "destroying an in-flight machine");
    }
    /// Thread-safe external result for the current/next park, at any time while
    /// active: during steps, child waits or queued deferrals. Only a suspended
    /// park can resume from this call; otherwise the result stays buffered.
    /// First pending wake wins; duplicates are dropped until consumption. Idle
    /// wakes assert in Debug. A wake after consumption may belong to the next park;
    /// an unused wake is discarded on completion. This call may complete/destroy
    /// the machine inline: producers must stop before destroying it.
    void wake(error result = {}) noexcept { deliver<false>(std::move(result)); }
    /// Thread-safe conditional delivery: atomically requires a current park and
    /// no pending result. Returns acceptance, or false on idle/child/claimed waits.
    /// Safe for cancellation races, but still requires a live object for the call;
    /// an accepted wake may invoke a receiver that destroys it before this returns.
    bool wake_if_parked(error result = {}) noexcept { return deliver<true>(std::move(result)); }
    /// Set a non-owning home executor while idle, serialized with start/bind.
    /// It must outlive active work. Synchronous continuations stay in the driver;
    /// foreign continuations post here unless already running on this executor.
    void bind(executor& ex) noexcept {
        STEPWRIGHT_ASSERT(state_.load(std::memory_order_relaxed) == 0, "bind while active");
        home_ = &ex;
    }
    /// Query the home (or nullptr); concurrent reads are safe while binding is stable.
    executor* home() const noexcept { return home_; }
    /// Borrow the last start token; stable during an active run. Retain a copy if
    /// needed across restart/destruction; do not race this reference with start.
    const cancel_token& token() const noexcept { return cancellation_.token(); }
    /// Thread-safe cancellation query while the start token is stable.
    bool cancel_requested() const noexcept { return token().cancel_requested(); }

protected:
    /// Complete successfully after the current step returns; no allocation.
    static step done() noexcept { return {}; }
    /// Complete with a failure (empty means success), adding D's context and
    /// optional annotate hook. Construct in a step; completion may destroy D.
    static step fail(error result) noexcept {
        step transition;
        transition.result = std::move(result);
        return transition;
    }
    /// Create a code/message failure with error::make's ownership and context.
    /// Deduction keeps existing fail({}) calls unambiguous (empty error).
    template <class Code>
        requires(std::is_same_v<Code, errc> || std::is_same_v<Code, std::errc> ||
                 std::is_same_v<Code, std::error_code>)
    static step fail(Code code, std::string_view message = {}) {
        return fail(error::make(code, message));
    }
    template <auto Fn>
    /// Run Fn in the same driver loop; its optional error argument is empty.
    static step then() noexcept {
        return transition<Fn>(step::kind::next);
    }
    template <auto Fn>
    /// After this step returns, start an idle child with this token, then run Fn
    /// with its result. Child must outlive its completion. No-arg Fn skips failures.
    static step after(task& child) noexcept {
        auto value = transition<Fn>(step::kind::child);
        value.child = &child;
        value.resume = &resume_child<Fn>;
        return value;
    }
    /// Start an idle child and complete with its result; same lifetime as after().
    static step forward(task& child) noexcept {
        step value;
        value.action = step::kind::child;
        value.child = &child;
        value.call = [](void*, const error& result) noexcept { return fail(result); };
        value.resume = &resume_forward;
        return value;
    }
    template <auto Fn>
    /// Await a current/upcoming wake; run Fn with its result (no-arg Fn skips
    /// failures). The external producer must respect the machine's lifetime.
    static step park() noexcept {
        return transition<Fn>(step::kind::park);
    }
    template <auto Fn>
    /// Post the next step to the previously bound home executor (required); Fn
    /// runs on a thread of that executor, even when a multi-threaded executor runs
    /// the item before this step has returned. Fn's optional error argument is
    /// empty; pending wakes stay for a later park. An immediate executor still uses
    /// the loop, never recursive continuation.
    static step defer() noexcept {
        auto value = transition<Fn>(step::kind::defer);
        value.resume = &deferred<Fn>;
        return value;
    }

private:
    friend D;
    machine() = default;
    static constexpr std::uint32_t active = 1;
    static constexpr std::uint32_t running = 2;
    static constexpr std::uint32_t claimed = 4;
    static constexpr std::uint32_t wake_ready = 8;
    static constexpr std::uint32_t parked = 16;
    static constexpr std::uint32_t deferring = 32;
    static constexpr std::uint32_t deferred_ready = 64;
    static constexpr std::uint32_t child_ready = 128;
    static constexpr std::uint32_t child_failed = 256;

    // A function, so that it is evaluated once D is complete.
    static constexpr bool has_cancel_hook() noexcept {
        return requires(D& derived) { derived.on_cancel(); };
    }

    template <auto Fn>
    static constexpr bool valid_no_arg = std::is_same_v<decltype(Fn), step (D::*)()> ||
                                         std::is_same_v<decltype(Fn), step (D::*)() noexcept>;
    template <auto Fn>
    static constexpr bool valid_arg =
        std::is_same_v<decltype(Fn), step (D::*)(const error&)> ||
        std::is_same_v<decltype(Fn), step (D::*)(const error&) noexcept>;

    template <auto Fn>
    static step invoke(void* ctx, const error& result) noexcept {
        static_assert(valid_no_arg<Fn> || valid_arg<Fn>,
                      "stepwright step must be step (D::*)() or step (D::*)(const error&), "
                      "optionally noexcept");
        auto& self = *static_cast<machine*>(ctx);
        if constexpr (valid_no_arg<Fn>) {
            if (result) {
                return fail(result);
            }
        }
        STEPWRIGHT_ASSERT(!self.step_guard_.exchange(true, std::memory_order_acquire),
                          "concurrent machine steps");
        step value;
        if constexpr (valid_arg<Fn>) {
            value = (static_cast<D&>(self).*Fn)(result);
        } else if constexpr (valid_no_arg<Fn>) {
            value = (static_cast<D&>(self).*Fn)();
        }
#if STEPWRIGHT_DEBUG
        self.step_guard_.store(false, std::memory_order_release);
#endif
        return value;
    }
    template <auto Fn>
    static step transition(typename step::kind action) noexcept {
#if STEPWRIGHT_TRACK_TASKS
        step value{.action = action,
                   .call = &invoke<Fn>,
                   .resume = &resume<Fn>,
                   .child = nullptr,
                   .result = {}};
        value.name = detail::value_name<Fn>();
        return value;
#else
        return {.action = action,
                .call = &invoke<Fn>,
                .resume = &resume<Fn>,
                .child = nullptr,
                .result = {}};
#endif
    }
    static machine& owner(work_item* item) noexcept {
        // Downcasting a nullable secondary-base pointer preserves null. Reject it
        // explicitly, also making the non-null invariant visible to GCC's analyser.
        if (!item) {
            std::terminate();
        }
        return *static_cast<machine*>(item);
    }
    template <auto Fn>
    static void resume(work_item* item) noexcept {
        auto& self = owner(item);
        error result = self.consume();
        self.drive(invoke<Fn>(&self, result));
    }
    static void resume_forward(work_item* item) noexcept {
        auto& self = owner(item);
        self.finish(self.consume_child());
    }
    template <auto Fn>
    static void resume_child(work_item* item) noexcept {
        auto& self = owner(item);
        error result = self.consume_child();
        self.drive(invoke<Fn>(&self, result));
    }
    template <auto Fn>
    static void deferred(work_item* item) noexcept {
        auto& self = owner(item);
        self.work_item::run = &resume_deferred<Fn>;
        const auto old = self.state_.fetch_or(deferred_ready | running, std::memory_order_acq_rel);
        if ((old & running) == 0) {
            resume_deferred<Fn>(item);
        }
    }
    template <auto Fn>
    static void resume_deferred(work_item* item) noexcept {
        auto& self = owner(item);
        self.state_.fetch_and(~(deferred_ready | deferring), std::memory_order_acq_rel);
        self.drive(invoke<Fn>(&self, {}));
    }
    void on_start(completion receiver, cancel_token token) final {
        STEPWRIGHT_ASSERT(state_.load(std::memory_order_relaxed) == 0, "start while active");
        receiver_ = receiver;
        state_.store(active | running, std::memory_order_release);
#if STEPWRIGHT_TRACK_TASKS
        detail::live_registry::instance().add(live_, detail::type_name<D>());
#endif
        // Idle here: finish() disarmed whatever the previous run registered. The
        // default hook only acts on a park, so it registers at the first park
        // (see docs/design/performance.md); a custom hook registers now.
        cancellation_.bind(std::move(token), &cancel, this);
        registered_ = has_cancel_hook() && cancellation_.token().can_be_cancelled();
        if (registered_) {
            cancellation_.activate();
        }
        drive(invoke<&D::begin>(this, {}));
    }
    static void cancel(void* ctx) noexcept {
        auto& self = *static_cast<machine*>(ctx);
        if constexpr (has_cancel_hook()) {
            static_cast<D&>(self).on_cancel();
        } else {
            // This callback is armed only for cancellable tokens. The requested
            // exchange, this load, park publication and its token query are all
            // seq_cst: the two sides cannot both miss the other's publication.
            if ((self.state_.load(std::memory_order_seq_cst) & parked) != 0) {
                self.wake_if_parked(error::make(errc::cancelled));
            }
        }
    }
    template <bool Conditional>
    bool deliver(error result) noexcept {
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            if constexpr (Conditional) {
                if ((old & parked) == 0) {
                    return false;
                }
            } else {
                STEPWRIGHT_ASSERT((old & active) != 0, "wake on idle machine");
            }
            if ((old & active) == 0 || (old & claimed) != 0) {
                return false;
            }
            if (state_.compare_exchange_weak(old, old | claimed, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        result_ = std::move(result);
        old |= claimed;
        for (;;) {
            // Only a suspended park belongs to a wake. Child/defer readiness
            // owns its own continuation even if a reply arrived before it.
            const bool take_ownership = (old & (running | parked)) == parked;
            const auto published = old | wake_ready | (take_ownership ? running : 0U);
            if (state_.compare_exchange_weak(old, published, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (take_ownership) {
                    dispatch();
                }
                return true;
            }
        }
    }
    void deliver_child(const error& result) noexcept {
        // Exactly once by task's contract; no external-wake duplicate claim.
        const bool failed = static_cast<bool>(result);
        if (failed) {
            child_result_ = result;
        }
        const auto old = state_.fetch_or(child_ready | running | (failed ? child_failed : 0U),
                                         std::memory_order_acq_rel);
        if ((old & running) == 0) {
            dispatch();
        }
    }
    error child_result(std::uint32_t state) noexcept {
        return (state & child_failed) != 0 ? std::move(child_result_) : error{};
    }
    error consume_child() noexcept {
        const auto old = state_.fetch_and(~(child_ready | child_failed), std::memory_order_acq_rel);
        return child_result(old);
    }
    bool await_child(error& result) noexcept {
        auto old = state_.fetch_and(~(child_ready | child_failed), std::memory_order_acq_rel);
        if ((old & child_ready) != 0) {
            result = child_result(old);
            return true;
        }
        for (;;) {
            const bool ready = (old & child_ready) != 0;
            const auto next_state = ready ? old & ~(child_ready | child_failed) : old & ~running;
            // One atomic decision: consume ready state, or release ownership.
            if (state_.compare_exchange_weak(old, next_state, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (ready) {
                    result = child_result(old);
                }
                return ready;
            }
        }
    }
    void dispatch() noexcept {
        if (home_ && !home_->running_in_this_thread()) {
            home_->post(static_cast<work_item&>(*this));
        } else {
            work_item::run(this);
        }
    }
    error consume() noexcept {
        error result = std::move(result_);
        state_.fetch_and(~(claimed | wake_ready | parked), std::memory_order_acq_rel);
        return result;
    }
    bool suspend(std::uint32_t readiness) noexcept {
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((old & readiness) != 0) {
                return false;
            }
            if (state_.compare_exchange_weak(old, old & ~running, std::memory_order_release,
                                             std::memory_order_acquire)) {
                return true;
            }
        }
    }
    // Live registry state (no code without STEPWRIGHT_TRACK_TASKS). Always written
    // by the driver that owns `running`, before ownership can pass to another thread.
    void track([[maybe_unused]] detail::live_state s,
               [[maybe_unused]] const char* resume = nullptr) noexcept {
#if STEPWRIGHT_TRACK_TASKS
        live_.set(s, resume);
#endif
    }
    static const char* resume_name([[maybe_unused]] const step& value) noexcept {
#if STEPWRIGHT_TRACK_TASKS
        return value.name;
#else
        return nullptr;
#endif
    }
    void drive(step value) noexcept {
        // This noexcept boundary deliberately terminates if a user step throws.
        track(detail::live_state::running);
        for (;;) {
            error result;
            switch (value.action) {
                case step::kind::finish:
                    finish(std::move(value.result));
                    return;
                case step::kind::next:
                    break;
                case step::kind::child:
                    work_item::run = value.resume;
                    track(detail::live_state::waiting_child, resume_name(value));
                    value.child->start({.fn =
                                            [](void* ctx, const error& delivered) noexcept {
                                                static_cast<machine*>(ctx)->deliver_child(
                                                    delivered);
                                            },
                                        .ctx = this},
                                       token());
                    if (!await_child(result)) {
                        return;
                    }
                    track(detail::live_state::running);
                    break;
                case step::kind::park:
                    work_item::run = value.resume;
                    track(detail::live_state::parked, resume_name(value));
                    if (token().can_be_cancelled()) {
                        // Register before publishing the park: a request that
                        // finds the registration runs the default hook, which
                        // either sees `parked` or is followed by the query below.
                        if (!registered_) {
                            registered_ = true;
                            cancellation_.activate();
                        }
                        // Pair SC park/request publication and the two SC reads
                        // to exclude the cross-atomic store-buffering outcome.
                        state_.fetch_or(parked, std::memory_order_seq_cst);
                        if constexpr (!has_cancel_hook()) {
                            if (cancel_requested()) {
                                wake_if_parked(error::make(errc::cancelled));
                            }
                        }
                    } else {
                        // Empty tokens keep the original acquire/release path.
                        state_.fetch_or(parked, std::memory_order_acq_rel);
                    }
                    if (suspend(wake_ready)) {
                        return;
                    }
                    track(detail::live_state::running);
                    result = consume();
                    break;
                case step::kind::defer:
                    STEPWRIGHT_ASSERT(home_, "defer requires bind");
                    work_item::run = value.resume;
                    track(detail::live_state::deferred, resume_name(value));
                    state_.fetch_or(deferring, std::memory_order_acq_rel);
                    // Off the home executor, hand the machine over *before* posting:
                    // whichever executor thread runs the item owns the next step, so
                    // it runs there even if the item runs before this thread could
                    // suspend. Nothing touches *this after the post (the machine may
                    // already have completed). On an executor thread (an immediate
                    // executor, or a worker) keep posting first: an item run inline
                    // only signals this loop, which keeps deferral chains flat.
                    if (executor& home = *home_; !home.running_in_this_thread()) {
                        static_cast<void>(suspend(deferred_ready)); // not posted: not ready
                        home.post(static_cast<work_item&>(*this));
                        return;
                    }
                    home_->post(static_cast<work_item&>(*this));
                    if (suspend(deferred_ready)) {
                        return;
                    }
                    track(detail::live_state::running);
                    state_.fetch_and(~(deferred_ready | deferring), std::memory_order_acq_rel);
                    break;
            }
            value = value.call(this, result);
        }
    }
    void finish(error result) noexcept {
        if (registered_) {
            cancellation_.disarm();
        }
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            // A wake may already have claimed the slot but not published it.
            if ((old & claimed) != 0 && (old & wake_ready) == 0) {
                std::this_thread::yield();
                old = state_.load(std::memory_order_acquire);
                continue;
            }
            if (state_.compare_exchange_weak(old, 0, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        result_ = {};
#if STEPWRIGHT_TRACK_TASKS
        detail::live_registry::instance().remove(live_);
#endif
        if (result) {
            result.add_context(detail::type_name<D>());
            if constexpr (requires(D& derived, error& e) { derived.annotate(e); }) {
                static_cast<D&>(*this).annotate(result);
            }
        }
        const completion receiver = receiver_;
        if constexpr (requires(D& derived, const error& e) { derived.on_complete(e); }) {
            static_cast<D&>(*this).on_complete(result);
        }
        receiver(result);
    }

    completion receiver_;
    executor* home_ = nullptr;
    cancel_registration cancellation_;
    error result_;
    // Independent readiness and slots: a reply can arrive before a child's
    // completion, and child-result consumption must never touch that reply.
    error child_result_;
    std::atomic<std::uint32_t> state_{0};
    std::atomic<bool> step_guard_{false};
    // Whether this run activated cancellation_; only the driver owning `running`
    // reads or writes it.
    bool registered_ = false;
#if STEPWRIGHT_TRACK_TASKS
    detail::live_node live_;
#endif
};
} // namespace stepwright
