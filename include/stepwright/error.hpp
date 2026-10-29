#pragma once

#include <stepwright/config.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

// Payload blocks are recycled unless AddressSanitizer is active, where every block
// goes straight to ::operator new/delete so use-after-free stays detectable. Both
// modes allocate blocks the same way and never change any class layout.
#if defined(__SANITIZE_ADDRESS__)
#define STEPWRIGHT_DETAIL_ERROR_RECYCLING 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STEPWRIGHT_DETAIL_ERROR_RECYCLING 0
#endif
#endif
#ifndef STEPWRIGHT_DETAIL_ERROR_RECYCLING
#define STEPWRIGHT_DETAIL_ERROR_RECYCLING 1
#endif

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

// Constant-initialized storage whose destructor never destroys the category.
union immortal_error_category {
    error_category value;
    constexpr immortal_error_category() noexcept : value() {}
    immortal_error_category(const immortal_error_category&) = delete;
    immortal_error_category& operator=(const immortal_error_category&) = delete;
    // NOLINTNEXTLINE(modernize-use-equals-default): must not destroy the member.
    ~immortal_error_category() {}
};
inline constinit const immortal_error_category error_category_instance{};
} // namespace detail

/// Thread-safe process-lifetime category named "stepwright"; no per-call allocation.
/// Never destroyed, so errors stay comparable and printable during static destruction.
inline const std::error_category& category() noexcept {
    return detail::error_category_instance.value;
}
/// Convert a library code to a value; thread-safe, no allocation.
inline std::error_code make_error_code(errc code) noexcept {
    return {static_cast<int>(code), category()};
}

namespace detail {
/// Where a payload lives, and so how its last handle releases it.
enum class error_storage : std::uint8_t {
    immortal, ///< Static, shared, never freed; no reference counting.
    block,    ///< A recycled fixed-size block (error_blocks).
    large     ///< An exact ::operator new allocation for a long message.
};

/// Failure payload header; the message bytes follow it in the same storage.
struct error_payload {
    std::atomic<std::size_t> refs{1};
    std::error_code code;
    std::size_t length;
    std::array<const char*, 8> frames{};
    std::size_t dropped = 0;
    std::uint8_t count = 0;
    error_storage storage;

    error_payload(std::error_code ec, std::size_t size, error_storage kind) noexcept
        : code(ec)
        , length(size)
        , storage(kind) {}
    char* text() noexcept { return reinterpret_cast<char*>(this + 1); }
    const char* text() const noexcept { return reinterpret_cast<const char*>(this + 1); }
};

/// Every recycled payload occupies one block of this size.
inline constexpr std::size_t error_block_size = 256;
/// Longest message stored in a block (144 bytes on 64-bit targets).
inline constexpr std::size_t error_message_capacity = error_block_size - sizeof(error_payload);
/// Blocks per magazine; a thread caches at most two magazines.
inline constexpr std::size_t error_magazine_size = 32;
/// Most free blocks the shared depot keeps; beyond it magazines are freed.
inline constexpr std::size_t error_depot_limit = 4096;
/// Whether free blocks are recycled (false under AddressSanitizer).
inline constexpr bool error_recycling = STEPWRIGHT_DETAIL_ERROR_RECYCLING != 0;
static_assert(sizeof(error_payload) <= error_block_size / 2);

/// Payload for a message-less library code, or nullptr for an unknown value.
/// Constructed once in static storage that is never destroyed, so it stays valid
/// during static initialization and destruction and is never freed or written.
inline error_payload* immortal_library_payload(errc code) noexcept {
    constexpr int first = static_cast<int>(errc::cancelled);
    constexpr int last = static_cast<int>(errc::broken_promise);
    const int value = static_cast<int>(code);
    if (value < first || value > last) {
        return nullptr;
    }
    using table = std::array<error_payload, last - first + 1>;
    alignas(table) static std::array<std::byte, sizeof(table)> storage;
    // A trivially destructible pointer: no exit-time destructor is registered.
    static auto* const payloads = ::new (static_cast<void*>(storage.data()))
        table{error_payload(make_error_code(errc::cancelled), 0, error_storage::immortal),
              error_payload(make_error_code(errc::timed_out), 0, error_storage::immortal),
              error_payload(make_error_code(errc::closed), 0, error_storage::immortal),
              error_payload(make_error_code(errc::broken_promise), 0, error_storage::immortal)};
    return &(*payloads)[static_cast<std::size_t>(value - first)];
}
/// The same for a std::error_code: nullptr unless it holds a stepwright code.
inline error_payload* immortal_error_payload(std::error_code code) noexcept {
    return code.category() == category() ? immortal_library_payload(static_cast<errc>(code.value()))
                                         : nullptr;
}

/// A free block, linked into a magazine through its own storage.
struct error_free_block {
    error_free_block* next;          ///< Next block of the same magazine.
    error_free_block* next_magazine; ///< In the depot, on a magazine's first block.
    std::size_t rounds;              ///< In the depot, on a magazine's first block.
};
static_assert(sizeof(error_free_block) <= error_block_size);

/// An intrusive list of up to error_magazine_size free blocks.
struct error_magazine {
    error_free_block* head = nullptr;
    std::size_t rounds = 0;

