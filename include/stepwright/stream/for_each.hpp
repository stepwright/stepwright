#pragma once

#include <stepwright/detail/wait_core.hpp>
#include <stepwright/machine.hpp>
#include <stepwright/stream/source.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Task draining a source: calls f(std::move(item)) for every item and completes
/// successfully at end, with the source's failure at failed, or with
/// errc::cancelled when its token is cancelled (an outstanding readiness wait is
/// cancelled first, through the token it was started with; between items the
/// token is checked before each pull). f may return:
/// - void: continue;
/// - bool: false stops early and completes successfully;
/// - error (or anything convertible to it): a failure stops and completes with it.
/// f must not throw (escaping exceptions terminate) and runs on the thread that
/// delivered readiness. Synchronous chains of items use constant stack depth.
/// A machine: restartable after completion; the source must outlive the run and
/// the task must outlive its completion. No allocation per item.
template <class T, class F>
class for_each_op final : public machine<for_each_op<T, F>> {
    using result_type = std::invoke_result_t<F&, T&&>;
    static_assert(std::is_void_v<result_type> || std::is_same_v<result_type, bool> ||
                      std::is_convertible_v<result_type, error>,
                  "for_each callbacks return void, bool or error");

public:
    /// Bind to s (non-owning) and store f.
    for_each_op(source<T>& s, F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : source_(&s)
        , ready_(s)
        , fn_(std::move(f)) {}
    /// The callable.
    F& function() noexcept { return fn_; }

private:
    friend class machine<for_each_op>;
    using base = machine<for_each_op>;
    using typename base::step;

    step begin() noexcept {
        for (;;) {
            if (this->cancel_requested()) {
                return base::fail(detail::cancelled_error());
            }
            pull<T> next = source_->try_pull();
            switch (next.status()) {
                case pull<T>::state::item:
                    if constexpr (std::is_void_v<result_type>) {
                        fn_(std::move(next).value());
                    } else if constexpr (std::is_same_v<result_type, bool>) {
                        if (!fn_(std::move(next).value())) {
                            return base::done();
                        }
                    } else {
                        error result = fn_(std::move(next).value());
                        if (result) {
                            return base::fail(std::move(result));
                        }
                    }
                    break;
                case pull<T>::state::pending:
                    // A no-argument continuation turns a cancelled wait into failure.
                    return base::template after<&for_each_op::begin>(ready_);
                case pull<T>::state::end:
                    return base::done();
                case pull<T>::state::failed:
                    return base::fail(next.failure_reason());
            }
        }
    }

    source<T>* source_;
    typename source<T>::ready_op ready_;
    F fn_;
};

/// Build a for_each_op draining s into f (guaranteed copy elision).
template <class T, class F>
for_each_op<T, std::decay_t<F>> for_each(source<T>& s, F&& f) {
    return for_each_op<T, std::decay_t<F>>(s, std::forward<F>(f));
}
} // namespace stepwright
