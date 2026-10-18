cmake_minimum_required(VERSION 3.20)
project(Preflop C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# PokerHandEvaluator: 7-card evaluator only, no googletest/benchmarks/PLO tables.
foreach(opt BUILD_TESTS BUILD_EXAMPLES BUILD_CARD5 BUILD_CARD6 BUILD_CARD7 BUILD_PLO4 BUILD_PLO5 BUILD_PLO6)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(phevaluator
    GIT_REPOSITORY https://github.com/HenryRLee/PokerHandEvaluator.git
    GIT_TAG v0.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cpp)
FetchContent_MakeAvailable(phevaluator)
find_package(Threads REQUIRED)

# All headless code (rules engine, cards, equity) lives in one library.
add_library(trainer src/trainer/game.cpp src/solver/cards.cpp src/solver/equity.cpp
    src/solver/utility.cpp src/solver/strategy.cpp
    src/solver/mccfr.cpp src/solver/range.cpp src/trainer/trainer.cpp)
target_include_directories(trainer PUBLIC src)
target_link_libraries(trainer PUBLIC pheval)

enable_testing()
add_executable(game_test tests/game_test.cpp)
target_link_libraries(game_test trainer)
add_test(NAME game_test COMMAND game_test)
add_executable(equity_test tests/equity_test.cpp)
target_link_libraries(equity_test trainer)
target_compile_definitions(equity_test PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME equity_test COMMAND equity_test)
add_executable(utility_test tests/utility_test.cpp)
target_link_libraries(utility_test trainer)
add_test(NAME utility_test COMMAND utility_test)
add_executable(solver_test tests/solver_test.cpp)
target_link_libraries(solver_test trainer)
target_compile_definitions(solver_test PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME solver_test COMMAND solver_test)
add_executable(trainer_test tests/trainer_test.cpp)
target_link_libraries(trainer_test trainer)
target_compile_definitions(trainer_test PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME trainer_test COMMAND trainer_test)
add_executable(range_test tests/range_test.cpp)
target_link_libraries(range_test trainer)
target_compile_definitions(range_test PRIVATE REF_DIR="${CMAKE_SOURCE_DIR}/ref")
add_test(NAME range_test COMMAND range_test)

add_executable(gen_equity src/tools/gen_equity.cpp)
target_link_libraries(gen_equity trainer Threads::Threads)
target_compile_definitions(gen_equity PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_executable(validate src/tools/validate.cpp)
target_link_libraries(validate trainer)
target_compile_definitions(validate PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data" REF_DIR="${CMAKE_SOURCE_DIR}/ref")
add_executable(audit src/tools/audit.cpp)
target_link_libraries(audit trainer)
target_compile_definitions(audit PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME audit_selfcheck COMMAND audit --self-check)
add_executable(solve src/tools/solve.cpp)
target_link_libraries(solve trainer)
target_compile_definitions(solve PRIVATE DATA_DIR="${CMAKE_SOURCE_DIR}/data")

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

add_executable(preflop src/gui/main.cpp)
target_link_libraries(preflop trainer raylib Threads::Threads)