    bool full() const noexcept { return rounds == error_magazine_size; }
    void push(void* block) noexcept {
        head = ::new (block) error_free_block{.next = head, .next_magazine = nullptr, .rounds = 0};
        ++rounds;
    }
    void* pop() noexcept {
        error_free_block* block = head;
        head = block->next;
        --rounds;
        return block;
    }
    void free_all() noexcept {
        while (head) {
            error_free_block* next = head->next;
            ::operator delete(head);
            head = next;
        }
        rounds = 0;
    }
};

/// Process-wide store of full (or, after thread exit, partial) magazines that
/// balances blocks between threads. Never destroyed and always reachable from its
/// static storage. Only list splicing happens under the lock: no allocation,
/// deallocation or other user code.
class error_block_depot {
public:
    error_block_depot(const error_block_depot&) = delete;
    error_block_depot& operator=(const error_block_depot&) = delete;
    ~error_block_depot() = delete;

    static error_block_depot& instance() noexcept {
        alignas(error_block_depot) static std::array<std::byte, sizeof(error_block_depot)> storage;
        // A trivially destructible pointer: no exit-time destructor is registered.
        static auto* const depot = ::new (static_cast<void*>(storage.data())) error_block_depot();
        return *depot;
    }
    /// A racy hint: false means a take() would very likely find nothing.
    bool stocked() const noexcept { return blocks_hint_.load(std::memory_order_relaxed) != 0; }
    /// Move one magazine into the empty out; false if the depot is empty.
    bool take(error_magazine& out) noexcept {
        const std::lock_guard lock(lock_);
        error_free_block* first = magazines_;
        if (!first) {
            return false;
        }
        magazines_ = first->next_magazine;
        blocks_ -= first->rounds;
        blocks_hint_.store(blocks_, std::memory_order_relaxed);
        out.head = first;
        out.rounds = first->rounds;
        return true;
    }
    /// Store the non-empty in and leave it empty; false (in unchanged) if full.
    bool put(error_magazine& in) noexcept {
        {
            const std::lock_guard lock(lock_);
            if (blocks_ + in.rounds > error_depot_limit) {
                return false;
            }
            in.head->next_magazine = magazines_;
            in.head->rounds = in.rounds;
            magazines_ = in.head;
            blocks_ += in.rounds;
            blocks_hint_.store(blocks_, std::memory_order_relaxed);
        }
        in = {};
        return true;
    }
    /// Free blocks currently stored (a snapshot, for tests and diagnostics).
    std::size_t size() const noexcept { return blocks_hint_.load(std::memory_order_relaxed); }

private:
    error_block_depot() noexcept = default;

    std::mutex lock_;
    error_free_block* magazines_ = nullptr;
    std::size_t blocks_ = 0;
    std::atomic<std::size_t> blocks_hint_{0};
};

/// Per-thread magazines. Constant-initialized and trivially destructible: thread
/// exit is handled by error_cache_guard, registered only once the thread keeps
/// blocks. `previous` is always empty or full; `loaded` may be partial.
struct error_block_cache {
    static constexpr std::uint8_t fresh = 0; ///< Holds no blocks; no exit hook yet.
    static constexpr std::uint8_t live = 1;  ///< Exit hook registered.
    static constexpr std::uint8_t dead = 2;  ///< Exit hook ran; never cache again.

    error_magazine loaded;
    error_magazine previous;
    std::uint8_t state = fresh;
};

/// Fixed-size payload block allocator: per-thread magazines over a shared depot
/// (Bonwick and Adams' magazine design). Steady-state allocate/release, including
/// allocation on one thread and release on another, performs no heap allocation.
class error_blocks {
public:
    /// One error_block_size block; may throw std::bad_alloc.
    [[gnu::returns_nonnull]] static void* allocate() {
        if constexpr (!error_recycling) {
            return ::operator new(error_block_size);
        } else {
            error_block_cache& cache = local();
            if (cache.loaded.rounds == 0) {
                if (cache.previous.rounds != 0) {
                    std::swap(cache.loaded, cache.previous);
                } else if (!refill(cache)) {
                    return ::operator new(error_block_size);
                }
            }
            return cache.loaded.pop();
        }
    }
    /// Return a block from allocate() (from any thread) to this thread's cache.
    static void release(void* block) noexcept {
        if constexpr (!error_recycling) {
            ::operator delete(block);
        } else {
            error_block_cache& cache = local();
            if (cache.state != error_block_cache::live) {
                if (cache.state == error_block_cache::dead) {
                    // Thread exit or static destruction already flushed the cache.
                    ::operator delete(block);
                    return;
                }
                enlist(cache);
            }
            if (cache.loaded.full()) {
                if (cache.previous.rounds != 0) {
                    spill(cache.previous);
                }
                cache.previous = std::exchange(cache.loaded, {});
            }
            cache.loaded.push(block);
        }
    }
    /// This thread's magazines (tests and diagnostics).
    static error_block_cache& local() noexcept {
        static thread_local error_block_cache cache;
        return cache;
    }

private:
    friend class error_cache_guard;

