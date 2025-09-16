# cmake_minimum_required(VERSION 3.20)
# project(wangcai_orderbook_cpp LANGUAGES CXX)

# find_package(Boost REQUIRED)

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# # 基础库源文件
# set(LIB_SOURCES
#     src/orderbook.cpp
#     src/call_auction_engine.cpp
#     src/con_auction_engine.cpp
#     src/OrderLoader.cpp
#     src/close_auction_engine.cpp
#     src/data_manager.cpp
#     src/backtest_engine.cpp
#     src/multi_backtest_engine.cpp
# )

# # 基础库
# add_library(wangcai_orderbook_cpp STATIC ${LIB_SOURCES})
# target_include_directories(wangcai_orderbook_cpp PUBLIC include ${Boost_INCLUDE_DIRS})

# set_target_properties(wangcai_orderbook_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# set(TASKFLOW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/taskflow)

# if(EXISTS ${TASKFLOW_DIR}/taskflow/taskflow.hpp)
#     message(STATUS "Found Taskflow: ${TASKFLOW_DIR}")
#     target_include_directories(wangcai_orderbook_cpp PRIVATE ${TASKFLOW_DIR})
# else()
#     message(FATAL_ERROR "Taskflow not found at ${TASKFLOW_DIR}. Please ensure it's cloned to 3rd_party/taskflow")
# endif()

# # Remove the need for TASKFLOW_INCLUDE_DIR cache variable
# unset(TASKFLOW_INCLUDE_DIR CACHE)

# # =========================
# # Python bindings
# # =========================
# option(BUILD_PYTHON "Build Python bindings" OFF)

# if (DEFINED SKBUILD OR BUILD_PYTHON)
#   set(PYBIND11_FINDPYTHON ON)
#   find_package(pybind11 CONFIG REQUIRED)

#   pybind11_add_module(wangcai_cpp MODULE
#       bindings/wangcai_py.cpp
#   )

#   # 链接你的静态库
#   target_link_libraries(wangcai_cpp PRIVATE wangcai_orderbook_cpp)
#   target_include_directories(wangcai_cpp PRIVATE
#       ${CMAKE_CURRENT_SOURCE_DIR}/include
#       ${Boost_INCLUDE_DIRS}
#       ${TASKFLOW_DIR}
#   )
#   target_compile_features(wangcai_cpp PRIVATE cxx_std_20)

#   # 通过 scikit-build-core 安装到 wheel 的包目录（变量由其提供）
#   if (DEFINED SKBUILD)
#     install(TARGETS wangcai_cpp
#       LIBRARY DESTINATION ${SKBUILD_PLATLIB_DIR}/wangcai_bt
#       RUNTIME DESTINATION ${SKBUILD_PLATLIB_DIR}/wangcai_bt
#       ARCHIVE DESTINATION ${SKBUILD_PLATLIB_DIR}/wangcai_bt)
#   endif()
# endif()
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
    src/close_auction_engine.cpp
    src/data_manager.cpp
    src/backtest_engine.cpp
    src/multi_backtest_engine.cpp
)

# 基础库
add_library(wangcai_orderbook_cpp STATIC ${LIB_SOURCES})
target_include_directories(wangcai_orderbook_cpp PUBLIC include ${Boost_INCLUDE_DIRS})

set_target_properties(wangcai_orderbook_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
message(STATUS "SRC DIR = ${CMAKE_CURRENT_SOURCE_DIR}")

set(TASKFLOW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/taskflow)

if(EXISTS ${TASKFLOW_DIR}/taskflow/taskflow.hpp)
    message(STATUS "Found Taskflow: ${TASKFLOW_DIR}")
    target_include_directories(wangcai_orderbook_cpp PRIVATE ${TASKFLOW_DIR})
else()
    message(FATAL_ERROR "Taskflow not found at ${TASKFLOW_DIR}. Please ensure it's cloned to 3rd_party/taskflow")
endif()

# Remove the need for TASKFLOW_INCLUDE_DIR cache variable
unset(TASKFLOW_INCLUDE_DIR CACHE)

# =========================
# Python bindings
# =========================
option(BUILD_PYTHON "Build Python bindings" ON)

if (DEFINED SKBUILD OR BUILD_PYTHON)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 CONFIG REQUIRED)
  
  # <--- 添加第1处：查找 TBB
  # 因为 C++ 并行算法 std::execution::par_unseq 依赖 TBB 作为后端
  find_package(TBB REQUIRED)
  message(STATUS "Found TBB, linking for parallel algorithms support.")
  # --->

  pybind11_add_module(wangcai_cpp MODULE
      bindings/wangcai_py.cpp
  )

  # 链接你的静态库和 TBB
  target_link_libraries(wangcai_cpp PRIVATE 
      wangcai_orderbook_cpp   # 链接你自己的核心逻辑库
      TBB::tbb                # <--- 添加第2处：链接 TBB
  )

  target_include_directories(wangcai_cpp PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${Boost_INCLUDE_DIRS}
      ${TASKFLOW_DIR}
  )
  target_compile_features(wangcai_cpp PRIVATE cxx_std_20)

  # 通过 scikit-build-core 安装到 wheel 的包目录（变量由其提供）
  if (DEFINED SKBUILD)
    install(TARGETS wangcai_cpp
      LIBRARY DESTINATION ${SKBUILD_PLATLIB_DIR}/wangcai_bt
      RUNTIME DESTINATION ${SKBUILD_PLATLIB_DIR}/wangcai_bt
      ARCHIVE DESTINATION ${SKBUILD_PLATLIB_DIR}/wangcai_bt)
  endif()
endif()