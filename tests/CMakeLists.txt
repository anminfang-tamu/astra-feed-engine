find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()

if(TARGET GTest::gtest_main)
    set(ASTRA_GTEST_MAIN GTest::gtest_main)
else()
    set(ASTRA_GTEST_MAIN gtest_main)
endif()

add_executable(astra_tests
    book/BookManagerTest.cpp
    book/BookStorageFootprintTest.cpp
    book/HotArenaAlignmentTest.cpp
    book/OrderBookModelTest.cpp
    book/OrderBookTest.cpp
    book/OrderTableTest.cpp
    book/PriceLevelStoreTest.cpp
    codec/MoldUdpDecoderTest.cpp
    engine/MarketDataEngineTest.cpp
    metrics/LatencyRecorderTest.cpp
    protocol/MoldUdpControlScheduleTest.cpp
    replay/itch/ItchMoldUdpSourceTest.cpp
    replay/itch/ItchParserTest.cpp
    source/UdpReceiverTest.cpp
    utils/BitmapTest.cpp
    utils/CpuAffinityTest.cpp
    utils/FixedMmapArrayTest.cpp
    utils/MappedArrayTest.cpp
    utils/MappedObjectArrayTest.cpp
    utils/Sha256Test.cpp
)

target_link_libraries(astra_tests
    PRIVATE
        astra::feed_engine
        ${ASTRA_GTEST_MAIN}
)
target_include_directories(astra_tests PRIVATE ${PROJECT_SOURCE_DIR}/benchmarks)
target_compile_options(astra_tests PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(astra_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(TARGET md_engine)
    add_test(
        NAME MdEngineCapacityConfigTest
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/book/MdEngineCapacityConfigTest.sh
                $<TARGET_FILE:md_engine>
    )
    add_test(
        NAME RunEngineDpdkConfigTest
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/source/RunEngineDpdkConfigTest.sh
                ${PROJECT_SOURCE_DIR}/scripts/run_engine_dpdk.sh
    )
    add_test(
        NAME RunEngineUdpConfigTest
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/source/RunEngineUdpConfigTest.sh
                ${PROJECT_SOURCE_DIR}/scripts/run_engine_udp.sh
    )
endif()

if(TARGET itch_moldudp_sender)
    add_test(
        NAME RunSenderConfigTest
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/source/RunSenderConfigTest.sh
                ${PROJECT_SOURCE_DIR}/scripts/run_sender.sh
    )
endif()

if(TARGET md_engine)
    add_test(
        NAME DeriveBookCapacityEvidenceTest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/book/DeriveBookCapacityEvidenceTest.py
                ${PROJECT_SOURCE_DIR}/scripts/derive_book_capacity_evidence.py
                $<TARGET_FILE:md_engine>
    )
else()
    add_test(
        NAME DeriveBookCapacityEvidenceTest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/book/DeriveBookCapacityEvidenceTest.py
                ${PROJECT_SOURCE_DIR}/scripts/derive_book_capacity_evidence.py
    )
endif()

if(TARGET itch_moldudp_sender AND TARGET md_engine)
    add_test(
        NAME ItchMoldUdpSenderIntegrationTest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/source/ItchMoldUdpSenderIntegrationTest.py
                $<TARGET_FILE:itch_moldudp_sender>
                $<TARGET_FILE:md_engine>
                ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/book-capacity-evidence-v2.txt
    )
    set_tests_properties(
        ItchMoldUdpSenderIntegrationTest
        PROPERTIES TIMEOUT 180
    )
endif()
