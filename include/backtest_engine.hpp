#pragma once

#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"
#include "data_manager.h"
#include "OrderLoader.h"
#include <queue>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <fstream>

namespace wangcai {

// 用户策略接口
class Strategy {
public:
    virtual ~Strategy() = default;
    
    // 接收订单事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onOrderEvent(const Event& event) = 0;
    
    // 接收成交事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onTradeEvent(const Execution& execution, const std::string& datetime) = 0;

    //  接受Tick事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) = 0;
    
    // 处理订单成交回报
    virtual void onOrderFilled(const std::string& order_id, Price price, Quantity volume) = 0;
    
    // 处理订单取消回报
    virtual void onOrderCancelled(const std::string& order_id, const std::string& reason) = 0;
    
    // 获取策略ID
    virtual std::string getStrategyId() const = 0;
};

// 回测引擎
class BacktestEngine {
public:
    BacktestEngine(const std::string& symbol, const std::string& date, const std::string& data_path);
    
    // 注册策略
    void registerStrategy(std::shared_ptr<Strategy> strategy);
    
    // 运行回测
    void run();
    
    // 获取回测结果
    std::map<std::string, Position> getPositions() const;
    double getTotalPnL() const;
    
    // 设置回调
    void setMarketDataCallback(std::function<void(const MarketData&)> callback);
    
    // 交易记录相关
    void enableTradeRecording(const std::string& output_file);  // 启用交易记录
    void writeTradeRecords() const;                             // 输出交易记录到CSV
    const std::vector<TradeRecord>& getTradeRecords() const;    // 获取所有交易记录
    
private:
    void initialize();
    void processUserOrder(const UserOrder& user_order);
    void processUserCancel(const UserCancel& user_cancel);
    void processUserEvent(const UserEvent& user_event);
    bool tryFillImmediately(std::shared_ptr<Order> user_order);

    void updatePosition(const std::string& strategy_id, const std::string& symbol, 
                       Direction direction, Quantity volume, Price price);
    void printResults() const;
    void recordTrade(const Execution& ex, const std::string& datetime);
    void recordCancel(uint64_t order_id, const std::string& datetime);
    void recordCancelWithOrderInfo(uint64_t original_id, const std::string& datetime, 
                                  std::shared_ptr<Order> order_info);
    
    // 新增：通知策略成交
    void notifyStrategiesOnExecution(const Execution& ex);
    
    // 成员变量
    std::string symbol_;
    std::string date_;
    std::string data_path_;
    
    // 订单簿和引擎
    std::unique_ptr<OrderBook> orderbook_;
    std::unique_ptr<CallAuctionEngine> call_engine_;
    std::unique_ptr<ConAuctionEngine> con_engine_;
    std::unique_ptr<CloseAuctionEngine> close_engine_; // 收盘集合竞价引擎
    std::unique_ptr<DataManager> data_manager_;

    // 策略管理
    std::vector<std::shared_ptr<Strategy>> strategies_;
    std::map<std::string, std::map<std::string, Position>> positions_; // strategy_id -> symbol -> position
    
    // 市场数据队列
    std::queue<MarketData> market_data_queue_;
    std::function<void(const MarketData&)> market_data_callback_;
    
    // 成交事件产生的策略事件队列
    std::vector<UserEvent> pending_trade_events_;
    
    // 事件数据
    bool continuous_mode_;
    bool closing_mode_ = false;                       // 是否进入收盘集合竞价阶段
    std::string current_datetime_;  // 当前事件时间
    std::string last_brk_datetime_; // 最后一条BRK事件时间（连续竞价成交回调用）
    
    // 订单ID管理
    uint64_t next_order_id_;
    uint64_t next_trade_id_;        // 交易ID生成器
    std::map<std::string, uint64_t> user_order_mapping_; // user_order_id -> system_order_id
    
    // 虚拟订单映射 - 用于成交回调时识别虚拟订单
    std::map<uint64_t, std::string> virtual_order_strategy_; // system_order_id -> strategy_id
    std::map<uint64_t, std::string> virtual_order_local_id_; // system_order_id -> local_order_id
    
    // 价格信息
    Price prev_close_;
    Price upper_limit_;
    Price lower_limit_;
    Price actual_open_;
    
    // 交易记录
    std::vector<TradeRecord> trade_records_;  // 所有交易记录
    std::string trade_output_file_;           // 输出文件路径
    bool recording_enabled_;                  // 是否启用记录
};

} // namespace wangcai 