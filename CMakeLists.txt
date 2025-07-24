cmake_minimum_required(VERSION 3.20)
project(wangcai_orderbook_cpp LANGUAGES CXX)

find_package(Boost REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 基础库源文件
set(LIB_SOURCES
    src/orderbook.cpp
    src/call_auction_engine.cpp
    src/con_auction_engine.cpp
    src/OrderLoader.cpp
)

# 基础库
add_library(wangcai_orderbook_cpp STATIC ${LIB_SOURCES})
target_include_directories(wangcai_orderbook_cpp PUBLIC include ${Boost_INCLUDE_DIRS})

# # 主程序可执行文件
# add_executable(main_program src/main.cpp)
# target_link_libraries(main_program PRIVATE wangcai_orderbook_cpp)

# # 撮合主程序
# add_executable(matching_main src/matching_main.cpp)
# target_link_libraries(matching_main PRIVATE wangcai_orderbook_cpp)

# # 集合竞价验证程序
# add_executable(call_auction_validation src/call_auction_validation.cpp)
# target_link_libraries(call_auction_validation PRIVATE wangcai_orderbook_cpp)

# # 简单测试程序
# add_executable(simple_test src/simple_test.cpp)
# target_link_libraries(simple_test PRIVATE wangcai_orderbook_cpp)

# 回测程序（包含回测引擎）
add_executable(backtest_main 
    src/backtest_main.cpp
    src/backtest_engine.cpp
)
target_link_libraries(backtest_main PRIVATE wangcai_orderbook_cpp)
target_include_directories(backtest_main PRIVATE include ${Boost_INCLUDE_DIRS})