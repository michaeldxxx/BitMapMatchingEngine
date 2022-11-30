cmake_minimum_required(VERSION 3.16)
project(untitled)

set(CMAKE_CXX_STANDARD 20)

find_package(GTest REQUIRED)
find_package(benchmark REQUIRED)
set(Boost_INCLUDE_DIR "../boost_1_79_0/boost")
find_package(Boost REQUIRED)

set(benchmark_INC_DIR "../benchmark")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx -dM -march=native -fconcepts")

add_executable(untitled main.cpp book.hpp)
target_include_directories(untitled PRIVATE ${Boost_INCLUDE_DIR} ${benchmark_INC_DIR})
target_link_libraries(untitled PRIVATE ${Boost_LIBRARIES} benchmark::benchmark ${GTEST_LIBRARIES})
