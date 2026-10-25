# Run one example and check it: exit code 0, and every "// expect: <text>" line
# of its source present (as a substring of one output line, in order).
#
#   cmake -DEXAMPLE=<executable> -DSOURCE=<example.cpp> -P check_example.cmake

execute_process(
    COMMAND ${EXAMPLE}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 120)
message("${output}")
if(errors)
    message("stderr:\n${errors}")
endif()
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${EXAMPLE} exited with ${status}")
endif()

file(STRINGS ${SOURCE} expectations REGEX "^// expect: ")
if(NOT expectations)
    message(FATAL_ERROR "${SOURCE} lists no expected output")
endif()
string(REPLACE ";" "\\;" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
set(position 0)
list(LENGTH lines count)
foreach(expectation IN LISTS expectations)
    string(REGEX REPLACE "^// expect: " "" wanted "${expectation}")
    set(found FALSE)
    while(position LESS count)
        list(GET lines ${position} line)
        math(EXPR position "${position} + 1")
        string(FIND "${line}" "${wanted}" at)
        if(NOT at EQUAL -1)
            set(found TRUE)
            break()
        endif()
    endwhile()
    if(NOT found)
        message(FATAL_ERROR "expected output line not found (in order): '${wanted}'")
    endif()
endforeach()
