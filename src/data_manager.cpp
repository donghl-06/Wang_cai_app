/*
 * @Author: chenlisen
 * @Date: 2025-08-13 08:47:50
 * @LastEditTime: 2025-08-15 06:57:06
 * @FilePath: /workspace/wangcai_cpp/src/data_manager.cpp
 */

#include "data_manager.h"
#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"
#include <string>
#include <algorithm>
#include <iostream>

namespace wangcai {

DataManager::DataManager(OrderBook* ob, CallAuctionEngine* call_engine, 
                        ConAuctionEngine* con_engine, CloseAuctionEngine* close_engine)
    : orderbook_(ob),
      call_engine_(call_engine),
      con_engine_(con_engine),
      close_engine_(close_engine) {}


void DataManager::updateSnapshot(const Event& ev) {
    assert(ev.source == "tick");
    snap_data.Exchange = ev.exchange != -1 ? ev.exchange : (ev.isSZ() ? 1 : 0);
    snap_data.Instrument = ev.sym;  
    snap_data.datetime = ""; // 不再输出 datetime 字段
    snap_data.Status = ev.status.empty() ? "0" : ev.status;
    snap_data.PreClose = ev.prevclose;
    snap_data.Open = ev.open;
    snap_data.High = ev.high;
    snap_data.Low = ev.low;
    snap_data.Close = ev.close;
    snap_data.last_price = orderbook_ -> getLastTradePrice();
    snap_data.Volume = ev.volume;
    snap_data.Turnover = ev.turnover;

    // ========= 设定 TradingDay / ActionDay =========
    int tday = 0;
    if (ev.datetime.size() >= 10) {
        std::string date_part = ev.datetime.substr(0, 10); // YYYY-MM-DD
        date_part.erase(std::remove(date_part.begin(), date_part.end(), '-'), date_part.end()); // YYYYMMDD
        tday = std::stoi(date_part);
    }
    snap_data.TradingDay = tday;
    snap_data.ActionDay = tday; // 默认与交易日一致

    // ============= 设置 Time 字段 =============
    if (ev.time_raw != -1) {
        snap_data.Time = ev.time_raw;
    } else {
        // 回退：从 datetime 提取 HHMMSS 并补 000
        int t_raw = 0;
        if (ev.datetime.length() >= 19) {
            std::string time_part = ev.datetime.substr(11, 5); // HH:MM
            time_part.erase(std::remove(time_part.begin(), time_part.end(), ':'), time_part.end()); // HHMM
            t_raw = std::stoi(time_part + "00000");
        }
        snap_data.Time = t_raw;
    }

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
    order_detail.Exchange = ev.exchange != -1 ? ev.exchange : (ev.isSZ() ? 1 : 0);
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
