// === src/con_auction_engine.cpp ===
#include "../include/con_auction_engine.hpp"
#include <algorithm>

namespace wangcai_orderbook_cpp {

// 连续竞价核心撮合函数
void ConAuctionEngine::match(std::shared_ptr<Order>& inc)
{
    bool buy = inc->direction == Direction::Buy; // 判断是买单还是卖单
    auto& opp = buy ? ob_._sell : ob_._buy;      // 对手方盘口（买单撮合卖盘，卖单撮合买盘）
    int& best = buy ? ob_._best_ask : ob_._best_bid; // 对手方最优价索引

    // 只要还有剩余未成交量且对手方有挂单
    while (inc->remaining_volume() > 0 && best != -1) {
        Price px = ob_._lower + best * ob_._tick; // 计算当前对手方最优价
        // 买单价格低于对手方最优卖价，或卖单价格高于对手方最优买价，则不能成交，退出
        if ((buy && inc->price < px) || (!buy && inc->price > px)) break;
        auto& bkt = opp[best]; // 取出对手方该价位的桶

        // 只要还有剩余未成交量且该价位有挂单
        while (inc->remaining_volume() > 0 && !bkt.orders.empty()) {
            auto oppo = bkt.orders.front(); // 取出对手方队首订单
            Quantity q = std::min(inc->remaining_volume(), oppo->remaining_volume()); // 成交量为两者剩余量较小值
            inc->traded_volume += q;   // 更新主动方已成交量
            oppo->traded_volume += q;  // 更新被动方已成交量
            ob_.bucketSub(best, !buy, q); // 桶减量（对手方）

            // 撮合回调，通知成交
            if (ob_._on_exec) {
                Execution ex(
                    buy ? inc->order_id : oppo->order_id,   // 买方订单ID
                    buy ? oppo->order_id : inc->order_id,   // 卖方订单ID
                    px, q                                   // 成交价、成交量
                );
                ob_._on_exec(ex);
            }

            // 如果对手方订单已全部成交，状态置为已成交并从队列和映射中移除
            if (oppo->remaining_volume() == 0) {
                oppo->status = OrderStatus::Filled;
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
            } else {
                // 否则置为部分成交
                oppo->status = OrderStatus::PartFilled;
            }
        }
        // 更新最优价索引（可能已被摘空）
        best = buy ? ob_._best_ask : ob_._best_bid;
    }
    // 主动方订单状态更新
    if (inc->remaining_volume() == 0)
        inc->status = OrderStatus::Filled;
    else if (inc->traded_volume)
        inc->status = OrderStatus::PartFilled;
}

// 连续竞价订单接收函数
void ConAuctionEngine::accept(std::shared_ptr<Order> od)
{
    bool buy = od->direction == Direction::Buy; // 判断买卖方向
    int idx = ob_.pxToIdx(od->price);           // 价格转桶索引
    ob_._omap[od->order_id] = od;               // 系统订单ID映射

    // 市价单直接撮合，剩余未成交部分直接撤销
    if (od->order_type == OrderType::Market) {
        match(od);
        if (od->remaining_volume()) od->status = OrderStatus::Cancelled;
        return;
    }
    // 限价单先撮合
    match(od);
    if (od->remaining_volume() == 0) return; // 全部成交则不挂入盘口

    // 剩余部分挂入盘口
    auto& side = buy ? ob_._buy : ob_._sell;
    side[idx].orders.push_back(od); // 加入对应价位队列
    od->level_iter = std::prev(side[idx].orders.end()); // 保存队列迭代器
    ob_.bucketAdd(idx, buy, od->remaining_volume());    // 桶加量
    ob_._loc[od->order_id] = { buy, idx, od->level_iter }; // 位置映射

    // 如果订单的本地ID是数字，建立输入ID到系统ID的映射
    try {
        uint64_t input_id = std::stoull(od->order_local_id);
        _input_id_to_system_id[input_id] = od->order_id;
    } catch (const std::exception&) {
        // 如果本地ID不是数字，忽略
    }
}

// 撤单（通过系统订单ID）
bool ConAuctionEngine::cancel(uint64_t oid)
{
    auto it = ob_._loc.find(oid);
    if (it == ob_._loc.end()) {
        // 订单不存在，撤单失败
        if (on_cancel_) on_cancel_(oid, false, "订单不存在");
        return false;
    }

    auto loc = it->second; // 订单位置
    auto& side = loc.is_buy ? ob_._buy : ob_._sell;
    auto ord = *loc.it;

    // 已撤销或已成交的订单不能重复撤单
    if (ord->status == OrderStatus::Cancelled || ord->status == OrderStatus::Filled) {
        std::string reason = (ord->status == OrderStatus::Cancelled) ? "订单已撤销" : "订单已成交";
        if (on_cancel_) on_cancel_(oid, false, reason);
        return false;
    }

    Quantity rem = ord->remaining_volume(); // 剩余未成交量
    side[loc.idx].orders.erase(loc.it);     // 从队列中移除
    ob_.bucketSub(loc.idx, loc.is_buy, rem); // 桶减量
    ord->status = OrderStatus::Cancelled;   // 状态置为已撤销
    ob_._loc.erase(it);                     // 位置映射移除
    ob_._omap.erase(oid);                   // 系统ID映射移除

    // 撤单成功回调
    if (on_cancel_) on_cancel_(oid, true, "撤单成功");

    return true;
}

// 撤单（通过输入订单ID）
bool ConAuctionEngine::cancel_by_input_id(uint64_t input_id)
{
    auto it = _input_id_to_system_id.find(input_id);
    if (it != _input_id_to_system_id.end()) {
        // 找到对应的系统订单ID，调用标准撤单方法
        bool result = cancel(it->second);
        // 从映射中移除
        _input_id_to_system_id.erase(it);
        return result;
    } else {
        // 输入订单ID不存在
        if (on_cancel_) on_cancel_(input_id, false, "输入订单ID不存在");
        return false;
    }
}

} // namespace wangcai_orderbook_cpp 