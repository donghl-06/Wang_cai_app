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

// 单个合约的CSV数据
struct SymbolData {
    std::string symbol;        // 合约代码
    std::string cstick_csv;    // tick数据CSV字符串
    std::string order_csv;     // 委托数据CSV字符串  
    std::string trade_csv;     // 成交数据CSV字符串
    std::string csbar1d_csv;   // 日线数据CSV字符串（包含涨跌停限制）
};

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
    // 从CSV字符串初始化（Python传入多个合约的DataFrame.to_csv()生成的字符串）
    explicit MultiBacktestEngine(const std::vector<SymbolData>& symbol_data_list);

    void registerStrategy(std::shared_ptr<Strategy> strategy);

    void run();
    
    std::map<std::string, Position> getPositions() const;

    double getTotalPnL() const;
private:
    std::vector<EngineManager> engines_;         
    std::vector<std::shared_ptr<Strategy>> strategies_;  
};
}; // namespace wangcai