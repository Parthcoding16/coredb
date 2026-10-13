cmake_minimum_required(VERSION 3.14)
project(CoreDB CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

file(GLOB_RECURSE ENGINE_SOURCES src/*.cpp)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(coredb_engine STATIC ${ENGINE_SOURCES})
target_include_directories(coredb_engine PUBLIC src)
target_compile_options(coredb_engine PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(coredb_engine PUBLIC Threads::Threads)

add_executable(coredb src/main.cpp)
target_link_libraries(coredb PRIVATE coredb_engine)

add_executable(coredb_tests tests/test_main.cpp)
target_link_libraries(coredb_tests PRIVATE coredb_engine)

add_executable(coredb_bench tests/bench.cpp)
target_link_libraries(coredb_bench PRIVATE coredb_engine)

enable_testing()
add_test(NAME coredb_tests COMMAND coredb_tests)
