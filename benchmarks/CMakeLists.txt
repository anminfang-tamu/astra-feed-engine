add_executable(astra_itch_trace_profile ItchTraceProfile.cpp)
target_compile_features(astra_itch_trace_profile PRIVATE cxx_std_20)
target_compile_options(astra_itch_trace_profile PRIVATE -Wall -Wextra -Wpedantic)

add_executable(astra_order_book_benchmark OrderBookBenchmark.cpp)
target_link_libraries(astra_order_book_benchmark PRIVATE astra::feed_engine)
target_compile_features(astra_order_book_benchmark PRIVATE cxx_std_20)
target_compile_options(astra_order_book_benchmark PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(astra_itch_book_replay_benchmark ItchBookReplayBenchmark.cpp)
target_link_libraries(astra_itch_book_replay_benchmark
    PRIVATE astra::feed_engine)
target_compile_features(astra_itch_book_replay_benchmark PRIVATE cxx_std_20)
target_compile_options(astra_itch_book_replay_benchmark PRIVATE
    -Wall -Wextra -Wpedantic)

if(ASTRA_BUILD_TESTS)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME OrderBookBenchmarkCliTest
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/OrderBookBenchmarkCliTest.py
                    $<TARGET_FILE:astra_order_book_benchmark>)
        set_tests_properties(OrderBookBenchmarkCliTest PROPERTIES TIMEOUT 30)

        add_test(
            NAME ItchBookReplayIdentityTest
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ItchBookReplayIdentityTest.py
                    $<TARGET_FILE:astra_itch_book_replay_benchmark>)
        set_tests_properties(ItchBookReplayIdentityTest PROPERTIES TIMEOUT 30)

    endif()

    # The verifier's symbol contract is intentionally tied to optimized code;
    # keep it out of Debug/multi-config test sets where inlining and helper
    # emission are materially different.
    find_program(ASTRA_BASH_EXECUTABLE bash)
    find_program(ASTRA_OBJDUMP_EXECUTABLE objdump)
    if(Python3_Interpreter_FOUND AND ASTRA_BASH_EXECUTABLE)
        add_test(
            NAME OrderBookHotPathVerifierContractTest
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/tests/test_verify_order_book_hot_path.py)
        set_tests_properties(OrderBookHotPathVerifierContractTest
                             PROPERTIES TIMEOUT 30)
    endif()
    if(ASTRA_BASH_EXECUTABLE)
        add_test(
            NAME OrderBookAcceptanceHarnessTest
            COMMAND ${ASTRA_BASH_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/OrderBookAcceptanceHarnessTest.sh
                    ${PROJECT_SOURCE_DIR}/scripts/run_order_book_acceptance.sh)
        set_tests_properties(OrderBookAcceptanceHarnessTest
                             PROPERTIES TIMEOUT 30)
    endif()
    if(ASTRA_BASH_EXECUTABLE AND ASTRA_OBJDUMP_EXECUTABLE AND
       NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(
            NAME OrderBookHotPathDisassemblyTest
            COMMAND ${ASTRA_BASH_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/scripts/verify_order_book_hot_path.sh
                    --schema redesign_v1
                    $<TARGET_FILE:astra_itch_book_replay_benchmark>)
        set_tests_properties(OrderBookHotPathDisassemblyTest
                             PROPERTIES TIMEOUT 30)
    endif()
endif()
