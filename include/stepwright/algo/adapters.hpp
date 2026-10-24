#pragma once

#include <stepwright/executor.hpp>
#include <stepwright/task.hpp>

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Task adapter around a callable F invoked as f(completion done, cancel_token)
/// on each start. F must arrange for done to be called exactly once (before or
/// after returning, on any thread) and must not touch the task afterwards.
/// Restartable per F's own rules; no allocation beyond storing F inline.
template <class F>
class fn_task final : public task {
public:
    /// Store f by value.
    explicit fn_task(F f) noexcept(std::is_nothrow_move_constructible_v<F>) : fn_(std::move(f)) {}

private:
    void on_start(completion done, cancel_token token) override { fn_(done, std::move(token)); }

    F fn_;
};

/// Build an fn_task from a callable (guaranteed copy elision).
template <class F>
fn_task<std::decay_t<F>> make_task(F&& f) {
    return fn_task<std::decay_t<F>>(std::forward<F>(f));
}

/// Run inner, then cleanup(ctx, result) on the completing thread, then complete
/// with inner's result. The start token is passed to inner. Cleanup runs exactly
/// once per run and must not throw. Restartable; inner and the cleanup target
/// must outlive the run. No allocation.
class finally_task final : public task {
public:
    /// Cleanup callback ABI: called once per run with the context and inner's result.
    using cleanup_fn = void (*)(void* ctx, const error& result) noexcept;

    /// Bind inner and a plain cleanup function with its context (non-owning).
    finally_task(task& inner, cleanup_fn cleanup, void* ctx) noexcept
        : inner_(&inner)
        , cleanup_(cleanup)
        , ctx_(ctx) {}
    /// Bind inner and a callable invoked as cleanup(result); non-owning.
    template <class F>
        requires(!std::is_pointer_v<F> && !std::is_function_v<F> &&
                 std::is_nothrow_invocable_v<F&, const error&>)
    finally_task(task& inner, F& cleanup) noexcept
        : finally_task(
              inner,
              [](void* ctx, const error& result) noexcept { (*static_cast<F*>(ctx))(result); },
              static_cast<void*>(&cleanup)) {}

private:
    void on_start(completion done, cancel_token token) override {
        receiver_ = done;
        inner_->start(completion::bind<&finally_task::on_inner>(this), std::move(token));
    }
    void on_inner(const error& result) noexcept {
        const completion receiver = receiver_;
        cleanup_(ctx_, result);
        receiver(result);
    }

    task* inner_;
    cleanup_fn cleanup_;
    void* ctx_;
    completion receiver_;
};

/// Complete successfully from a work item run on ex: a hop onto an executor.
/// Ignores cancellation (it completes promptly once ex runs it). Uses an embedded
/// work_item; restartable after completion; ex must outlive the run. No allocation.
class reschedule final : public task, private work_item {
public:
    /// Bind to an executor (non-owning).
    explicit reschedule(executor& ex) noexcept : ex_(&ex) { run = &on_run; }

private:
    void on_start(completion done, cancel_token /*token*/) override {
        receiver_ = done;
        ex_->post(static_cast<work_item&>(*this));
    }
    static void on_run(work_item* item) noexcept {
        if (!item) {
            std::terminate();
        }
        const completion receiver = static_cast<reschedule&>(*item).receiver_;
        receiver({});
    }

    executor* ex_;
    completion receiver_;
};
} // namespace stepwright
