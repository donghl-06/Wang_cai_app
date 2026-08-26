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
    bool is_etf = false;       // 是否为ETF（ETF=三位小数/tick=10，股票=两位小数/tick=100）
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
    // 从CSV字符串初始化，处理完每只后自动释放其 CSV 字符串节省内存
    explicit MultiBacktestEngine(std::vector<SymbolData> symbol_data_list);

    void registerStrategy(std::shared_ptr<Strategy> strategy);

    void run();
    
    std::map<std::string, Position> getPositions() const;

    double getTotalPnL() const;
    
    // === 严格主动单模式（欠债限制功能）===
    void setStrictActiveOrderMode(bool enabled);
    bool isStrictActiveOrderMode() const;
    bool hasDebt() const;
    
    // === 真实成交替代模式 ===
    void setRealTradeMatchMode(bool enabled);
    bool isRealTradeMatchMode() const;
    
    // === 用户下单队列回报开关 ===
    void setQueueInfoEnabled(bool enabled);
    bool isQueueInfoEnabled() const;
    
    // === 用户自定义数据推送功能 ===
    // 设置自定义事件时间戳列表（由 Python 层传入）
    // 时间戳格式需与其他事件一致（如 "2025-11-17 09:35:00"）
    void loadCustomEventTimes(const std::vector<std::string>& datetimes);
    
    // 开启/关闭自定义数据功能（默认关闭）
    void setCustomDataEnabled(bool enabled) { custom_data_enabled_ = enabled; }
    bool isCustomDataEnabled() const { return custom_data_enabled_; }
    
    // 获取当前自定义事件索引（用于 Python 层获取对应数据）
    size_t getCurrentCustomEventIndex() const { return custom_event_idx_; }
    
private:
    std::vector<EngineManager> engines_;         
    std::vector<std::shared_ptr<Strategy>> strategies_;
    
    // === 用户自定义数据相关 ===
    bool queue_info_enabled_ = false;                    // 队列回报开关（默认关闭）
    bool custom_data_enabled_ = false;                    // 功能开关，默认关闭
    std::vector<CustomEventTime> custom_events_;         // 自定义事件时间戳列表
    size_t custom_event_idx_ = 0;                        // 当前处理的自定义事件索引
};
}; // namespace wangcai