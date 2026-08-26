// include/close_auction_engine.hpp
#pragma once
#include "orderbook.h"
#include "FenwickTree.hpp"
#include <functional>
#include <unordered_map>

namespace wangcai {

// 集合竞价撮合引擎类，负责集合竞价阶段的订单管理与撮合
class CloseAuctionEngine {
public:
    // 价格/成交量回调类型，用于实时发布预测价和成交量
    using PxCallback     = std::function<void(Price, Quantity)>;
    // 撤单回调类型，参数为订单ID、是否成功、原因、订单指针
    using CancelCallback = std::function<void(uint64_t, bool, const std::string&, std::shared_ptr<Order>)>;

    // 构造函数
    // ob         : 订单簿引用
    // prev_close : 前收盘价
    // exch       : 交易所标识（"SZ"或"SH"）
    // px_cb      : 预测价回调（可选）
    // cancel_cb  : 撤单回调（可选）
    CloseAuctionEngine(OrderBook& ob,
                       Price prev_close,
                       std::string_view exch,
                       PxCallback px_cb = nullptr,
                       CancelCallback cancel_cb = nullptr);

    // 接收新订单，加入集合竞价队列
    // od: 订单指针
    void accept(std::shared_ptr<Order> od);

    // 撤单（通过系统订单ID）
    // oid: 系统订单ID
    void cancel(uint64_t oid);

    // 撤单（通过原始输入ID）
    // input_id: 原始订单输入ID
    void cancel_by_input_id(uint64_t input_id, int channel_no = -1);

    // 集合竞价结算与撮合（如09:25/14:57）
    void settle();  // 15:30

    // 获取预测成交价
    Price    getPredictPrice()  const { return _predict_px; }
    // 获取预测成交量
    Quantity getPredictVolume() const { return _predict_vol; }

    // 可选：为了把14:57时盘口剩余单纳入Fenwick统计，提供一个bootstrap
    void bootstrap_from_orderbook();

private:
    // 树状数组增量操作，统计买卖盘挂单量
    // idx: 价格桶索引
    // buy: true为买盘，false为卖盘
    // d  : 增量（可为负，撤单时用）
    void  fenwickAdd(int idx,bool buy,int64_t d);

    // 计算深市集合竞价预测价
    Price calcPredict_SZ();

    // 计算沪市集合竞价预测价
    Price calcPredict_SH();

    // 实时发布预测价
    void  publish();

    // 应用集合竞价撮合成交
    // idx: 成交价对应的价格桶索引
    // buy_tot: 总买量
    // sell_tot: 总卖量
    void  applyAuctionTrade(int idx, uint64_t buy_tot, uint64_t sell_tot);

    // 订单簿引用
    OrderBook&  ob_;
    // 预测价回调
    PxCallback  on_px_;
    // 撤单回调
    CancelCallback on_cancel_;

    // 买盘树状数组
    Fenwick _bit_buy;
    // 卖盘树状数组
    Fenwick _bit_sell;
    // 买盘累计量
    int64_t _tot_buy{0};
    // 卖盘累计量
    int64_t _tot_sell{0};

    // 前收盘价
    Price   _prev_close{};
    // 交易所标识
    std::string _exch;

    // 预测成交价
    Price    _predict_px{0};
    // 预测成交量
    Quantity _predict_vol{0};
    // 实际成交价
    Price    _real_px{0};
};

} // namespace wangcai
