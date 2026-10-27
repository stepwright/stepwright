// 07: a paginated stream written as a source_machine.
//
// source_machine<Self, T> is a machine that *produces* a stream: a step may
// emit<&Self::f>(item), which hands one item to the consumer and resumes at f on
// the following pull. Between items it can do anything a machine can, such as
// awaiting a child with after(). The stream is lazy and pull-driven: nothing runs
// until the consumer asks for the next item, so a slow consumer never makes the
// producer buffer more than the item it holds.
//
// Here the stream lists users from a backend that returns them a page at a time.
// Fetching a page is a child task (a machine that hops to the pool, standing in
// for a network request); the stream emits that page's users one by one, then
// fetches the next page, and ends after the last one.
#include <stepwright/stepwright.hpp>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace example {
constexpr std::array<std::string_view, 10> all_users{
    "ada", "barbara", "edsger", "grace", "john", "ken", "leslie", "margaret", "niklaus", "tony"};
constexpr std::size_t page_size = 4;

// One request to the backend: loads page `page` into `items`.
struct fetch_page : stepwright::machine<fetch_page> {
    std::size_t page = 0;
    std::vector<std::string> items; // reused from page to page: no steady-state growth
    bool has_more = false;
    int requests = 0;

    step begin() { return defer<&fetch_page::respond>(); } // the "network" round trip
    step respond() {
        ++requests;
        items.clear();
        for (std::size_t i = page * page_size; i < all_users.size() && items.size() < page_size;
             ++i) {
            items.emplace_back(all_users[i]);
        }
        has_more = (page + 1) * page_size < all_users.size();
        return done();
    }
};

// The stream: source<std::string>, one user per pull.
struct user_stream : stepwright::source_machine<user_stream, std::string> {
    explicit user_stream(stepwright::thread_pool& pool) {
        bind(pool);
        fetch_.bind(pool);
    }
    int pages_fetched() const noexcept { return fetch_.requests; }

    step begin() { return after<&user_stream::on_page>(fetch_); }
    step on_page() { // a child failure skips this step and fails the stream
        next_ = 0;
        return then<&user_stream::next_user>();
    }
    step next_user() {
        if (next_ < fetch_.items.size()) {
            return emit<&user_stream::next_user>(fetch_.items[next_++]); // one item, then pause
        }
        if (!fetch_.has_more) {
            return end();
        }
        ++fetch_.page;
        return after<&user_stream::on_page>(fetch_);
    }

private:
    fetch_page fetch_;
    std::size_t next_ = 0;
};
} // namespace example

int main() {
    stepwright::thread_pool pool(2);
    example::user_stream users(pool);

    std::vector<std::string> seen; // for_each runs one item at a time
    auto consumer =
        stepwright::for_each(users, [&](std::string name) { seen.push_back(std::move(name)); });
    consumer.bind(pool);
    const stepwright::error e = stepwright::sync_wait(consumer);
    pool.wait_idle();

    for (std::size_t i = 0; i < seen.size(); ++i) {
        std::printf("user %zu: %s\n", i + 1, seen[i].c_str());
    }
    std::printf("%zu users from %d page requests (%zu per page)\n", seen.size(),
                users.pages_fetched(), example::page_size);
    return !e && seen.size() == example::all_users.size() && users.pages_fetched() == 3 ? 0 : 1;
}

// expect: user 1: ada
// expect: user 4: grace
// expect: user 5: john
// expect: user 10: tony
// expect: 10 users from 3 page requests (4 per page)
