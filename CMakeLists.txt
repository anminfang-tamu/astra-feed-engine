cmake_minimum_required(VERSION 3.20)

project(astra_feed_engine
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ASTRA_BUILD_APPS "Build application binaries" ON)
option(ASTRA_BUILD_TESTS "Build tests" ON)
option(ASTRA_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(ASTRA_ENABLE_DPDK "Build DPDK receiver support" OFF)

find_library(ZLIB_LIBRARY z REQUIRED)

add_library(astra_feed_engine STATIC
    src/book/BookManager.cpp
    src/book/OrderBook.cpp
    src/book/PriceLevel.cpp
    src/codec/BinaryDecoder.cpp
    src/codec/BinaryEncoder.cpp
    src/core/Config.cpp
    src/core/Time.cpp
    src/engine/MarketDataEngine.cpp
    src/metrics/Histogram.cpp
    src/metrics/LatencyRecorder.cpp
    src/metrics/MetricsReporter.cpp
    src/publish/ConsolePublisher.cpp
    src/publish/NullPublisher.cpp
    src/replay/GzipLineReader.cpp
    src/replay/NyseTaqParser.cpp
    src/replay/NyseTaqReplaySource.cpp
    src/replay/SymbolTable.cpp
    src/sequencing/SequenceTracker.cpp
    src/source/ReplayReceiver.cpp
    src/source/UdpReceiver.cpp
    src/source/UdpSender.cpp
    src/strategy/SimpleStrategy.cpp
    src/utils/CpuAffinity.cpp
    src/utils/Logger.cpp
    src/utils/MemoryPool.cpp
    src/utils/RingBuffer.cpp
    src/utils/Bitmap.cpp
)

add_library(astra::feed_engine ALIAS astra_feed_engine)

target_compile_features(astra_feed_engine PUBLIC cxx_std_20)

target_include_directories(astra_feed_engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(astra_feed_engine
    PRIVATE
        ${ZLIB_LIBRARY}
)

target_compile_options(astra_feed_engine
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(ASTRA_ENABLE_DPDK)
    target_sources(astra_feed_engine PRIVATE src/source/DpdkReceiver.cpp)
    target_compile_definitions(astra_feed_engine PUBLIC ASTRA_ENABLE_DPDK=1)
endif()

if(ASTRA_BUILD_APPS)
    add_executable(md_engine apps/md_engine_main.cpp)
    target_link_libraries(md_engine PRIVATE astra::feed_engine)

    add_executable(replay_player apps/replay_player_main.cpp)
    target_link_libraries(replay_player PRIVATE astra::feed_engine)

    add_executable(taq_replay_sender apps/taq_replay_sender_main.cpp)
    target_link_libraries(taq_replay_sender PRIVATE astra::feed_engine)

    add_executable(udp_book_validator apps/udp_book_validator_main.cpp)
    target_link_libraries(udp_book_validator PRIVATE astra::feed_engine)

    add_executable(replay_writer apps/replay_writer_main.cpp)
    target_link_libraries(replay_writer PRIVATE astra::feed_engine)

    add_executable(exchange_simulator apps/exchange_simulator_main.cpp)
    target_link_libraries(exchange_simulator PRIVATE astra::feed_engine)
endif()

if(ASTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
