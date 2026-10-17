cmake_minimum_required(VERSION 3.20)
project(Preflop C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(trainer src/trainer/game.cpp)
target_include_directories(trainer PUBLIC src)

enable_testing()
add_executable(game_test tests/game_test.cpp)
target_link_libraries(game_test trainer)
add_test(NAME game_test COMMAND game_test)

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

add_executable(preflop src/gui/main.cpp)
target_link_libraries(preflop trainer raylib)
