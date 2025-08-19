/*
 * @Author: chenlisen
 * @Date: 2025-08-18 11:25:53
 * @LastEditTime: 2025-08-18 12:45:45
 * @FilePath: /wangcai_cpp/include/multi_backtest_engine.h
 */

#pragma once
#include "backtest_engine.hpp"
#include "orderbook.h"
#include <vector>
#include <memory>
#include <string>
#include <map>
#include <queue>
#include <vector>
#include <functional>
#include <ranges>
#include <taskflow/taskflow.hpp>

namespace wangcai {

struct EngineManager {
    std::string symbol;
    std::unique_ptr<BacktestEngine> engine; 
    std::vector<Event> events;              // 合约对应的所有事件 按时间排序
    std::size_t idx = 0;                    // 下一条待处理事件索引
};


struct QueueEvent {
    std::string datetime; // 统一的时间戳
    std::size_t engineIndex; //引擎序列号
    Event event; // 事件

    auto operator<=>(const QueueEvent& other) const {
        return datetime <=> other.datetime;
    }
    bool operator==(const QueueEvent& other) const = default;
};

class MultiBacktestEngine {
public:
    explicit  MultiBacktestEngine(const std::vector<std::string>& symbols,
                        const std::string& date,
                        const std::string& data_path);

    void registerStrategy(std::shared_ptr<Strategy> strategy);

    void run();
    
    std::map<std::string, Position> getPositions() const;

    double getTotalPnL() const;
private:
    std::vector<EngineManager> engines_;         
    std::vector<std::shared_ptr<Strategy>> strategies_; 
    std::string date_;                          
    std::string data_path_;    
};
}; // namespace wangcai