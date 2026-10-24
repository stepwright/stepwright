#pragma once
#include <stepwright/machine.hpp>

#include <cstddef>
struct layout_sample : stepwright::machine<layout_sample> {
    step begin();
};
std::size_t ndebug_size();
std::size_t ndebug_align();
