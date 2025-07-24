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

namespace wangcai_orderbook_cpp {

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
    void cancel_by_input_id(uint64_t input_id);  // 通过输入订单ID撤单
    void settle();                       // 09:25

    // 获取预测结果的公共接口
    Price getPredictPrice() const { return _predict_px; }
    Quantity getPredictVolume() const { return _predict_vol; }
    
    // 调试接口
    int64_t getTotalBuy() const { return _tot_buy; }
    int64_t getTotalSell() const { return _tot_sell; }
    void print_orderbook_to_csv(const std::string& filename);

    Price getRealPrice() const { return _real_px; }
private:
    /* Fenwick helpers */
    void  fenwickAdd(int idx,bool buy,int64_t d);
    Price calcPredict_SZ();                 // 实时预测深交所
    Price calcPredict_SH();                 // 实时预测上交所
    void  publish();

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

    Price    _predict_px{0};
    Quantity _predict_vol{0};
    Price    _real_px{0};
};

} // namespace wangcai_orderbook_cpp 