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

    // 深市无涨跌幅日(新股前 5 日)开盘集合竞价 900% 有效竞价范围:
    // 买申报价 > 前收盘(首日=发行价)×900% 的暂存于交易主机、不参加开盘
    // 集合竞价,结转连续竞价(300869.SZ 2020-08-24 实证:发行价 10.16,
    // 91.44=10.16×9 以上的 100/100.01/101 买单全部缺席真实竞价但存活
    // 至开盘后撤单;卖方无上限——真实竞价 1.0 卖单正常成交)。
    void setSzNoLimitIpo(bool v) { sz_nolimit_ipo_ = v; }
    // 取出被 900% 规则暂存的买单(原申报序),由 BacktestEngine 在连续
    // 竞价开始时结转 con_engine(走既有笼子/暂存机制决定激活时机)
    std::vector<std::shared_ptr<Order>> takeDeferredIpoBuys() {
        return std::move(deferred_ipo_buys_);
    }

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

    // 深市无涨跌幅日 900% 暂存(见 setSzNoLimitIpo)
    bool sz_nolimit_ipo_ = false;
    std::vector<std::shared_ptr<Order>> deferred_ipo_buys_;
};

} // namespace wangcai
