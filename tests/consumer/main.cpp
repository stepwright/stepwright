#include <stepwright/stepwright.hpp>

bool peer_check();

namespace {
struct operation : stepwright::machine<operation> {
    int steps = 0;
    step begin() { return defer<&operation::finish>(); }
    step finish() {
        ++steps;
        return done();
    }
};
} // namespace

int main() {
    stepwright::thread_pool pool(2);
    operation op;
    op.bind(pool);
    bool success = false;
    op.start({.fn = [](void* ctx,
                       const stepwright::error& e) noexcept { *static_cast<bool*>(ctx) = !e; },
              .ctx = &success});
    pool.wait_idle();
    return success && op.steps == 1 && peer_check() && stepwright::version.major == 0 &&
                   stepwright::version.minor == 1
               ? 0
               : 1;
}
