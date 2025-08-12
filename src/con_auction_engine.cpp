// === src/con_auction_engine.cpp ===
#include "../include/con_auction_engine.hpp"
#include <algorithm>
#include <iostream>

namespace wangcai_orderbook_cpp {


// 连续竞价订单接收函数
void ConAuctionEngine::accept(std::shared_ptr<Order> od)
{
    if (market_type_ == MarketType::SH) {
        accept_sh(od);
    } else {
        accept_sz(od);
    }
}

// 上海市场订单处理
void ConAuctionEngine::accept_sh(std::shared_ptr<Order> od)
{
    bool buy = od->direction == Direction::Buy;
    int idx = ob_.pxToIdx(od->price);
    ob_._omap[od->order_id] = od;
    
    // 限价单先撮合
    match(od);
    if (od->remaining_volume() == 0) return;

    // 剩余部分挂入盘口
    auto& side = buy ? ob_._buy : ob_._sell;
    side[idx].orders.push_back(od);
    od->level_iter = std::prev(side[idx].orders.end());
    
    // 只有历史订单才更新vol_sum统计
    if (od->broker == "BRK") {
        ob_.bucketAdd(idx, buy, od->remaining_volume());
    }
    
    ob_._loc[od->order_id] = { buy, idx, od->level_iter };


}

// 深圳市场订单处理
void ConAuctionEngine::accept_sz(std::shared_ptr<Order> od)
{
    bool buy = od->direction == Direction::Buy;

    //深市：市价 / 本方最优 保护价转换 
    if (od->broker == "BRK" &&
        (od->order_type == OrderType::Market || od->order_type == OrderType::BestOwn)) {
        
        uint64_t ext_id = std::stoull(od->order_local_id);
        OrderType orig = od->order_type;
        Price px = 0;
        if (orig == OrderType::Market) {
            // 先看历史成交价格
            if (auto it = ob_.first_trade_px_.find(ext_id); it != ob_.first_trade_px_.end())
                px = it->second;
            // 无成交 → 对手最优
            if (px == 0) px = buy ? ob_.bestAsk() : ob_.bestBid();
            // 市场空簿 → 涨跌停兜底
            if (px == 0) px = buy ? ob_._upper : ob_._lower;
        }
        else if (orig == OrderType::BestOwn) {
            // 己方最优
            px = buy ? ob_.bestBid() : ob_.bestAsk();
            // 己方空簿 → 对手最优
            if (px == 0) px = buy ? ob_.bestAsk() : ob_.bestBid();
            if (px == 0) px = buy ? ob_._upper : ob_._lower;
        }
        if(od->order_local_id == "1282779"){
            std::cout<<"px"<<px<<std::endl;
        }
        // tick 对齐
        if (px < ob_._lower) px = ob_._lower;
        if (px > ob_._upper) px = ob_._upper;
        Price off = (px - ob_._lower) % ob_._tick;
        px -= off;   // 向下对齐

        od->price      = px;
        od->order_type = OrderType::Limit;
    }

    // 撮合
    match(od);
    if (od->remaining_volume() == 0) return;

    // 剩余挂簿
    int idx = ob_.pxToIdx(od->price);  // ← 现在才算 idx，确保用最终价
    auto& side = buy ? ob_._buy : ob_._sell;
    side[idx].orders.push_back(od);
    od->level_iter = std::prev(side[idx].orders.end());
    
    // 只有历史订单才更新vol_sum统计
    if (od->broker == "BRK") {
        ob_.bucketAdd(idx, buy, od->remaining_volume());
    }
    
    ob_._loc[od->order_id] = { buy, idx, od->level_iter }; // 记录订单位置
}

// 连续竞价核心撮合函数
void ConAuctionEngine::match(std::shared_ptr<Order>& inc)
{
    // 根据市场类型调用对应的撮合逻辑
    if (market_type_ == MarketType::SH) {
        match_sh(inc);
    } else {
        match_sz(inc);
    }
}

// 上海市场撮合逻辑
void ConAuctionEngine::match_sh(std::shared_ptr<Order>& inc)
{
    bool buy = inc->direction == Direction::Buy;
    auto& opp = buy ? ob_._sell : ob_._buy;
    int& best = buy ? ob_._best_ask : ob_._best_bid;

    // 撮合规则：价格优先、时间优先
    while (inc->remaining_volume() > 0 && best != -1) {
        // 计算当前最优对手价
        Price px = ob_._lower + best * ob_._tick;
        // 买单价格低于对手价/卖单价格高于对手价则无法成交，退出
        if ((buy && inc->price < px) || (!buy && inc->price > px)) break;
        
        auto& bkt = opp[best];

        // 遍历该价位下的所有对手方订单（时间优先）
        while (inc->remaining_volume() > 0 && !bkt.orders.empty()) {
            auto oppo = bkt.orders.front();
            
            // 检查对手订单是否有效
            if (oppo->remaining_volume() == 0) {
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
                continue;
            }

            // 虚拟订单处理：如果一方是USER订单，另一方是历史订单
            bool inc_is_user = (inc->broker != "BRK");
            bool oppo_is_user = (oppo->broker != "BRK");
            
            if (inc_is_user ^ oppo_is_user) {
                // 只有一方是USER订单的情况 - 进行虚拟成交
                auto virt = inc_is_user ? inc : oppo;
                
                Quantity q = virt->remaining_volume();
                Price fill_price = px;  // 使用当前撮合价格
                
                // 虚拟成交：只影响USER订单，不影响历史订单
                virt->traded_volume = virt->volume;
                virt->status = OrderStatus::Filled;
                
                // 虚拟成交回调：使用正常的订单ID，BacktestEngine通过映射识别
                if (ob_._on_exec) {
                    Execution ex(
                        buy ? inc->order_id : oppo->order_id,
                        buy ? oppo->order_id : inc->order_id,
                        fill_price, q);
                    ob_._on_exec(ex);  // BacktestEngine通过映射识别虚拟成交
                }
                
                // 从订单簿移除虚拟订单，但不影响历史订单
                if (inc_is_user) {
                    // inc是虚拟订单，移除虚拟订单但历史订单继续撮合
                    std::cout << "[虚拟成交-SH] USER订单 " << inc->order_local_id 
                              << " 与历史订单虚拟成交 " << q << "@" << fill_price / 10000.0 << std::endl;
                    return;  // 虚拟订单处理完毕，历史订单继续正常撮合
                } else {
                    // oppo是虚拟订单，移除虚拟订单，继续与下一个对手撮合
                    bkt.orders.pop_front();
                    ob_._loc.erase(oppo->order_id);
                    ob_._omap.erase(oppo->order_id);
                    std::cout << "[虚拟成交-SH] USER订单 " << oppo->order_local_id 
                              << " 与历史订单虚拟成交 " << q << "@" << fill_price / 10000.0 << std::endl;
                    continue;  // 继续处理下一个对手订单（历史订单继续撮合）
                }
            }

            // 正常撮合逻辑（双方都是历史订单）
            Quantity q = std::min(inc->remaining_volume(), oppo->remaining_volume());
            if (q == 0) break;

            // 执行成交
            inc->traded_volume += q;
            oppo->traded_volume += q;
            ob_.bucketSub(best, !buy, q);

            // 成交回调
            if (ob_._on_exec) {
                Execution ex(
                    ob_.sys2input_.count(buy ? inc->order_id : oppo->order_id) ? 
                        ob_.sys2input_[buy ? inc->order_id : oppo->order_id] : 
                        (buy ? inc->order_id : oppo->order_id),
                    ob_.sys2input_.count(buy ? oppo->order_id : inc->order_id) ? 
                        ob_.sys2input_[buy ? oppo->order_id : inc->order_id] : 
                        (buy ? oppo->order_id : inc->order_id),
                    px, q);
                ob_._on_exec(ex);
            }

            // 对手方订单完全成交，移出订单簿
            if (oppo->remaining_volume() == 0) {
                oppo->status = OrderStatus::Filled;
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
            } else {
                oppo->status = OrderStatus::PartFilled;
            }
        }
        
        // bucketSub会自动更新best，不需要手动重新赋值
        // 但需要重新获取当前最优价
        best = buy ? ob_._best_ask : ob_._best_bid;
    }

    // 更新本方订单状态
    if (inc->remaining_volume() == 0)
        inc->status = OrderStatus::Filled;
    else if (inc->traded_volume > 0)
        inc->status = OrderStatus::PartFilled;
}

// 深圳市场撮合逻辑
void ConAuctionEngine::match_sz(std::shared_ptr<Order>& inc)
{
    bool buy = inc->direction == Direction::Buy;
    auto& opp = buy ? ob_._sell : ob_._buy;
    int& best = buy ? ob_._best_ask : ob_._best_bid;

    // 撮合规则：价格优先、时间优先
    while (inc->remaining_volume() > 0 && best != -1) {
        // 计算当前最优对手价
        Price px = ob_._lower + best * ob_._tick;
        // 买单价格低于对手价/卖单价格高于对手价则无法成交，退出
        if ((buy && inc->price < px) || (!buy && inc->price > px)) break;
        
        auto& bkt = opp[best];

        // 遍历该价位下的所有对手方订单（时间优先）
        while (inc->remaining_volume() > 0 && !bkt.orders.empty()) {
            auto oppo = bkt.orders.front();
            
            // 检查对手订单是否有效
            if (oppo->remaining_volume() == 0) {
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
                continue;
            }

            // 虚拟订单处理：如果一方是USER订单，另一方是历史订单
            bool inc_is_user = (inc->broker != "BRK");
            bool oppo_is_user = (oppo->broker != "BRK");
            
            if (inc_is_user ^ oppo_is_user) {
                // 只有一方是USER订单的情况 - 进行虚拟成交
                auto virt = inc_is_user ? inc : oppo;
                
                Quantity q = virt->remaining_volume();
                Price fill_price = px;  // 使用当前撮合价格
                
                // 虚拟成交：只影响USER订单，不影响历史订单
                virt->traded_volume = virt->volume;
                virt->status = OrderStatus::Filled;
                
                // 虚拟成交回调：使用正常的订单ID，BacktestEngine通过映射识别
                if (ob_._on_exec) {
                    Execution ex(
                        buy ? inc->order_id : oppo->order_id,
                        buy ? oppo->order_id : inc->order_id,
                        fill_price, q);
                    ob_._on_exec(ex);  // BacktestEngine通过映射识别虚拟成交
                }
                
                // 从订单簿移除虚拟订单，但不影响历史订单
                if (inc_is_user) {
                    // inc是虚拟订单，移除虚拟订单但历史订单继续撮合
                    std::cout << "[虚拟成交-SZ] USER订单 " << inc->order_local_id 
                              << " 与历史订单虚拟成交 " << q << "@" << fill_price / 10000.0 << std::endl;
                    return;  // 虚拟订单处理完毕，历史订单继续正常撮合
                } else {
                    // oppo是虚拟订单，移除虚拟订单，继续与下一个对手撮合
                    bkt.orders.pop_front();
                    ob_._loc.erase(oppo->order_id);
                    ob_._omap.erase(oppo->order_id);
                    std::cout << "[虚拟成交-SZ] USER订单 " << oppo->order_local_id 
                              << " 与历史订单虚拟成交 " << q << "@" << fill_price / 10000.0 << std::endl;
                    continue;  // 继续处理下一个对手订单（历史订单继续撮合）
                }
            }

            // 正常撮合逻辑（双方都是历史订单）
            Quantity q = std::min(inc->remaining_volume(), oppo->remaining_volume());
            if (q == 0) break;

            // 执行成交
            inc->traded_volume += q;
            oppo->traded_volume += q;
            ob_.bucketSub(best, !buy, q);

            // 成交回调
            if (ob_._on_exec) {
                // 安全获取输入订单ID
                uint64_t buy_input_id = buy ? inc->order_id : oppo->order_id;
                uint64_t sell_input_id = buy ? oppo->order_id : inc->order_id;
                
                auto buy_it = ob_.sys2input_.find(buy_input_id);
                if (buy_it != ob_.sys2input_.end()) {
                    buy_input_id = buy_it->second;
                }
                
                auto sell_it = ob_.sys2input_.find(sell_input_id);
                if (sell_it != ob_.sys2input_.end()) {
                    sell_input_id = sell_it->second;
                }
                
                Execution ex(buy_input_id, sell_input_id, px, q);
                ob_._on_exec(ex);
            }

            // 对手方订单完全成交，移出订单簿
            if (oppo->remaining_volume() == 0) {
                oppo->status = OrderStatus::Filled;
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
            } else {
                oppo->status = OrderStatus::PartFilled;
            }
        }
        
        // bucketSub会自动更新best，重新获取当前最优价
        best = buy ? ob_._best_ask : ob_._best_bid;
    }

    // 更新本方订单状态
    if (inc->remaining_volume() == 0)
        inc->status = OrderStatus::Filled;
    else if (inc->traded_volume > 0)
        inc->status = OrderStatus::PartFilled;
}

// 撤单（通过系统订单ID）
bool ConAuctionEngine::cancel(uint64_t oid)
{
    auto it = ob_._loc.find(oid);
    if (it == ob_._loc.end()) {
        // 订单不存在，撤单失败
        if (on_cancel_) on_cancel_(oid, false, "订单不存在", nullptr);
        // std::cout << "撤单失败：订单不存在" << std::endl;
        return false;
    }

    auto loc = it->second; // 订单位置
    auto& side = loc.is_buy ? ob_._buy : ob_._sell;
    auto ord = *loc.it;

    // 已撤销或已成交的订单不能重复撤单
    if (ord->status == OrderStatus::Cancelled || ord->status == OrderStatus::Filled) {
        std::string reason = (ord->status == OrderStatus::Cancelled) ? "订单已撤销" : "订单已成交";
        if (on_cancel_) on_cancel_(oid, false, reason, nullptr);
        // std::cout << "撤单失败：" << reason << std::endl;
        return false;
    }

    // 在实际撤单前先记录撤单信息（现在还能获取到订单信息）
    // 但我们需要通过回调通知外部记录撤单，因为这里没有直接访问BacktestEngine的方法
    
    Quantity rem = ord->remaining_volume(); // 剩余未成交量
    side[loc.idx].orders.erase(loc.it);     // 从队列中移除
    ob_.bucketSub(loc.idx, loc.is_buy, rem); // 桶减量
    ord->status = OrderStatus::Cancelled;   // 状态置为已撤销
    ob_._loc.erase(it);                     // 位置映射移除
    ob_._omap.erase(oid);                   // 系统ID映射移除

    // 撤单成功回调，传递订单信息
    if (on_cancel_) on_cancel_(oid, true, "撤单成功", ord);
    // std::cout << "撤单成功" << std::endl;

    return true;
}

// 撤单（通过输入订单ID）
bool ConAuctionEngine::cancel_by_input_id(uint64_t input_id)
{
    auto it = ob_.input2sys_.find(input_id);          // 查共享表
    if (it != ob_.input2sys_.end()) {
        // 找到对应的系统订单ID，调用标准撤单方法
        bool result = cancel(it->second);
        // 从映射中移除
        ob_.input2sys_.erase(it);                     // 从共享表删
        ob_.sys2input_.erase(it->second);             // 从共享表删
        return result;
    } else {
        // 输入订单ID不存在
        if (on_cancel_) on_cancel_(input_id, false, "输入订单ID不存在", nullptr);
        // std::cout << "撤单失败：输入订单ID不存在" << std::endl;
        return false;
    }
}

} // namespace wangcai_orderbook_cpp 