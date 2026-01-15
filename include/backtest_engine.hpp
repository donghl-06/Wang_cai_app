#pragma once

#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"
#include "data_manager.h"
#include "OrderLoader.h"
#include "market_info.h"
#include <queue>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <atomic>  // 用于 atomic<bool>

namespace wangcai {

// 前向声明和类型别名
using OrderDetail = wangcai::OrderDetail;
using TradeDetail = wangcai::TradeDetail; 
using Snapshot = wangcai::Snapshot;

// 用户策略接口
class Strategy {
public:
    virtual ~Strategy() = default;
    
    // 接收订单事件，返回要处理的事件列表（下单或撤单）  
    virtual std::vector<UserEvent> onOrderEvent(const OrderDetail& order) = 0;
    
    // 接收成交事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onTradeEvent(const TradeDetail& trade) = 0;

    //  接受Tick事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) = 0;
    
    // 接收用户自定义事件，返回要处理的事件列表（下单或撤单）
    // 参数 index 对应 Python 层自定义数据列表的索引，实际数据由 Python 绑定层传入
    // 默认返回空事件，避免影响未实现该接口的策略
    virtual std::vector<UserEvent> onCustomEvent(size_t index) { return {}; }
    
    // 新的统一交易回调接口（包含持仓管理）
    virtual void onTradeCallback(const TradeCallback& callback) = 0;
    
    // 下单回调接口
    virtual void onOrderCallback(const OrderCallback& callback) = 0;
    
    // 原有接口保留（兼容性）
    virtual void onOrderFilled(const std::string& order_id, Price price, Quantity volume) = 0;
    virtual void onOrderCancelled(const std::string& order_id, const std::string& reason) = 0;
    
    // 获取策略ID
    virtual std::string getStrategyId() const = 0;
    
    // 检查策略是否已完成所有处理（基于atomic状态变量）
    virtual bool isProcessingComplete() const { return processing_complete_.load(); }
    
    // 获取当前持仓（由策略基类管理）
    int64_t getPosition(const std::string& symbol) const {
        return position_manager_.getPosition(symbol);
    }

    void setPosition(const std::string& symbol, int64_t quantity) {
        position_manager_.setInitPosition(symbol, quantity);
    }
    
    // 获取所有持仓
    const std::map<std::string, int64_t>& getAllPositions() const {
        return position_manager_.getAllPositions();
    }
    
    // 公有方法：更新持仓（供 BacktestEngine 调用）
    void updateStrategyPosition(const std::string& symbol, int64_t quantity_change) {
        position_manager_.updatePosition(symbol, quantity_change);
    }
    
    // 异步处理状态管理（公有方法，供Python策略调用）
    void markProcessingStarted() {
        processing_complete_.store(false);
    }
    
    void markProcessingComplete() {
        processing_complete_.store(true);
    }

protected:
    // 持仓管理器，由基类提供
    mutable StrategyPositionManager position_manager_;
    
    // 异步处理状态管理（用于异步策略）
    mutable std::atomic<bool> processing_complete_{true}; // 默认同步策略已完成
    
    // 更新持仓的受保护方法，供派生类使用
    void updatePosition(const std::string& symbol, int64_t quantity_change) {
        position_manager_.updatePosition(symbol, quantity_change);
    }
};

// 回测引擎
class BacktestEngine {
public:
    // 从CSV字符串初始化回测引擎（Python传入DataFrame.to_csv()生成的字符串）
    BacktestEngine(const std::string& symbol, 
                   const std::string& cstick_csv,
                   const std::string& order_csv, 
                   const std::string& trade_csv,
                   const std::string& csbar1d_csv);
    
    // 注册策略
    void registerStrategy(std::shared_ptr<Strategy> strategy);
    
    // 获取回测结果
    std::map<std::string, Position> getPositions() const;
    double getTotalPnL() const;
    
    // 设置回调
    void setMarketDataCallback(std::function<void(const MarketData&)> callback);
    
    // 交易记录相关
    void enableTradeRecording(const std::string& output_file);  // 启用交易记录
    void writeTradeRecords() const;                             // 输出交易记录到CSV
    const std::vector<TradeRecord>& getTradeRecords() const;    // 获取所有交易记录
    
    // === 严格主动单模式（欠债限制功能）===
    // 开启后，虚拟主动单吃掉的历史订单需要被真实市场消耗后才能下新的主动单
    void setStrictActiveOrderMode(bool enabled);
    bool isStrictActiveOrderMode() const;
    bool hasDebt() const;  // 检查是否有未还清的欠债

    // === 用户自定义事件支持 ===
    // 提交用户事件（用于外部驱动，例如自定义数据推送）
    void submitUserEvent(const UserEvent& user_event);
    // 检查是否包含某个用户订单（用于撤单路由）
    bool hasUserOrder(const std::string& order_id) const;
    // 设置当前时间（用于自定义事件推送时的回调时间）
    void setCurrentDatetimeForCustomEvent(const std::string& datetime);

    void processEvent(const Event& ev);
    void finish();
    
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
    
    // 新增：创建交易回调对象
    TradeCallback createTradeCallback(const std::string& strategy_id, const std::string& order_id, 
                                    Direction direction, Quantity volume, Price price, 
                                    const std::string& datetime, char match_type);
    
    // 新增：通知策略统一交易回调
    void notifyStrategyTradeCallback(const std::string& strategy_id, const TradeCallback& callback);
    
    // 新增：创建下单回调对象
    OrderCallback createOrderCallback(const std::string& strategy_id, const std::string& order_id,
                                    Direction direction, Quantity volume, Price price, 
                                    const std::string& datetime);
    
    // 新增：通知策略下单回调
    void notifyStrategyOrderCallback(const std::string& strategy_id, const OrderCallback& callback);
    
    // 检查所有策略是否已完成处理
    bool areAllStrategiesComplete() const;
    
    // 等待所有策略完成处理
    void waitForStrategiesCompletion();
    
    // 成员变量
    std::string symbol_;
    std::string cstick_csv_;
    std::string order_csv_;
    std::string trade_csv_;
    std::string csbar1d_csv_;
    
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
    bool price_cage_checked_ = false;                 // 是否已检查过价格笼子
    std::string current_datetime_;  // 当前事件时间
    std::string last_brk_datetime_; // 最后一条BRK事件时间（连续竞价成交回调用）
    
    // 订单ID管理
    uint64_t next_order_id_;
    uint64_t next_trade_id_;        // 交易ID生成器
    std::map<std::string, uint64_t> user_order_mapping_; // user_order_id -> system_order_id
    std::map<uint64_t, Direction> user_order_direction_;  // system_order_id -> direction
    
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