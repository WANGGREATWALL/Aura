# GCC/Clang syntax-only negative consumer checks. Never link or execute target code.
foreach(kind IN ITEMS 2 0 1)
    execute_process(COMMAND "${COMPILER}" -std=c++11 -fsyntax-only -DAURA_STATIC
        "-DFLOW_RETURN_KIND=${kind}" "-I${SOURCE_DIR}/inc"
        "${SOURCE_DIR}/test/flow_callable_contract.cpp"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(kind EQUAL 2)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Valid int consumer did not compile: ${output}${error}")
        endif()
    else()
        if(result EQUAL 0 OR NOT error MATCHES "A flow task must return int")
            message(FATAL_ERROR "Expected ordinary callable contract diagnostic (${kind}): ${output}${error}")
        endif()
    endif()
endforeach()
message(STATUS "C++11 int accepted; void and long rejected by flow contract")
