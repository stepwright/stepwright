# Runnable examples (see examples/README.md). Each one is also a CTest test,
# example_<name>, that requires exit code 0 and every line its source lists in
# a trailing "// expect: <text>" comment (checked by check_example.cmake).

set(STEPWRIGHT_EXAMPLES
    01_first_machine)

add_custom_target(all_examples)
foreach(name IN LISTS STEPWRIGHT_EXAMPLES)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE stepwright::stepwright)
    stepwright_apply_warnings(${name})
    add_dependencies(all_examples ${name})
    if(STEPWRIGHT_BUILD_TESTS)
        add_test(NAME example_${name}
            COMMAND ${CMAKE_COMMAND}
                -DEXAMPLE=$<TARGET_FILE:${name}>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_example.cmake)
    endif()
endforeach()

