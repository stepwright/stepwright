# Compile and run the exact fenced C++ blocks readers see, with no hidden prelude.
# Blocks are complete programs. Backticks inside C++ blocks are intentionally not
# used, keeping extraction simple and deterministic without a Markdown dependency.
foreach(document README.md docs/overview.md docs/guide.md)
    set(path "${PROJECT_SOURCE_DIR}/${document}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" text)
    set(index 0)
    string(MAKE_C_IDENTIFIER "${document}" id)
    # Iterate by consuming one fenced block at a time instead of treating C++
    # semicolons as CMake list delimiters.
    while(text MATCHES "```cpp\n([^`]*)```")
        set(code "${CMAKE_MATCH_1}")
        string(FIND "${text}" "```cpp\n${code}```" position)
        string(LENGTH "```cpp\n${code}```" length)
        math(EXPR next "${position} + ${length}")
        string(SUBSTRING "${text}" ${next} -1 text)
        math(EXPR index "${index} + 1")
        set(name "guide_${id}_${index}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp" "${code}")
        add_executable(${name} "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp")
        target_link_libraries(${name} PRIVATE stepwright::stepwright)
        stepwright_apply_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
    endwhile()
endforeach()
