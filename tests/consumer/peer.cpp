#include <stepwright/debug/live_tasks.hpp>
#include <stepwright/stream/channel.hpp>
#include <stepwright/testing/virtual_executor.hpp>
#include <stepwright/version.hpp>

bool peer_check();

bool peer_check() {
    static_assert(STEPWRIGHT_VERSION_MAJOR == 0 && STEPWRIGHT_VERSION_MINOR == 2);
    stepwright::channel<int> channel(1);
    if (!channel.try_send(42)) {
        return false;
    }
    channel.close();
    auto result = channel.try_pull();
    stepwright::testing::virtual_executor clock;
    return result.has_item() && result.value() == 42 && channel.try_pull().at_end() &&
           clock.run_until_quiescent() == 0 && stepwright::debug::live_count() == 0;
}
