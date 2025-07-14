cmake_minimum_required(VERSION 3.20)
project(wangcai_orderbook_cpp LANGUAGES CXX)

find_package(Boost REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SRC src/*.cpp)
add_library(wangcai_orderbook_cpp STATIC ${SRC})
target_include_directories(wangcai_orderbook_cpp PUBLIC include ${Boost_INCLUDE_DIRS})

add_executable(test src/main.cpp)
target_link_libraries(test PRIVATE wangcai_orderbook_cpp)