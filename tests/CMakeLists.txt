include(${PROJECT_SOURCE_DIR}/cmake/Dependencies.cmake)
stepwright_fetch_googletest()

# stepwright_add_test(<name> [extra sources...])
function(stepwright_add_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE stepwright::stepwright GTest::gtest_main)
    stepwright_apply_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

stepwright_add_test(test_smoke)
foreach(name error cancel executor machine_basic machine_stack machine_threads machine_cancel machine_defer)
    stepwright_add_test(test_${name})
endforeach()
foreach(name handoff event shared_task async_mutex async_shared_mutex when_all when_any timer
        delay_deadline adapters)
    stepwright_add_test(test_${name})
endforeach()
foreach(name source source_machine channel merge broadcast sources for_each)
    stepwright_add_test(test_${name})
endforeach()
stepwright_add_test(test_layout test_layout_ndebug.cpp)
stepwright_add_test(test_alloc)
stepwright_add_test(test_alloc_m2)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CODE_INSTRUMENTATION MATCHES "^(TSAN|LEAK)$")
    target_compile_definitions(test_alloc PRIVATE STEPWRIGHT_SANITIZER_ALLOC_HOOK)
    target_compile_definitions(test_alloc_m2 PRIVATE STEPWRIGHT_SANITIZER_ALLOC_HOOK)
endif()
set_source_files_properties(test_layout.cpp PROPERTIES COMPILE_OPTIONS "-UNDEBUG")
set_source_files_properties(test_layout_ndebug.cpp PROPERTIES COMPILE_DEFINITIONS "NDEBUG")

# A broken analyser invocation must not silently turn into a clean report.
# This runs a failing stub, never the host's cppcheck (analysis stays in Docker).
find_program(STEPWRIGHT_FALSE_PROGRAM false REQUIRED)
add_test(NAME regression_cppcheck_runner_failure
    COMMAND ${CMAKE_COMMAND} -E env CPPCHECK=${STEPWRIGHT_FALSE_PROGRAM}
        bash ${PROJECT_SOURCE_DIR}/scripts/run-cppcheck.sh
        -b ${CMAKE_CURRENT_BINARY_DIR}/cppcheck-failure)
set_tests_properties(regression_cppcheck_runner_failure PROPERTIES WILL_FAIL TRUE)
