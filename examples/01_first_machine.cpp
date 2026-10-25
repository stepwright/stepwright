// 01: a first machine.
//
// An operation is a class deriving from stepwright::machine<Self>. Each step is an
// ordinary member function returning the next transition; the base class runs
// them in a loop. This one greets a user in three steps:
//
//   begin()      validate the input, then<&greet::look_up>()
//   look_up()    after<&greet::on_user>(lookup_): start a child, continue later
//   on_user(e)   the child's result: fail(e) passes the error on, done() succeeds
//
// Nothing here is asynchronous: the child completes inside its start(), and the
// driver loop simply carries on (no recursion, no allocation). Errors are values:
// each machine an error passes through adds its name to the error's context.
#include "example_support.hpp"
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace example {
struct user {
    std::string_view name;
    int id;
};
constexpr std::array<user, 2> directory{{{.name = "alice", .id = 1}, {.name = "bob", .id = 2}}};

// A child operation: find a user by name, or fail.
struct lookup_user : stepwright::machine<lookup_user> {
    std::string_view name;
    int found_id = 0;

    step begin() {
        for (const auto& entry : directory) {
            if (entry.name == name) {
                found_id = entry.id;
                return done();
            }
        }
        return fail(stepwright::error::make(std::errc::invalid_argument, "no such user"));
    }
};

struct greet : stepwright::machine<greet> {
    std::string_view name;
    std::string greeting; // the result, read after completion

    step begin() {
        if (name.empty()) {
            return fail(stepwright::error::make(std::errc::invalid_argument, "empty name"));
        }
        return then<&greet::look_up>(); // the next step, in the same driver loop
    }
    step look_up() {
        lookup_.name = name;
        return after<&greet::on_user>(lookup_); // start the child, resume with its result
    }
    step on_user(const stepwright::error& e) {
        if (e) {
            return fail(e); // the error keeps lookup_user's context and gains ours
        }
        greeting = "hello, " + std::string(name) + " (id " + std::to_string(lookup_.found_id) + ")";
        return done();
    }

private:
    lookup_user lookup_; // children are plain members: no allocation per run
};

void print_failure(std::string_view who, const stepwright::error& e) {
    std::printf("greet(%.*s) failed: %.*s\n", static_cast<int>(who.size()), who.data(),
                static_cast<int>(e.message().size()), e.message().data());
    for (const char* frame : e.context()) {
        std::printf("  via %s\n", frame); // innermost first
    }
}
} // namespace example

int main() {
    example::greet op; // a machine is restartable: one object, three runs
    int failures = 0;
    for (const std::string_view name : {"alice", "mallory", ""}) {
        op.name = name;
        const stepwright::error e = example::run_blocking(op);
        if (e) {
            example::print_failure(name, e);
            ++failures;
        } else {
            std::printf("greet(%.*s): %s\n", static_cast<int>(name.size()), name.data(),
                        op.greeting.c_str());
        }
    }
    return failures == 2 ? 0 : 1;
}

// expect: greet(alice): hello, alice (id 1)
// expect: greet(mallory) failed: no such user
// expect:   via example::lookup_user
// expect:   via example::greet
// expect: greet() failed: empty name
