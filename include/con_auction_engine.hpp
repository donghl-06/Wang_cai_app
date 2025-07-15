// === include/con_auction_engine.hpp ===
/*
 * @brief : 09:30 连续竞价引擎
 */
#pragma once
#include "orderbook.h"
#include <functional>
#include <unordered_map>

namespace wangcai_orderbook_cpp {

class ConAuctionEngine {
public:
    using CancelCallback = std::function<void(uint64_t order_id, bool success, const std::string& reason)>;
    
    explicit ConAuctionEngine(OrderBook& ob, CancelCallback cancel_cb = nullptr)
        : ob_(ob), on_cancel_(cancel_cb) {}
    
    void accept(std::shared_ptr<Order>);
    bool cancel(uint64_t oid);
    bool cancel_by_input_id(uint64_t input_id);  // 通过输入订单ID撤单

private:
    void match(std::shared_ptr<Order>&);
    OrderBook& ob_;
    CancelCallback on_cancel_;
    
    // 原始输入订单ID到系统订单ID的映射
    std::unordered_map<uint64_t, uint64_t> _input_id_to_system_id;
};

} // namespace wangcai_orderbook_cpp 