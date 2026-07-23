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
    replay/itch/ItchParserTest.cpp
    sequencing/SequenceTrackerTest.cpp
    source/UdpReceiverTest.cpp
    utils/BitmapTest.cpp
    utils/CpuAffinityTest.cpp
    utils/FixedMmapArrayTest.cpp
    utils/MappedArrayTest.cpp
    utils/MappedObjectArrayTest.cpp
)

target_link_libraries(astra_tests
    PRIVATE
        astra::feed_engine
        ${ASTRA_GTEST_MAIN}
)

include(GoogleTest)
gtest_discover_tests(astra_tests)

if(TARGET md_engine)
    add_test(
        NAME MdEngineCapacityConfigTest
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/book/MdEngineCapacityConfigTest.sh
                $<TARGET_FILE:md_engine>
    )
endif()
