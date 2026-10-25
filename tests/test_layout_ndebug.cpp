#include "test_layout_sample.hpp"
static_assert(STEPWRIGHT_DEBUG == 0);
std::size_t ndebug_size() {
    return sizeof(layout_sample);
}
std::size_t ndebug_align() {
    return alignof(layout_sample);
}
primitive_layout ndebug_primitive_layout() {
    return measure_primitive_layout();
}
stream_layout ndebug_stream_layout() {
    return measure_stream_layout();
}
runtime_layout ndebug_runtime_layout() {
    return measure_runtime_layout();
}
