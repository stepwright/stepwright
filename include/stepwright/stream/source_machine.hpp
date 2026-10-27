#pragma once

#include <stepwright/detail/live_state.hpp>
#include <stepwright/detail/type_name.hpp>
#include <stepwright/detail/wait_core.hpp>
#include <stepwright/executor.hpp>
#include <stepwright/stream/source.hpp>
#if STEPWRIGHT_TRACK_TASKS
#include <stepwright/detail/live_registry.hpp>
#endif

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Transition returned by a source_machine step. Use the protected transition
/// helpers of source_machine; the fields are interpreted only by its driver.
struct source_step {
    /// Transition opcode.
    enum class kind : std::uint8_t {
        finish, ///< Publish end/failure.
        emit,   ///< Yield one item to the consumer.
        next,   ///< Invoke the next local step.
        child,  ///< Await the child task.
        park,   ///< Await an external wake.
        defer   ///< Resume through the home executor.
    };
    using call_type = source_step (*)(void*, const error&) noexcept; ///< Static step thunk.
    kind action = kind::finish;                                      ///< Driver opcode.
    call_type call = nullptr;                                        ///< Next local step.
    void (*resume)(work_item*) noexcept = nullptr;                   ///< Asynchronous resume thunk.
    task* child = nullptr; ///< Borrowed child, for child opcode.
    error result;          ///< Terminal error (empty = end).
#if STEPWRIGHT_TRACK_TASKS
    const char* name = nullptr; ///< Static resume-step name (tracking builds only).
#endif
};

template <class D, class T>
/// A source<T> written as steps, like machine<D>. Entry step: `step begin()`.
/// Steps are `step (D::*)()` or `step (D::*)(const error&)` (optionally noexcept)
/// and return one of: emit<&D::f>(value) (hand one item to the consumer; f runs on
/// the following pull), end(), fail(e), then<&D::f>(), after<&D::f>(child),
/// park<&D::f>() (resumed by wake()), defer<&D::f>() (requires bind()). As in
/// machine<D>, a no-argument step skips failures: the stream fails instead.
///
/// Lazy and pull-driven: nothing runs before the first try_pull()/on_ready().
/// try_pull() runs the driver on the consumer's thread until it emits, ends,
/// fails or suspends on a child, park or deferral (then it returns `pending`).
/// A suspended source resumes on the thread that completed its child or wake (or
/// on its home executor), runs to the next emit/end/fail, buffers that outcome
/// (at most one item) and signals the consumer's readiness wait. Constant stack
/// depth, no allocation per step or item, exclusive step execution.
///
/// Cancellation: children receive the token given to set_token() (empty by
/// default), never the consumer's readiness token. Cancelling a readiness wait
/// only cancels that wait; the source keeps its state and children. A requested
/// set_token() token wakes a current park with errc::cancelled (or runs the
/// optional D::on_cancel() hook on the requesting thread).
///
/// Lifetime: destroy only while not suspended, i.e. before the first pull,
/// between pulls of a source that is not waiting on a child, park or deferral,
/// or after end/failure; and never with an outstanding readiness wait (Debug
/// asserts). Children, wakers and the home executor must respect that lifetime.
/// Wakes delivered after end/failure are dropped. Optional hook:
/// annotate(error&) adds failure context, as in machine<D>.
class source_machine : public source<T>, private work_item {
public:
    using step = source_step; ///< Convenient step return type for derived classes.

    /// Debug asserts that the source is not suspended and has no readiness wait.
    ~source_machine() override {
        const auto s = state_.load(std::memory_order_acquire);
        STEPWRIGHT_ASSERT((s & waiting) == 0 && consumer_.idle(),
                          "destroying a source with an outstanding readiness wait");
        STEPWRIGHT_ASSERT((s & busy) == 0 || owned_, "destroying a suspended source_machine");
        if (held_ || (s & item_ready) != 0) {
            std::destroy_at(&item_.value);
        }
#if STEPWRIGHT_TRACK_TASKS
        if (live_started_) {
            detail::live_registry::instance().remove(live_);
        }
#endif
    }

