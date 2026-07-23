cmake_minimum_required(VERSION 3.20)

project(astra_feed_engine
    VERSION 0.1.0
    LANGUAGES CXX
)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel
    )
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASTRA_BUILD_APPS       "Build application binaries" ON)
option(ASTRA_BUILD_TESTS      "Build tests"                ON)
option(ASTRA_BUILD_BENCHMARKS "Build benchmarks"           OFF)
option(ASTRA_ENABLE_DPDK      "Build DPDK receiver support" OFF)
option(ASTRA_ENABLE_IPO       "Enable interprocedural optimization (LTO)" OFF)

if(ASTRA_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT ASTRA_IPO_SUPPORTED
        OUTPUT ASTRA_IPO_ERROR
        LANGUAGES CXX
    )
    if(NOT ASTRA_IPO_SUPPORTED)
        message(FATAL_ERROR
            "ASTRA_ENABLE_IPO=ON, but IPO/LTO is unavailable: ${ASTRA_IPO_ERROR}"
        )
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

add_library(astra_feed_engine STATIC
    src/book/BookCapacityProfile.cpp
    src/book/BookManager.cpp
    src/book/OrderBook.cpp
    src/book/OrderTable.cpp
    src/book/PriceLevelStore.cpp
    src/codec/MoldUdpDecoder.cpp
    src/core/Time.cpp
    src/engine/MarketDataEngine.cpp
    src/metrics/LatencyRecorder.cpp
    src/metrics/StageLatencyRecorder.cpp
    src/replay/itch/ItchMoldUdpSource.cpp
    src/parser/ItchParser.cpp
    src/source/DualUdpBatchReceiver.cpp
    src/source/DualUdpReceiver.cpp
    src/source/UdpBatchReceiver.cpp
    src/source/UdpReceiver.cpp
    src/source/UdpSender.cpp
    src/utils/CpuAffinity.cpp
)

add_library(astra::feed_engine ALIAS astra_feed_engine)

target_compile_features(astra_feed_engine PUBLIC cxx_std_20)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(astra_feed_engine PUBLIC _GNU_SOURCE)
endif()

target_include_directories(astra_feed_engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(astra_feed_engine
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(ASTRA_ENABLE_DPDK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ASTRA_ENABLE_DPDK requires Linux")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
    target_sources(astra_feed_engine PRIVATE src/source/DpdkReceiver.cpp)
    target_compile_definitions(astra_feed_engine PUBLIC ASTRA_ENABLE_DPDK=1)
    target_link_libraries(astra_feed_engine PRIVATE PkgConfig::DPDK)
endif()

if(ASTRA_BUILD_APPS)
    find_package(Threads REQUIRED)

    add_executable(md_engine            apps/md_engine_main.cpp)
    target_link_libraries(md_engine     PRIVATE astra::feed_engine)

    add_executable(itch_moldudp_sender  apps/itch_moldudp_sender_main.cpp)
    target_link_libraries(itch_moldudp_sender
        PRIVATE
            astra::feed_engine
            Threads::Threads
    )
endif()

if(ASTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ASTRA_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
