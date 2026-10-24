#include "test_layout_sample.hpp"
static_assert(STEPWRIGHT_DEBUG == 0);
std::size_t ndebug_size() {
    return sizeof(layout_sample);
}
std::size_t ndebug_align() {
    return alignof(layout_sample);
}
m2_layout ndebug_m2_layout() {
    return measure_m2_layout();
}
m3_layout ndebug_m3_layout() {
    return measure_m3_layout();
}
