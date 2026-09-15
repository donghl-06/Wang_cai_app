// === include/call_auction_engine.hpp ===
/*
 * @brief : 09:15-09:25 集合竞价引擎（实时预测价）
 */
#pragma once
#include "orderbook.h"
#include "FenwickTree.hpp"
#include <functional>
#include <unordered_map>
#include "OrderLoader.h"

namespace wangcai {

class CallAuctionEngine {
public:
    using PxCallback = std::function<void(Price, Quantity)>;
    using CancelCallback = std::function<void(uint64_t order_id, bool success, const std::string& reason, 
                                            std::shared_ptr<Order> order_info)>;

    CallAuctionEngine(OrderBook& ob,
                      Price      prev_close,
                      std::string_view exch,   // "SH"/"SZ"
                      PxCallback px_cb = nullptr,
                      CancelCallback cancel_cb = nullptr);

    void accept(std::shared_ptr<Order>);
    void cancel(uint64_t oid);
    void cancel_by_input_id(uint64_t input_id, int channel_no = -1);  // 通过市场复合键撤单
    void settle();                       // 09:25

    // 获取预测结果的公共接口（惰性重算：accept/cancel 只置脏标记，
    // 读取时才计算；on_px_ 回调从未接线，原逐事件 publish 是纯浪费）
    Price getPredictPrice() const { ensurePredict(); return _predict_px; }
    Quantity getPredictVolume() const { ensurePredict(); return _predict_vol; }
    
    // 调试接口
    int64_t getTotalBuy() const { return _tot_buy; }
    int64_t getTotalSell() const { return _tot_sell; }
    Price getRealPrice() const { return _real_px; }
private:
    /* Fenwick helpers */
    void  fenwickAdd(int idx,bool buy,int64_t d);
    Price calcPredict_SZ() const;             // 实时预测深交所
    Price calcPredict_SH() const;             // 实时预测上交所
    void  publish();
    void  ensurePredict() const;              // 脏标记置位时重算预测价/量

    /* 批量结算 */
    void applyAuctionTrade(int auction_idx,
                           uint64_t buy_tot, uint64_t sell_tot);

    OrderBook&  ob_; // 订单簿
    PxCallback  on_px_; // 预测价格回调
    CancelCallback on_cancel_; // 撤单回调

    Fenwick _bit_buy, _bit_sell;
    int64_t _tot_buy{0}, _tot_sell{0};

    Price   _prev_close{};
    std::string _exch;

    mutable Price    _predict_px{0};
    mutable Quantity _predict_vol{0};
    mutable bool     _predict_dirty{true};
    // calcPredict_SH 复用缓冲,避免每次计算堆分配两个 N+1 数组
    mutable std::vector<uint64_t> _sh_buy_cumu, _sh_sell_cumu;
    mutable std::vector<Price>    _sh_tradable_prices;
    Price    _real_px{0};
};

} // namespace wangcai
