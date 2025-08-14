// === include/con_auction_engine.hpp ===
/*
 * @brief : 09:30 连续竞价引擎
 */
#pragma once
#include "orderbook.h"
#include <functional>
#include <unordered_map>
#include "OrderLoader.h"

namespace wangcai {

// 市场类型枚举
enum class MarketType {
    SH,  // 上海市场
    SZ   // 深圳市场
};

class ConAuctionEngine {
public:
        using CancelCallback = std::function<void(uint64_t order_id, bool success, const std::string& reason, 
                                            std::shared_ptr<Order> order_info)>;
    
    explicit ConAuctionEngine(OrderBook& ob, MarketType market_type, CancelCallback cancel_cb = nullptr)
        : ob_(ob), market_type_(market_type), on_cancel_(cancel_cb) {}
    
    void accept(std::shared_ptr<Order>);
    bool cancel(uint64_t oid);
    bool cancel_by_input_id(uint64_t input_id);  // 通过输入订单ID撤单

private:
    void match(std::shared_ptr<Order>&);
    void match_sh(std::shared_ptr<Order>&);  // 上海市场撮合逻辑
    void match_sz(std::shared_ptr<Order>&);  // 深圳市场撮合逻辑
    void accept_sh(std::shared_ptr<Order> od);  // 上海市场订单处理
    void accept_sz(std::shared_ptr<Order> od);  // 深圳市场订单处理
    OrderBook& ob_;
    MarketType market_type_;  // 市场类型
    CancelCallback on_cancel_;
    
};

} // namespace wangcai 