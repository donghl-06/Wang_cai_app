/*
 * @Author: chenlisen
 * @Date: 2025-08-13 08:47:50
 * @LastEditTime: 2025-08-14 05:57:10
 * @FilePath: /wangcai_cpp/src/data_manager.cpp
 */

#include "data_manager.h"
#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"
#include <string>
#include <algorithm>
#include <iostream>

namespace wangcai_orderbook_cpp {

DataManager::DataManager(OrderBook* ob, CallAuctionEngine* call_engine, 
                        ConAuctionEngine* con_engine, CloseAuctionEngine* close_engine)
    : orderbook_(ob),
      call_engine_(call_engine),
      con_engine_(con_engine),
      close_engine_(close_engine) {}


void DataManager::updateSnapshot(const Event& ev) {
    assert(ev.source == "tick");
    snap_data.Exchange = ev.is_SZ ? 0: 1;
    snap_data.Instrument = ev.sym;  
    snap_data.datetime = ev.datetime;
    snap_data.PreClose = ev.prevclose;
    snap_data.Open = ev.open;
    snap_data.High = ev.high;
    snap_data.Low = ev.low;
    snap_data.Close = ev.close;
    snap_data.last_price = orderbook_ -> getLastTradePrice();
    snap_data.Volume = ev.volume;
    snap_data.Turnover = ev.turnover;
    snap_data.TradingDay = 0; // TODO: Set actual trading day
    snap_data.ActionDay = 0; // TODO: Set actual action day

    snap_data.bids = ev.bids_;
    snap_data.asks = ev.asks_;
    snap_data.bid_sizes = ev.bid_sizes_;
    snap_data.ask_sizes = ev.ask_sizes_;
    snap_data.NumTrades = ev.tradecount;
    snap_data.UpperLimit = orderbook_->_upper;
    snap_data.LowerLimit = orderbook_->_lower;
    snap_data.TotalAskVol = ev.totalasize;
    snap_data.TotalBidVol = ev.totalbsize;
    snap_data.Iopv = ev.iopv;

}

void DataManager::updateOrderDetail(const Event& ev, char orderKind) {
    assert(ev.source == "ord");
    order_detail.Exchange = ev.is_SZ ? 0 : 1;
    order_detail.Instrument = ev.sym;
    order_detail.ChannelNo = ev.channelno;
    order_detail.Price = ev.price;
    order_detail.Volume = ev.size;
    order_detail.Side = ev.side;
    order_detail.OrderKind = orderKind;
    order_detail.SeqNo = ev.seqno;
    order_detail.BizIndex = ev.bizindex;
    order_detail.OrderNo = ev.orderid;
}


Snapshot DataManager::getSnapshot() const {
    return snap_data;
}
OrderDetail DataManager::getOrderDetail() const {
    return order_detail;
}

} // namespace wangcai