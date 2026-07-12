add_executable(astra_itch_replay_benchmark
    OrderBookBenchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/book/BookManager.cpp
    ${PROJECT_SOURCE_DIR}/src/book/OrderArena.cpp
    ${PROJECT_SOURCE_DIR}/src/book/OrderBook.cpp
    ${PROJECT_SOURCE_DIR}/src/book/PriceLevelIndex.cpp
    ${PROJECT_SOURCE_DIR}/src/parser/ItchParser.cpp
)

target_compile_features(astra_itch_replay_benchmark PRIVATE cxx_std_20)

target_include_directories(astra_itch_replay_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(astra_itch_replay_benchmark
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)
