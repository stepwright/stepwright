#include "test_layout_sample.hpp"
static_assert(STEPWRIGHT_DEBUG == 0);
std::size_t ndebug_size() {
    return sizeof(layout_sample);
}
std::size_t ndebug_align() {
    return alignof(layout_sample);
}
