include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# stepwright_add_test(<name> [extra sources...])
function(stepwright_add_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE stepwright::stepwright GTest::gtest_main)
    stepwright_apply_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

stepwright_add_test(test_smoke)
