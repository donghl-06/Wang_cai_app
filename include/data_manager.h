/*
 * @Author: chenlisen
 * @Date: 2025-08-13 08:43:25
 * @LastEditTime: 2025-08-14 05:57:00
 * @FilePath: /wangcai_cpp/include/data_manager.h
 */
#pragma once
#include "types.h"
#include "market_info.h"
#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"


namespace wangcai_orderbook_cpp {

class DataManager {
public:
    explicit DataManager(OrderBook* ob, CallAuctionEngine* call_engine, 
                        ConAuctionEngine* con_engine, CloseAuctionEngine* close_engine);

    void updateSnapshot(const Event& ev);
    void updateOrderDetail(const Event& ev, char orderKind);
    Snapshot getSnapshot() const;
    OrderDetail getOrderDetail() const;
private:
    Snapshot snap_data; // 快照数据
    OrderDetail order_detail; // 委托明细数据

    OrderBook* orderbook_ = nullptr;
    CallAuctionEngine* call_engine_ = nullptr;
    ConAuctionEngine* con_engine_ = nullptr;
    CloseAuctionEngine* close_engine_ = nullptr;
};

} // namespace wangcai