cmake_minimum_required(VERSION 3.16)
project(trading_system_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
    add_compile_options(/W4 /wd4201 /wd4324)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(TRADING_SOURCES
    src/core/status.c
    src/core/clock.c
    src/core/log.c
    src/core/events.c
    src/core/config.c
    src/core/seqwatch.c
    src/observability/metrics.c
    src/persistence/journal.c
    src/persistence/journal_replay.c
    src/oms/oms.c
    src/rms/rms.c
    src/ems/ems.c
    src/ems/venue_mock.c
    src/ems/venue_replay.c
    src/ems/venue_dummy.c
    src/ems/venue_kraken.c
    src/ems/kraken_http.c
    src/ems/kraken_crypto.c
    src/strategy/replay.c
    src/strategy/momentum.c
    src/strategy/book_imbalance.c
)

add_library(trading STATIC ${TRADING_SOURCES})
target_include_directories(trading PUBLIC include)

if(NOT MSVC)
    target_link_libraries(trading PUBLIC m)
else()
    target_link_libraries(trading PUBLIC winhttp)
endif()

add_executable(trading_sim src/app/main.c)
target_link_libraries(trading_sim PRIVATE trading)

add_executable(trading_bench src/app/bench.c)
target_link_libraries(trading_bench PRIVATE trading)

enable_testing()

add_executable(trading_tests
    tests/testutil.c
    tests/test_core.c
    tests/test_oms.c
    tests/test_rms.c
    tests/test_ems.c
    tests/test_journal.c
    tests/test_pipeline.c
    tests/test_hardening.c
    tests/test_main.c
)
target_include_directories(trading_tests PRIVATE tests)
target_link_libraries(trading_tests PRIVATE trading)

add_test(NAME trading_tests COMMAND trading_tests)
