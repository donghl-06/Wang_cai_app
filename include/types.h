/*
 * @Author: chenlisen
 * @Date: 2025-07-07 08:43:04
 * @LastEditTime: 2025-07-08 02:52:29
 * @FilePath: /wangcai_orderbook_cpp/include/types.h
 */

#pragma once

#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <atomic>
#include <chrono>

namespace wangcai_orderbook_cpp {

    inline std::atomic<uint64_t> g_order_id_counter{0};
    inline std::atomic<uint64_t> g_execution_id_counter{0};

    inline uint64_t generate_order_id() {
        return ++g_order_id_counter;
    }

    inline uint64_t generate_execution_id() {
        return ++g_execution_id_counter;
    }

    namespace TradingTime {
        constexpr auto TIME_START = std::chrono::hours(9) + std::chrono::minutes(15);       // 9:15 开始接收订单
        constexpr auto TIME_CANCEL_END = std::chrono::hours(9) + std::chrono::minutes(20);  // 9:20 停止撤单
        constexpr auto TIME_MATCH = std::chrono::hours(9) + std::chrono::minutes(25);       // 9:25 集合竞价撮合
        constexpr auto TIME_OPEN = std::chrono::hours(9) + std::chrono::minutes(30);        // 9:30 连续竞价开始
    }

    enum class Market : uint8_t {
        SH,     ///< 上交所
        SZ,     ///< 深交所
        Unknown ///< 未知
    };

    using Price = uint64_t;
    using Quantity = uint64_t;

}// namespace wangcai_orderbook_cpp