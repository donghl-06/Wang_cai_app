/*
 * @Author: chenlisen
 * @Date: 2025-07-07 08:43:04
 * @LastEditTime: 2025-08-21 02:20:53
 * @FilePath: /wangcai_cpp/include/types.h
 */

#pragma once

#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <map>

namespace wangcai {
    using Price = uint64_t;
    using Quantity = uint64_t;
    inline std::atomic<uint64_t> g_order_id_counter{0};
    inline std::atomic<uint64_t> g_execution_id_counter{0};
    //枚举类型

enum class OrderStatus : uint8_t {
    Submitted,      ///< 已提交
    PartFilled,     ///< 部分成交
    Filled,         ///< 全部成交
    Cancelled,      ///< 已撤销
    Rejected        ///< 拒绝
};

enum class OrderType : uint8_t {
    Limit,              ///< 限价
    Market,             ///< 市价
    BestCounterpart,    ///< 对手价
    BestOwn             ///< 本方价
};

enum class Direction : uint8_t { Buy, Sell };

//基础类型
using Timestamp = std::chrono::time_point<
                     std::chrono::system_clock,
                     std::chrono::nanoseconds>;
                     
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

// 用户下单结构
struct UserOrder {
    std::string order_id;
    std::string symbol;
    Direction direction;
    OrderType order_type;
    Price price;
    Quantity volume;
    std::string strategy_id; // 用于区分不同策略
};

// 用户撤单结构
struct UserCancel {
    std::string order_id;     // 要撤销的订单ID
    std::string strategy_id;  // 策略ID
    
    UserCancel(const std::string& oid, const std::string& sid) 
        : order_id(oid), strategy_id(sid) {}
};

// 用户事件结构（统一的输入）
struct UserEvent {
    enum Type { ORDER, CANCEL };
    Type type;
    UserOrder order;    // 下单信息（当type为ORDER时使用）
    UserCancel cancel;  // 撤单信息（当type为CANCEL时使用）
    
    // 构造函数：下单事件
    explicit UserEvent(const UserOrder& ord) : type(ORDER), order(ord), cancel("", "") {}
    
    // 构造函数：撤单事件
    explicit UserEvent(const UserCancel& can) : type(CANCEL), order(), cancel(can) {}
};

// 持仓信息
struct Position {
    std::string symbol;
    int64_t quantity;      // 正数为多头，负数为空头
    double avg_cost;       // 平均成本价
    double unrealized_pnl; // 未实现盈亏
    double realized_pnl;   // 已实现盈亏
};

// 交易回调信息结构体
struct TradeCallback {
    std::string localid;      // 本地订单ID
    char direction;           // 'B'=买入, 'S'=卖出
    Quantity volume;          // 成交数量
    Price price;              // 成交价格（厘为单位）
    double matchamount;       // 成交金额（元）
    int64_t deltapos;         // 当前总持仓量（正数=多头，负数=空头）
    std::string matchtime;    // 成交时间
    char matchtype;           // 'T'=成交回调, 'D'=撤单回调
    
    TradeCallback(const std::string& id, char dir, Quantity vol, Price px, 
                 double amount, int64_t delta, const std::string& time, char type)
        : localid(id), direction(dir), volume(vol), price(px), 
          matchamount(amount), deltapos(delta), matchtime(time), matchtype(type) {}
};

// 下单回调信息结构体
struct OrderCallback {
    std::string time;         // 下单时间
    int exchange;             // 交易所（0=上海, 1=深圳）
    Price ask1;               // 卖一价格（厘为单位）
    Price bid1;               // 买一价格（厘为单位）
    int64_t deltapos;         // 当前总持仓量（正数=多头，负数=空头）
    Price price;              // 下单价格（厘为单位）
    Quantity volume;          // 下单数量
    int direction;            // 方向（1=买入, 2=卖出）
    std::string orderlocalid; // 本地订单ID
    
    OrderCallback(const std::string& tm, int exch, Price a1, Price b1, 
                 int64_t delta, Price px, Quantity vol, int dir, const std::string& id)
        : time(tm), exchange(exch), ask1(a1), bid1(b1), 
          deltapos(delta), price(px), volume(vol), direction(dir), orderlocalid(id) {}
};

// 策略持仓管理器
class StrategyPositionManager {
public:

    void setInitPosition(const std::string& symbol, int64_t quantity) {
        positions_[symbol] = quantity;
    }

    // 更新持仓：买入为正，卖出为负
    void updatePosition(const std::string& symbol, int64_t quantity_change) {
        positions_[symbol] += quantity_change;
    }
    
    // 获取当前持仓
    int64_t getPosition(const std::string& symbol) const {
        auto it = positions_.find(symbol);
        return it != positions_.end() ? it->second : 0;
    }
    
    // 获取所有持仓
    const std::map<std::string, int64_t>& getAllPositions() const {
        return positions_;
    }
    
    // 清空持仓
    void clearPositions() {
        positions_.clear();
    }
    
private:
    std::map<std::string, int64_t> positions_;  // symbol -> quantity
};
    

}// namespace wangcai