    /// Thread-safe result for the current/next park (same rules as machine::wake);
    /// dropped once the stream has ended or failed. May run the source inline.
    void wake(error result = {}) noexcept { deliver<false>(std::move(result)); }
    /// Deliver only to a current park without a pending result; returns acceptance.
    bool wake_if_parked(error result = {}) noexcept { return deliver<true>(std::move(result)); }
    /// Set a home executor (non-owning) before the first pull; resumptions and
    /// deferrals run there.
    void bind(executor& ex) noexcept { home_ = &ex; }
    /// The home executor, or nullptr.
    executor* home() const noexcept { return home_; }
    /// Token passed to children and observed by parks. Set it while not suspended
    /// (typically before the first pull). A requested token wakes a current park
    /// with errc::cancelled.
    void set_token(const cancel_token& token) { cancellation_.arm(token, &cancel, this); }
    /// The token passed to children (empty by default).
    const cancel_token& token() const noexcept { return cancellation_.token(); }
    /// Whether set_token()'s token has been cancelled.
    bool cancel_requested() const noexcept { return token().cancel_requested(); }

    /// Consumer: run the source until it produces an outcome or suspends.
    pull<T> try_pull() final {
        STEPWRIGHT_ASSERT(consumer_.idle(), "try_pull with an outstanding readiness wait");
        if (held_) {
            held_ = false;
            return take();
        }
        if (owned_) {
            switch (drive<true>(next_(this, {}))) {
                case outcome::emitted:
                    return take();
                case outcome::finished:
                    return pull<T>::failure(terminal_);
                case outcome::suspended:
                    return pull<T>::pending();
            }
        }
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((old & terminal) != 0) {
                return pull<T>::failure(terminal_);
            }
            if ((old & item_ready) == 0) {
                return pull<T>::pending();
            }
            // Take the published item and own the driver for the next pull.
            if (state_.compare_exchange_weak(old, (old & ~item_ready) | busy | running,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        owned_ = true;
        return take();
    }
    /// Consumer: complete when try_pull() can make progress; see source::on_ready.
    /// A source between pulls runs to its next outcome here (on this thread).
    void on_ready(completion done, cancel_token token) final {
        STEPWRIGHT_ASSERT(consumer_.idle(), "concurrent readiness waits on a source");
        if (!held_ && owned_) {
            if (drive<true>(next_(this, {})) == outcome::emitted) {
                held_ = true;
            }
        }
        if (held_ || owned_) {
            done({});
            return;
        }
        consumer_.prepare(done, token);
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((old & (item_ready | terminal)) != 0) {
                consumer_.unprepare();
                done({});
                return;
            }
            STEPWRIGHT_ASSERT((old & busy) != 0, "source_machine readiness at an unowned boundary");
            if (state_.compare_exchange_weak(old, old | waiting, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        if (token.can_be_cancelled()) {
            consumer_.arm(token, &cancel_wait, this);
        }
    }

protected:
    /// Finish the stream successfully (try_pull returns end from now on).
    static step end() noexcept { return {}; }
    /// Finish the stream with result (empty means end), adding D's context.
    static step fail(error result) noexcept {
        step transition;
        transition.result = std::move(result);
        return transition;
    }
    /// Create a code/message failure with error::make's ownership and context.
    /// Deduction keeps existing fail({}) calls unambiguous (end).
    template <class Code>
        requires(std::is_same_v<Code, errc> || std::is_same_v<Code, std::errc> ||
                 std::is_same_v<Code, std::error_code>)
    static step fail(Code code, std::string_view message = {}) {
        return fail(error::make(code, message));
    }
    template <auto Fn>
    /// Hand value to the consumer; Fn runs on the following pull.
    step emit(T value) noexcept {
        std::construct_at(&item_.value, std::move(value));
        return transition<Fn>(step::kind::emit);
    }
    template <auto Fn>
    /// Run Fn in the same driver loop; its optional error argument is empty.
    static step then() noexcept {
        return transition<Fn>(step::kind::next);
    }
    template <auto Fn>
    /// Start an idle child with token(), then run Fn with its result.
    static step after(task& child) noexcept {
        auto value = transition<Fn>(step::kind::child);
        value.child = &child;
        value.resume = &resume_child<Fn>;
        return value;
    }
    template <auto Fn>
    /// Await a current/upcoming wake(); run Fn with its result.
    static step park() noexcept {
        return transition<Fn>(step::kind::park);
    }
    template <auto Fn>
    /// Post the next step to the bound home executor (required); Fn runs on a
    /// thread of that executor (see machine::defer).
    static step defer() noexcept {
        auto value = transition<Fn>(step::kind::defer);
        value.resume = &deferred<Fn>;
        return value;
    }

private:
    friend D;
    source_machine() noexcept = default;

    // State word. busy: between a pull that started a run and the publication of
    // its outcome (set while the consumer owns the driver, or while suspended).
    static constexpr std::uint32_t running = 1;
    static constexpr std::uint32_t claimed = 2;
    static constexpr std::uint32_t wake_ready = 4;
    static constexpr std::uint32_t parked = 8;
    static constexpr std::uint32_t deferring = 16;
    static constexpr std::uint32_t deferred_ready = 32;
    static constexpr std::uint32_t child_ready = 64;
    static constexpr std::uint32_t child_failed = 128;
    static constexpr std::uint32_t busy = 256;
    static constexpr std::uint32_t item_ready = 512;
    static constexpr std::uint32_t terminal = 1024;
    static constexpr std::uint32_t waiting = 2048;

    enum class outcome : std::uint8_t {
        emitted,
        finished,
        suspended
    };

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
                      "stepwright source step must be step (D::*)() or step (D::*)(const error&), "
                      "optionally noexcept");
        auto& self = *static_cast<source_machine*>(ctx);
        if constexpr (valid_no_arg<Fn>) {
            if (result) {
                return fail(result);
            }
        }
        STEPWRIGHT_ASSERT(!self.step_guard_.exchange(true, std::memory_order_acquire),
                          "concurrent source_machine steps");
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
    static source_machine& owner(work_item* item) noexcept {
        if (!item) {
            std::terminate();
        }
        return *static_cast<source_machine*>(item);
    }
    template <auto Fn>
    static void resume(work_item* item) noexcept {
        auto& self = owner(item);
        error result = self.consume();
        self.template drive<false>(invoke<Fn>(&self, result));
    }
    template <auto Fn>
    static void resume_child(work_item* item) noexcept {
        auto& self = owner(item);
        error result = self.consume_child();
        self.template drive<false>(invoke<Fn>(&self, result));
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
        self.template drive<false>(invoke<Fn>(&self, {}));
    }

    // The item was constructed by emit(), reached through a step thunk GCC cannot
    // follow from a freshly constructed source; its -Wmaybe-uninitialized here is a
    // false positive (held_/item_ready guarantee construction).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
    pull<T> take() noexcept {
        pull<T> result = pull<T>::of(std::move(item_.value));
        std::destroy_at(&item_.value);
        return result;
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    // Sync: the consumer drives (try_pull/on_ready) and owns the driver; an item
    // stays in the buffer for the caller and nothing is published. Async: a
    // resumption publishes the outcome and signals a registered readiness wait;
    // after that publication the driver does not touch the object again.
    // Live registry hooks (no code without STEPWRIGHT_TRACK_TASKS). Written by the
    // driver that owns `running`, before ownership can pass to another thread.
    void track([[maybe_unused]] detail::live_state s,
               [[maybe_unused]] const char* resume = nullptr) noexcept {
#if STEPWRIGHT_TRACK_TASKS
        if (!live_started_) {
            live_started_ = true;
            detail::live_registry::instance().add(live_, detail::type_name<D>());
        }
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
    template <bool Sync>
    outcome drive(step value) noexcept {
        track(detail::live_state::running);
        for (;;) {
            error result;
            switch (value.action) {
                case step::kind::emit:
                    next_ = value.call;
                    track(detail::live_state::suspended, resume_name(value));
                    if constexpr (!Sync) {
                        publish_item();
                    }
                    return outcome::emitted;
                case step::kind::finish:
                    finish<Sync>(std::move(value.result));
                    return outcome::finished;
                case step::kind::next:
                    break;
                case step::kind::child:
                    work_item::run = value.resume;
                    track(detail::live_state::waiting_child, resume_name(value));
                    value.child->start({.fn =
                                            [](void* ctx, const error& delivered) noexcept {
                                                static_cast<source_machine*>(ctx)->deliver_child(
                                                    delivered);
                                            },
                                        .ctx = this},
                                       token());
                    if (!await_child(result)) {
                        // Ownership released: another thread may already run (and
                        // complete) the source. Only the consumer-owned flag is
                        // touched, and only by the consumer itself.
                        if constexpr (Sync) {
                            owned_ = false;
                        }
                        return outcome::suspended;
                    }
                    track(detail::live_state::running);
                    break;
                case step::kind::park:
                    work_item::run = value.resume;
                    track(detail::live_state::parked, resume_name(value));
                    if (token().can_be_cancelled()) {
                        // Same SC park/cancel handshake as machine<D>.
                        state_.fetch_or(parked, std::memory_order_seq_cst);
                        if constexpr (!requires(D& derived) { derived.on_cancel(); }) {
                            if (cancel_requested()) {
                                wake_if_parked(detail::cancelled_error());
                            }
                        }
                    } else {
                        state_.fetch_or(parked, std::memory_order_acq_rel);
                    }
                    if (suspend(wake_ready)) {
                        if constexpr (Sync) {
                            owned_ = false;
                        }
                        return outcome::suspended;
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
                        if constexpr (Sync) {
                            owned_ = false;
                        }
                        static_cast<void>(suspend(deferred_ready)); // not posted: not ready
                        home.post(static_cast<work_item&>(*this));
                        return outcome::suspended;
                    }
                    home_->post(static_cast<work_item&>(*this));
                    if (suspend(deferred_ready)) {
                        if constexpr (Sync) {
                            owned_ = false;
                        }
                        return outcome::suspended;
                    }
                    track(detail::live_state::running);
                    state_.fetch_and(~(deferred_ready | deferring), std::memory_order_acq_rel);
                    break;
            }
            value = value.call(this, result);
        }
    }
    void publish_item() noexcept {
        auto old = state_.load(std::memory_order_relaxed);
        while (!state_.compare_exchange_weak(old, (old & ~(busy | running | waiting)) | item_ready,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
        }
        if ((old & waiting) != 0) {
            consumer_.resolve({});
        }
    }
    template <bool Sync>
    void finish(error result) noexcept {
#if STEPWRIGHT_TRACK_TASKS
        detail::live_registry::instance().remove(live_);
#endif
        if (result) {
            result.add_context(detail::type_name<D>());
            if constexpr (requires(D& derived, error& e) { derived.annotate(e); }) {
                static_cast<D&>(*this).annotate(result);
            }
        }
        terminal_ = std::move(result);
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            // A wake may have claimed the slot but not yet published it.
            if ((old & claimed) != 0 && (old & wake_ready) == 0) {
                std::this_thread::yield();
                old = state_.load(std::memory_order_acquire);
                continue;
            }
            if ((old & claimed) != 0) {
                result_ = {}; // claimed blocks other wakers; drop the pending wake
            }
            if (state_.compare_exchange_weak(old, terminal, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        if constexpr (Sync) {
            owned_ = false;
        } else {
            if ((old & waiting) != 0) {
                consumer_.resolve({});
            }
        }
    }
    static void cancel(void* ctx) noexcept {
        auto& self = *static_cast<source_machine*>(ctx);
        if constexpr (requires(D& derived) { derived.on_cancel(); }) {
            static_cast<D&>(self).on_cancel();
        } else {
            if ((self.state_.load(std::memory_order_seq_cst) & parked) != 0) {
                self.wake_if_parked(detail::cancelled_error());
            }
        }
    }
    static void cancel_wait(void* ctx) noexcept {
        auto& self = *static_cast<source_machine*>(ctx);
        auto old = self.state_.load(std::memory_order_acquire);
        for (;;) {
            if ((old & waiting) == 0) {
                return; // the publication won and resolves the wait
            }
            if (self.state_.compare_exchange_weak(old, old & ~waiting, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                break;
            }
        }
        self.consumer_.resolve(detail::cancelled_error());
    }
    template <bool Conditional>
    bool deliver(error result) noexcept {
        auto old = state_.load(std::memory_order_acquire);
        for (;;) {
            if constexpr (Conditional) {
                if ((old & parked) == 0) {
                    return false;
                }
            }
            if ((old & (terminal | claimed)) != 0) {
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

    union item_slot {
        item_slot() noexcept {}
        item_slot(const item_slot&) = delete;
        item_slot& operator=(const item_slot&) = delete;
        ~item_slot() {}
        T value;
    };

    executor* home_ = nullptr;
    step::call_type next_ = &invoke<&D::begin>;
    cancel_registration cancellation_;
    error result_;       // wake slot
    error child_result_; // child failure slot
    error terminal_;     // end (empty) or failure, published by `terminal`
    detail::wait_core consumer_;
    std::atomic<std::uint32_t> state_{busy | running};
    std::atomic<bool> step_guard_{false};
    bool owned_ = true; // consumer-only: the consumer owns the driver between pulls
    bool held_ = false; // consumer-only: the buffer holds an unpublished item
    item_slot item_;
#if STEPWRIGHT_TRACK_TASKS
    bool live_started_ = false; // driver-only: registered once, at the first run
    detail::live_node live_;
#endif
};
} // namespace stepwright
