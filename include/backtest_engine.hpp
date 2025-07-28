#pragma once

#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"
#include "OrderLoader.h"
#include <queue>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <fstream>

namespace wangcai_orderbook_cpp {

// 市场数据结构
struct MarketData {
    std::string datetime;
    std::string symbol;
    Price best_bid;
    Price best_ask;
    Quantity bid_volume;
    Quantity ask_volume;
    Price last_price;
    Quantity last_volume;
    std::string event_type; // "order", "trade", "cancel"
};

// 交易记录结构（对应cstra文件格式）
struct TradeRecord {
    std::string datetime;     // 交易时间
    std::string sym;          // 合约代码
    double price;             // 成交价格（原始价格，单位元）
    double size;              // 成交数量
    uint64_t bidorderid;      // 买方订单ID
    uint64_t askorderid;      // 卖方订单ID
    uint64_t tradeid;         // 交易ID
    int exectype;             // 执行类型（1=正常成交）
    std::string tradebsflag;  // 交易标志
    int channelno;            // 通道号
    int64_t bizindex;         // 业务索引
    
    TradeRecord(const std::string& dt, const std::string& symbol, double px, double sz, 
                uint64_t bid_id, uint64_t ask_id, uint64_t trade_id, 
                int exec_type = 1, const std::string& trade_flag = " ", 
                int channel = 2012, int64_t biz_idx = 0)
        : datetime(dt), sym(symbol), price(px), size(sz), 
          bidorderid(bid_id), askorderid(ask_id), tradeid(trade_id),
          exectype(exec_type), tradebsflag(trade_flag), channelno(channel), bizindex(biz_idx) {}
};

// 用户订单结构
struct UserOrder {
    std::string order_id;
    std::string symbol;
    Direction direction;
    OrderType order_type;
    Price price;
    Quantity volume;
    std::string strategy_id; // 用于区分不同策略
};

// 持仓信息
struct Position {
    std::string symbol;
    int64_t quantity;      // 正数为多头，负数为空头
    double avg_cost;       // 平均成本价
    double unrealized_pnl; // 未实现盈亏
    double realized_pnl;   // 已实现盈亏
};

// 用户策略接口
class Strategy {
public:
    virtual ~Strategy() = default;
    
    // 处理市场数据，返回要下的订单列表
    virtual std::vector<UserOrder> onMarketData(const MarketData& data) = 0;
    
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
    // 初始化订单簿和引擎
    void initialize();
    
    // 发布市场数据
    void publishMarketData(const std::string& event_type, const std::string& datetime = "");
    
    // 处理用户订单
    void processUserOrder(const UserOrder& user_order);
    
    // 更新持仓
    void updatePosition(const std::string& strategy_id, const std::string& symbol, 
                       Direction direction, Quantity volume, Price price);
    
    // 打印结果
    void printResults() const;
    
    // 记录交易
    void recordTrade(const Execution& ex, const std::string& datetime);
    
    // 记录撤单
    void recordCancel(uint64_t order_id, const std::string& datetime);
    
    // 记录撤单信息，包含订单详细信息
    void recordCancelWithOrderInfo(uint64_t original_id, const std::string& datetime, 
                                  std::shared_ptr<Order> order_info);
    
    // 成员变量
    std::string symbol_;
    std::string date_;
    std::string data_path_;
    
    // 订单簿和引擎
    std::unique_ptr<OrderBook> orderbook_;
    std::unique_ptr<CallAuctionEngine> call_engine_;
    std::unique_ptr<ConAuctionEngine> con_engine_;
    std::unique_ptr<CloseAuctionEngine> close_engine_; // 收盘集合竞价引擎
    
    
    // 策略管理
    std::vector<std::shared_ptr<Strategy>> strategies_;
    std::map<std::string, std::map<std::string, Position>> positions_; // strategy_id -> symbol -> position
    
    // 市场数据队列
    std::queue<MarketData> market_data_queue_;
    std::function<void(const MarketData&)> market_data_callback_;
    
    // 事件数据
    bool continuous_mode_;
    bool closing_mode_ = false;                       // 是否进入收盘集合竞价阶段
    std::string current_datetime_;  // 当前事件时间
    std::string last_brk_datetime_; // 最后一条BRK事件时间（连续竞价成交回调用）
    
    // 订单ID管理
    uint64_t next_order_id_;
    uint64_t next_trade_id_;        // 交易ID生成器
    std::map<std::string, uint64_t> user_order_mapping_; // user_order_id -> system_order_id
    
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

} // namespace wangcai_orderbook_cpp 