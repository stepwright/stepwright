#pragma once

#include <stepwright/config.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace stepwright {
/// Library error codes; comparable to std::error_code via make_error_code().
// NOLINTNEXTLINE(performance-enum-size): the public error-code contract specifies int.
enum class errc : int {
    cancelled = 1, ///< A cooperative cancellation request won.
    timed_out,     ///< A deadline expired before the inner operation finished.
    closed,        ///< A send/publication could not proceed because its target closed.
    broken_promise ///< A producer abandoned an expected result.
};

namespace detail {
class error_category final : public std::error_category {
public:
    const char* name() const noexcept override { return "stepwright"; }
    std::string message(int value) const override {
        switch (static_cast<errc>(value)) {
            case errc::cancelled:
                return "cancelled";
            case errc::timed_out:
                return "timed_out";
            case errc::closed:
                return "closed";
            case errc::broken_promise:
                return "broken_promise";
        }
        return "unknown stepwright error";
    }
};
} // namespace detail

/// Thread-safe process-lifetime category named "stepwright"; no per-call allocation.
inline const std::error_category& category() noexcept {
    static const detail::error_category instance;
    return instance;
}
/// Convert a library code to a value; thread-safe, no allocation.
inline std::error_code make_error_code(errc code) noexcept {
    return {static_cast<int>(code), category()};
}

/// Cheap, copyable error value. Distinct handles are safe on different threads;
/// serialize mutation/move/destruction of the same handle. Context is copy-on-write
/// so changing one handle never mutates another handle's observable payload.
class error {
    struct payload {
        std::atomic<std::size_t> refs{1};
        std::error_code code;
        std::size_t length;
        std::array<const char*, 8> frames{};
        std::size_t count = 0;
        std::size_t dropped = 0;

        payload(std::error_code ec, std::size_t size) : code(ec), length(size) {}
        char* text() noexcept { return reinterpret_cast<char*>(this + 1); }
        const char* text() const noexcept { return reinterpret_cast<const char*>(this + 1); }
    };

public:
    /// Empty success handle; construction/copy/move of empty handles uses no atomics.
    error() noexcept = default;
    /// Retain the payload (if any); other may be read concurrently, not changed.
    error(const error& other) noexcept : data_(other.data_) {
        if (data_) {
            data_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    /// Transfer the payload and leave other empty; requires exclusive access to it.
    error(error&& other) noexcept : data_(std::exchange(other.data_, nullptr)) {}
    /// Retain another payload; serialize writes to this handle.
    error& operator=(const error& other) noexcept {
        error copy(other);
        swap(copy);
        return *this;
    }
    /// Transfer another payload, leaving other empty; exclusive access to both.
    error& operator=(error&& other) noexcept {
        error moved(std::move(other));
        swap(moved);
        return *this;
    }
    /// Release this handle; distinct copies keep their payload and views alive.
    STEPWRIGHT_ALWAYS_INLINE ~error() {
        // Keep the empty fast path visible even if the compiler outlines payload
        // destruction; empty transitions must not call the cold release routine.
        if (data_) {
            release();
        }
    }

    /// Own a copy of code/message in one allocation (zero code means success).
    /// May throw on allocation failure; independent calls are thread-safe.
    static error make(std::error_code code, std::string_view message = {}) {
        error result;
        if (code) {
            result.data_ = allocate(code, message);
        }
        return result;
    }
    /// Create a library-code error; same ownership/allocation rules as make(error_code).
    static error make(errc code, std::string_view message = {}) {
        return make(make_error_code(code), message);
    }
    /// Create a standard generic-code error; same rules as make(error_code).
    static error make(std::errc code, std::string_view message = {}) {
        return make(std::make_error_code(code), message);
    }
    /// True for failure; concurrent const access is safe on an unchanged handle.
    explicit operator bool() const noexcept { return data_ != nullptr; }
    /// Return the code, or {} for success; concurrent const query.
    std::error_code code() const noexcept { return data_ ? data_->code : std::error_code{}; }
    /// Borrow owned message text; valid while its payload is retained unchanged.
    std::string_view message() const noexcept {
        return data_ ? std::string_view(data_->text(), data_->length) : std::string_view{};
    }
    /// Compare a library code; concurrent const query.
    bool is(errc value) const noexcept { return code() == make_error_code(value); }
    /// Append a static-storage string pointer, cloning a shared payload first.
    /// Keep eight frames, count later drops; empty is a no-op. Exclusive access
    /// to this handle is required; distinct handles may add context concurrently.
    void add_context(const char* where) {
        if (!data_) {
            return;
        }
        if (data_->refs.load(std::memory_order_acquire) != 1) {
            error copy;
            copy.data_ = allocate(data_->code, message());
            copy.data_->frames = data_->frames;
            copy.data_->count = data_->count;
            copy.data_->dropped = data_->dropped;
            swap(copy);
        }
        if (data_->count < data_->frames.size()) {
            data_->frames[data_->count++] = where;
        } else {
            ++data_->dropped;
        }
    }
    /// Borrow kept frame pointers; valid while this payload remains unchanged.
    std::span<const char* const> context() const noexcept {
        return data_ ? std::span<const char* const>(data_->frames.data(), data_->count)
                     : std::span<const char* const>{};
    }
    /// Count frames omitted beyond eight; concurrent const query.
    std::size_t dropped_context() const noexcept { return data_ ? data_->dropped : 0; }
    /// Format code/message/context into a new string ("ok" for empty); may allocate.
    std::string describe() const {
        if (!data_) {
            return "ok";
        }
        std::string result = data_->code.message();
        if (!message().empty()) {
            result += ": ";
            result += message();
        }
        if (data_->count != 0) {
            result += " [";
            for (std::size_t i = 0; i < data_->count; ++i) {
                if (i != 0) {
                    result += " <- ";
                }
                result += data_->frames[i];
            }
            result += ']';
        }
        return result;
    }
    /// Swap handles without allocation/atomics; exclusive access to both required.
    void swap(error& other) noexcept { std::swap(data_, other.data_); }

private:
    static payload* allocate(std::error_code code, std::string_view message) {
        void* storage = ::operator new(sizeof(payload) + message.size());
        auto* data = new (storage) payload(code, message.size());
        if (!message.empty()) {
            std::memcpy(data->text(), message.data(), message.size());
        }
        return data;
    }
    void release() noexcept {
        if (data_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            data_->~payload();
            ::operator delete(data_);
        }
    }
    payload* data_ = nullptr;
};
} // namespace stepwright

namespace std {
template <>
/// Enable standard implicit error-code conversion for stepwright::errc.
struct is_error_code_enum<stepwright::errc> : true_type {};
} // namespace std