    // Hand a non-empty magazine to the depot, or free it outside the depot lock.
    static void spill(error_magazine& magazine) noexcept {
        if (!error_block_depot::instance().put(magazine)) {
            magazine.free_all();
        }
    }
    // Load an empty cache from the depot; false if there is nothing to take.
    static bool refill(error_block_cache& cache) noexcept {
        if (cache.state == error_block_cache::dead) {
            return false;
        }
        error_block_depot& depot = error_block_depot::instance();
        if (!depot.stocked()) {
            return false;
        }
        if (cache.state == error_block_cache::fresh) {
            enlist(cache);
        }
        return depot.take(cache.loaded);
    }
    static void enlist(error_block_cache& cache) noexcept;
};

/// Thread-exit hook for error_block_cache: marks it dead, then moves its blocks to
/// the depot (or frees them). Later releases on this thread bypass the cache.
class error_cache_guard {
public:
    explicit error_cache_guard(error_block_cache& cache) noexcept : cache_(&cache) {}
    error_cache_guard(const error_cache_guard&) = delete;
    error_cache_guard& operator=(const error_cache_guard&) = delete;
    ~error_cache_guard() {
        cache_->state = error_block_cache::dead;
        for (error_magazine* magazine : {&cache_->loaded, &cache_->previous}) {
            if (magazine->rounds != 0) {
                error_blocks::spill(*magazine);
            }
        }
    }

private:
    error_block_cache* cache_;
};

inline void error_blocks::enlist(error_block_cache& cache) noexcept {
    // Constructed on this thread's first call; its destructor runs at thread exit.
    static thread_local const error_cache_guard guard(cache);
    cache.state = error_block_cache::live;
}
} // namespace detail

/// Cheap, copyable error value. Distinct handles are safe on different threads;
/// serialize mutation/move/destruction of the same handle. Context is copy-on-write
/// so changing one handle never mutates another handle's observable payload.
/// Message-less library codes share static payloads without reference counting;
/// other payloads use recycled blocks unless their message is long.
class error {
    using payload = detail::error_payload;
    using storage = detail::error_storage;

public:
    /// Empty success handle; construction/copy/move of empty handles uses no atomics.
    error() noexcept = default;
    /// Retain the payload (if any); other may be read concurrently, not changed.
    error(const error& other) noexcept : data_(other.data_) {
        if (data_ && data_->storage != storage::immortal) {
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

    /// Own a copy of code/message (zero code means success). A library code without
    /// a message shares a static payload and never allocates; other payloads use a
    /// recycled block, or one allocation for a message longer than the block holds.
    /// May throw on allocation failure; independent calls are thread-safe.
    static error make(std::error_code code, std::string_view message = {}) {
        error result;
        if (code) {
            if (message.empty()) {
                result.data_ = detail::immortal_error_payload(code);
            }
            if (!result.data_) {
                result.data_ = allocate(code, message);
            }
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
        payload* current = data_;
        if (!current) {
            return;
        }
        if (current->storage == storage::immortal ||
            current->refs.load(std::memory_order_acquire) != 1) {
            payload* clone =
                allocate(current->code, std::string_view(current->text(), current->length));
            clone->frames = current->frames;
            clone->count = current->count;
            clone->dropped = current->dropped;
            error copy;
            copy.data_ = clone;
            swap(copy); // copy now releases this handle's share of the original
            current = clone;
        }
        if (current->count < current->frames.size()) {
            current->frames[current->count++] = where;
        } else {
            ++current->dropped;
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
        const bool fits = message.size() <= detail::error_message_capacity;
        void* block = fits ? detail::error_blocks::allocate()
                           : ::operator new(sizeof(payload) + message.size());
        auto* data =
            ::new (block) payload(code, message.size(), fits ? storage::block : storage::large);
        if (!message.empty()) {
            std::memcpy(data->text(), message.data(), message.size());
        }
        return data;
    }
    void release() noexcept { release_payload(data_); }
    // Out of line: only the null check is inlined into every destruction site,
    // and the static immortal payloads (never freed) stay out of the optimizer's
    // view of the free path.
    [[gnu::noinline]] static void release_payload(payload* data) noexcept {
        const storage kind = data->storage;
        if (kind == storage::immortal || data->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }
        data->~payload();
        if (kind == storage::block) {
            detail::error_blocks::release(data);
        } else {
            ::operator delete(data);
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
