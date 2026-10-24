include(${PROJECT_SOURCE_DIR}/cmake/Dependencies.cmake)
stepwright_fetch_benchmark()

add_custom_target(all_benchmarks)
function(stepwright_add_benchmark name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE stepwright::stepwright benchmark::benchmark_main)
    stepwright_apply_warnings(${name})
    add_dependencies(all_benchmarks ${name})
endfunction()

stepwright_add_benchmark(bench_engine)
