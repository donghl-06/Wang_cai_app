// === src/con_auction_engine.cpp ===
#include "../include/con_auction_engine.hpp"
#include <algorithm>
#include <iostream>

namespace wangcai {

// ============== 虚拟订单处理函数实现 ==============

// 虚拟订单成交（counterparty_id是对手方历史订单ID）
void ConAuctionEngine::virtual_fill(std::shared_ptr<Order>& od, Price fill_price, uint64_t counterparty_id)
{
    od->traded_volume = od->volume;
    od->status = OrderStatus::Filled;
    
    if (ob_._on_exec) {
        bool buy = (od->direction == Direction::Buy);
        // 对手方ID转换为input_id（如果有映射的话）
        auto cp_it = ob_.sys2input_.find(counterparty_id);
        uint64_t cp_input_id = (cp_it != ob_.sys2input_.end()) ? cp_it->second : counterparty_id;
        
        Execution ex(
            buy ? od->order_id : cp_input_id,   // 买方ID
            buy ? cp_input_id : od->order_id,   // 卖方ID
            fill_price,
            od->volume
        );
        ob_._on_exec(ex);
    }
}

// 虚拟订单入队
void ConAuctionEngine::accept_virtual_order(std::shared_ptr<Order> od)
{
    bool buy = (od->direction == Direction::Buy);
    
    // ========== 第一步：检查是否立即成交 ==========
    int& best_opp = buy ? ob_._best_ask : ob_._best_bid;
    if (best_opp != -1) {
        Price best_opp_price = ob_._lower + best_opp * ob_._tick;
        bool can_trade = (buy && best_opp_price <= od->price) || 
                         (!buy && best_opp_price >= od->price);
        if (can_trade) {
            // 找到对手方第一个历史订单作为counterparty
            auto& opp_side = buy ? ob_._sell : ob_._buy;
            auto& opp_bkt = opp_side[best_opp];
            uint64_t counterparty_id = 0;
            if (!opp_bkt.orders.empty()) {
                counterparty_id = opp_bkt.orders.front()->order_id;
            }
            
            // 立即全部成交，成交价=对手方最优价
            virtual_fill(od, best_opp_price, counterparty_id);
            ob_._omap[od->order_id] = od;  // 添加到omap以便查询
            return;
        }
    }
    
    // ========== 第二步：找同侧同价位最后一个历史订单 ==========
    int idx = ob_.pxToIdx(od->price);
    auto& same_side = buy ? ob_._buy : ob_._sell;  // 同侧订单簿
    auto& bkt = same_side[idx];
    
    uint64_t trigger_id = 0;
    bool is_tradeable = true;  // 默认：我是第一个，可成交
    
    if (!bkt.orders.empty()) {
        // 从后往前找最后一个历史订单
        for (auto rit = bkt.orders.rbegin(); rit != bkt.orders.rend(); ++rit) {
            if ((*rit)->is_historical) {
                trigger_id = (*rit)->order_id;
                is_tradeable = false;
                break;
            }
        }
    }
    
    // ========== 第三步：入队到虚拟订单簿 ==========
    auto& vmap = buy ? virtual_buy_ : virtual_sell_;
    vmap[od->price].emplace_back(VirtualOrder(od, trigger_id, is_tradeable));
    
    // 获取刚插入的迭代器
    auto it = std::prev(vmap[od->price].end());
    
    // 维护位置映射（O(1)撤单）
    virtual_loc_[od->order_id] = { od->price, buy, it };
    
    // 维护反向映射（trigger_map）
    if (trigger_id != 0) {
        trigger_map_[trigger_id].push_back(&(*it));
    }
    
    // 添加到omap以便后续查找
    ob_._omap[od->order_id] = od;
}

// 历史订单被移除时（成交或撤单），更新虚拟订单状态
void ConAuctionEngine::on_historical_order_removing(
    std::shared_ptr<Order>& removing_order, 
    int bucket_idx, 
    bool is_buy,
    bool is_filled)
{
    // 检查是否有虚拟订单以它为标签
    auto it = trigger_map_.find(removing_order->order_id);
    if (it == trigger_map_.end()) return;
    
    uint64_t prev_hist_id = 0;
    
    if (!is_filled) {
        // 【撤单】需要找前一个历史订单
        auto& bkt = (is_buy ? ob_._buy : ob_._sell)[bucket_idx];
        for (auto& ord : bkt.orders) {
            if (ord->order_id == removing_order->order_id) {
                break;
            }
            if (ord->is_historical) {
                prev_hist_id = ord->order_id;
            }
        }
    }
    // 【成交】prev_hist_id 保持为 0，虚拟订单直接可成交
    
    // 更新虚拟订单的标签
    for (auto* vod : it->second) {
        if (prev_hist_id != 0) {
            // 撤单：回退到前一个历史订单
            vod->trigger_order_id = prev_hist_id;
            vod->is_tradeable = false;
            trigger_map_[prev_hist_id].push_back(vod);
        } else {
            // 成交 或 前面没有历史订单了
            vod->trigger_order_id = 0;
            vod->is_tradeable = true;
        }
    }
    trigger_map_.erase(it);
}

// 历史订单处理完后，检查虚拟订单成交
void ConAuctionEngine::try_match_virtual_orders(std::shared_ptr<Order>& inc)
{
    // inc 是刚处理完的历史订单（主动方）
    bool buy = (inc->direction == Direction::Buy);
    auto& vopp = buy ? virtual_sell_ : virtual_buy_;  // 对手方虚拟订单
    
    for (auto vit = vopp.begin(); vit != vopp.end(); ) {
        Price px = vit->first;
        
        // 价格不匹配则跳过（买单找低价卖单，卖单找高价买单）
        if ((buy && px > inc->price) || (!buy && px < inc->price)) {
            ++vit;
            continue;
        }
        
        auto& vlist = vit->second;
        for (auto it = vlist.begin(); it != vlist.end(); ) {
            if (!it->is_tradeable) { 
                ++it; 
                continue; 
            }
            
            // 可成交！虚拟订单全部成交（不管inc有多少量）
            // 对手方ID是触发成交的历史订单inc
            virtual_fill(it->order, px, inc->order_id);
            
            // 从trigger_map移除（如果有的话）
            if (it->trigger_order_id != 0) {
                auto& vec = trigger_map_[it->trigger_order_id];
                vec.erase(std::remove(vec.begin(), vec.end(), &(*it)), vec.end());
                if (vec.empty()) trigger_map_.erase(it->trigger_order_id);
            }
            
            // 从virtual_loc_移除
            virtual_loc_.erase(it->order->order_id);
            
            // 从omap移除
            ob_._omap.erase(it->order->order_id);
            
            it = vlist.erase(it);
        }
        
        if (vlist.empty()) {
            vit = vopp.erase(vit);
        } else {
            ++vit;
        }
    }
}

// 撤销虚拟订单（O(1)复杂度）
bool ConAuctionEngine::cancel_virtual_order(uint64_t order_id)
{
    // O(1) 查找位置
    auto loc_it = virtual_loc_.find(order_id);
    if (loc_it == virtual_loc_.end()) {
        return false;  // 订单不存在
    }
    
    auto& loc = loc_it->second;
    auto& vmap = loc.is_buy ? virtual_buy_ : virtual_sell_;
    auto& vlist = vmap[loc.price];
    auto& vod = *loc.it;
    
    // 从trigger_map移除
    if (vod.trigger_order_id != 0) {
        auto& vec = trigger_map_[vod.trigger_order_id];
        vec.erase(std::remove(vec.begin(), vec.end(), &vod), vec.end());
        if (vec.empty()) trigger_map_.erase(vod.trigger_order_id);
    }
    
    // 更新状态
    vod.order->status = OrderStatus::Cancelled;
    
    // 回调
    if (on_cancel_) on_cancel_(order_id, true, "撤单成功", vod.order);
    
    // 从各映射中移除
    ob_._omap.erase(order_id);
    vlist.erase(loc.it);
    virtual_loc_.erase(loc_it);
    
    // 如果该价位为空，移除整个价位
    if (vlist.empty()) {
        vmap.erase(loc.price);
    }
    
    return true;
}

// ============== 连续竞价主函数 ==============

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
    // ========== 虚拟订单：走单独的虚拟订单簿 ==========
    if (!od->is_historical) {
        accept_virtual_order(od);
        return;
    }
    
    // ========== 历史订单：正常处理 ==========
    bool buy = od->direction == Direction::Buy;
    int idx = ob_.pxToIdx(od->price);
    ob_._omap[od->order_id] = od;
    
    // 限价单先撮合（只与历史订单撮合）
    match(od);
    
    // 撮合完后，检查虚拟订单是否可成交
    try_match_virtual_orders(od);
    
    if (od->remaining_volume() == 0) {
        return;
    }

    // 剩余部分挂入盘口
    auto& side = buy ? ob_._buy : ob_._sell;
    side[idx].orders.push_back(od);
    od->level_iter = std::prev(side[idx].orders.end());
    
    // 历史订单更新vol_sum
        ob_.bucketAdd(idx, buy, od->remaining_volume());
    
    ob_._loc[od->order_id] = { buy, idx, od->level_iter };
}

// 深圳市场订单处理
void ConAuctionEngine::accept_sz(std::shared_ptr<Order> od)
{
    // ========== 虚拟订单：走单独的虚拟订单簿 ==========
    if (!od->is_historical) {
        accept_virtual_order(od);
        return;
    }
    
    // ========== 历史订单：正常处理 ==========
    bool buy = od->direction == Direction::Buy;

    // 深市：市价 / 本方最优 保护价转换 
    if (od->order_type == OrderType::Market || od->order_type == OrderType::BestOwn) {
        
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
        
        // tick 对齐
        if (px < ob_._lower) px = ob_._lower;
        if (px > ob_._upper) px = ob_._upper;
        Price off = (px - ob_._lower) % ob_._tick;
        px -= off;   // 向下对齐

        od->price      = px;
        od->order_type = OrderType::Limit;
    }
    
    // 撮合（只与历史订单撮合）
    match(od);
    
    // 撮合完后，检查虚拟订单是否可成交
    try_match_virtual_orders(od);
    
    if (od->remaining_volume() == 0) return;

    // 剩余挂簿
    int idx = ob_.pxToIdx(od->price);  // ← 现在才算 idx，确保用最终价
    auto& side = buy ? ob_._buy : ob_._sell;
    side[idx].orders.push_back(od);
    od->level_iter = std::prev(side[idx].orders.end());
    
    // 历史订单更新vol_sum
        ob_.bucketAdd(idx, buy, od->remaining_volume());
    
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

// 上海市场撮合逻辑（只处理历史订单vs历史订单）
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

            // 历史订单vs历史订单，正常撮合
            Quantity q = std::min(inc->remaining_volume(), oppo->remaining_volume());
            if (q == 0) break;

            // 执行成交
            inc->traded_volume += q;
            oppo->traded_volume += q;
            ob_.bucketSub(best, !buy, q);

            // 成交回调
            if (ob_._on_exec) {
                uint64_t bid_id = buy ? inc->order_id : oppo->order_id;
                uint64_t ask_id = buy ? oppo->order_id : inc->order_id;
                
                auto bid_it = ob_.sys2input_.find(bid_id);
                auto ask_it = ob_.sys2input_.find(ask_id);
                
                Execution ex(
                    bid_it != ob_.sys2input_.end() ? bid_it->second : bid_id,
                    ask_it != ob_.sys2input_.end() ? ask_it->second : ask_id,
                    px, q);
                ob_._on_exec(ex);
            }

            // 对手方订单完全成交，移出订单簿
            if (oppo->remaining_volume() == 0) {
                // 通知虚拟订单系统
                on_historical_order_removing(oppo, best, !buy, true);
                
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

// 深圳市场撮合逻辑（只处理历史订单vs历史订单）
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

            // 历史订单vs历史订单，正常撮合
            Quantity q = std::min(inc->remaining_volume(), oppo->remaining_volume());
            if (q == 0) break;

            // 执行成交
            inc->traded_volume += q;
            oppo->traded_volume += q;
            ob_.bucketSub(best, !buy, q);

            // 成交回调
            if (ob_._on_exec) {
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
                // 通知虚拟订单系统
                on_historical_order_removing(oppo, best, !buy, true);
                
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
        // 不在历史订单簿中，尝试撤销虚拟订单
        if (cancel_virtual_order(oid)) {
            return true;
        }
        
        // 订单不存在，撤单失败
        auto orig_it = ob_.sys2input_.find(oid);
        uint64_t cstra_id = (orig_it != ob_.sys2input_.end()) ? orig_it->second : oid;
        std::cerr << "❌ [连续竞价撤单失败] cstra_id=" << cstra_id << " -> 订单不存在" << std::endl;
        if (on_cancel_) on_cancel_(oid, false, "订单不存在", nullptr);
        return false;
    }

    auto loc = it->second; // 订单位置
    auto& side = loc.is_buy ? ob_._buy : ob_._sell;
    auto ord = *loc.it;

    // 已撤销或已成交的订单不能重复撤单
    if (ord->status == OrderStatus::Cancelled || ord->status == OrderStatus::Filled) {
        std::string reason = (ord->status == OrderStatus::Cancelled) ? "订单已撤销" : "订单已成交";
        if (on_cancel_) on_cancel_(oid, false, reason, nullptr);
        return false;
    }

    // 通知虚拟订单系统（撤单前）
    if (ord->is_historical) {
        on_historical_order_removing(ord, loc.idx, loc.is_buy, false);
    }
    
    Quantity rem = ord->remaining_volume(); // 剩余未成交量
    side[loc.idx].orders.erase(loc.it);     // 从队列中移除
    
    // 历史订单减vol_sum
    if (ord->is_historical) {
        ob_.bucketSub(loc.idx, loc.is_buy, rem);
    }
    
    ord->status = OrderStatus::Cancelled;   // 状态置为已撤销
    ob_._loc.erase(it);                     // 位置映射移除
    ob_._omap.erase(oid);                   // 系统ID映射移除

    // 撤单成功回调，传递订单信息
    if (on_cancel_) on_cancel_(oid, true, "撤单成功", ord);

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
        // 输入订单ID不存在（未在input2sys_映射中找到）
        std::cerr << "❌ [连续竞价撤单失败] 输入订单ID=" << input_id << " -> input2sys_映射中不存在" << std::endl;
        if (on_cancel_) on_cancel_(input_id, false, "输入订单ID不存在", nullptr);
        return false;
    }
}

// ========== 价格笼子功能已禁用 ==========
// // ============== 价格笼子功能实现（2023年3月前规则）==============
// 
// // 获取买单基准价：即时最低卖一价 → 最近成交价 → 昨收价
// Price ConAuctionEngine::getBuyBenchmark() const
// {
//     // 1. 优先使用即时最低卖一价（best ask）
//     Price best_ask = ob_.bestAsk();
//     if (best_ask > 0) return best_ask;
//     
//     // 2. 无对手方报价，使用最近成交价
//     Price last_trade = ob_.getLastTradePrice();
//     if (last_trade > 0) return last_trade;
//     
//     // 3. 再无成交，使用昨收价
//     return ob_._prev_close_price;
// }
// 
// // 获取卖单基准价：即时最高买一价 → 最近成交价 → 昨收价
// Price ConAuctionEngine::getSellBenchmark() const
// {
//     // 1. 优先使用即时最高买一价（best bid）
//     Price best_bid = ob_.bestBid();
//     if (best_bid > 0) return best_bid;
//     
//     // 2. 无对手方报价，使用最近成交价
//     Price last_trade = ob_.getLastTradePrice();
//     if (last_trade > 0) return last_trade;
//     
//     // 3. 再无成交，使用昨收价
//     return ob_._prev_close_price;
// }
// 
// // 计算价格笼子上限（买单）：基准价 + max(基准价×2%, 0.1元)，向上取整到tick
// Price ConAuctionEngine::calcCageCeiling(Price benchmark) const
// {
//     if (benchmark <= 0) return ob_._upper;  // 无基准价，使用涨停价
//     
//     // 计算变化幅度 = max(基准价 × 2%, 0.1元)
//     // 价格单位是元 × 10000，所以 0.1元 = 1000
//     Price change_2pct = (benchmark * 2) / 100;  // 基准价 × 2%
//     Price min_change = 1000;  // 0.1元 = 1000
//     Price change = std::max(change_2pct, min_change);
//     
//     // 买单上限 = 基准价 + 变化幅度
//     Price ceiling = benchmark + change;
//     
//     // 对齐到tick（向上取整）
//     Price offset = (ceiling - ob_._lower) % ob_._tick;
//     if (offset > 0) {
//         ceiling = ceiling - offset + ob_._tick;
//     }
//     
//     // 不能超过涨停价
//     if (ceiling > ob_._upper) ceiling = ob_._upper;
//     
//     return ceiling;
// }
// 
// // 计算价格笼子下限（卖单）：基准价 - max(基准价×2%, 0.1元)，向下取整到tick
// Price ConAuctionEngine::calcCageFloor(Price benchmark) const
// {
//     if (benchmark <= 0) return ob_._lower;  // 无基准价，使用跌停价
//     
//     // 计算变化幅度 = max(基准价 × 2%, 0.1元)
//     // 价格单位是元 × 10000，所以 0.1元 = 1000
//     Price change_2pct = (benchmark * 2) / 100;  // 基准价 × 2%
//     Price min_change = 1000;  // 0.1元 = 1000
//     Price change = std::max(change_2pct, min_change);
//     
//     // 卖单下限 = 基准价 - 变化幅度
//     Price floor = benchmark - change;
//     
//     // 对齐到tick（向下取整）
//     if (floor < ob_._lower) floor = ob_._lower;
//     Price offset = (floor - ob_._lower) % ob_._tick;
//     floor = floor - offset;
//     
//     // 不能低于跌停价
//     if (floor < ob_._lower) floor = ob_._lower;
//     
//     return floor;
// }
// 
// // 检查订单是否在价格笼子范围内
// bool ConAuctionEngine::isInPriceCage(const std::shared_ptr<Order>& od) const
// {
//     if (!price_cage_enabled_) return true;  // 未启用价格笼子，始终返回true
//     
//     bool buy = od->direction == Direction::Buy;
//     
//     if (buy) {
//         // 买单：价格 ≤ 基准价 + max(基准价×2%, 0.1元)
//         Price benchmark = getBuyBenchmark();
//         Price ceiling = calcCageCeiling(benchmark);
//         return od->price <= ceiling;
//     } else {
//         // 卖单：价格 ≥ 基准价 - max(基准价×2%, 0.1元)
//         Price benchmark = getSellBenchmark();
//         Price floor = calcCageFloor(benchmark);
//         return od->price >= floor;
//     }
// }
// 
// // 根据订单时间自动检测并启用价格笼子（2023年3月1日前自动启用）
// void ConAuctionEngine::checkAndEnablePriceCage(const std::string& datetime)
// {
//     // datetime 格式: "YYYY-MM-DD HH:MM:SS" 或 "YYYY-MM-DD HH:MM:SS.mmm"
//     // 只需要比较前10个字符 "YYYY-MM-DD"
//     if (datetime.size() < 10) {
//         price_cage_enabled_ = false;
//         return;
//     }
//     
//     // 提取日期部分并比较
//     std::string date_str = datetime.substr(0, 10);  // "YYYY-MM-DD"
//     
//     // 2023年3月1日开始取消价格笼子，所以之前的日期启用
//     // 比较字符串即可（因为格式是 YYYY-MM-DD，字典序等于日期序）
//     price_cage_enabled_ = (date_str < "2023-03-01");
// }

// 添加到休眠队列
void ConAuctionEngine::addToDormant(std::shared_ptr<Order> od)
{
    bool buy = od->direction == Direction::Buy;
    if (buy) {
        dormant_buy_orders_.push_back(od);
    } else {
        dormant_sell_orders_.push_back(od);
    }
    od->status = OrderStatus::Pending;  // 标记为待处理（休眠）
}

// ========== 价格笼子功能已禁用 ==========
// // 尝试激活休眠订单
// void ConAuctionEngine::tryActivateDormantOrders()
// {
//     if (!price_cage_enabled_) return;
//     
//     // 尝试激活休眠买单
//     auto buy_it = dormant_buy_orders_.begin();
//     while (buy_it != dormant_buy_orders_.end()) {
//         auto& od = *buy_it;
//         if (isInPriceCage(od)) {
//             // 订单落回笼子范围内，激活并送入撮合
//             auto activated = od;
//             buy_it = dormant_buy_orders_.erase(buy_it);
//             
//             // 重新送入撮合（不再检查价格笼子）
//             ob_._omap[activated->order_id] = activated;
//             match(activated);
//             if (activated->remaining_volume() == 0) continue;
//             
//             // 剩余部分挂入盘口
//             int idx = ob_.pxToIdx(activated->price);
//             auto& side = ob_._buy;
//             side[idx].orders.push_back(activated);
//             activated->level_iter = std::prev(side[idx].orders.end());
//             
//             if (activated->broker == "BRK") {
//                 ob_.bucketAdd(idx, true, activated->remaining_volume());
//             } else {
//                 side[idx].user_vol_sum += activated->remaining_volume();
//             }
//             ob_._loc[activated->order_id] = { true, idx, activated->level_iter };
//         } else {
//             ++buy_it;
//         }
//     }
//     
//     // 尝试激活休眠卖单
//     auto sell_it = dormant_sell_orders_.begin();
//     while (sell_it != dormant_sell_orders_.end()) {
//         auto& od = *sell_it;
//         if (isInPriceCage(od)) {
//             // 订单落回笼子范围内，激活并送入撮合
//             auto activated = od;
//             sell_it = dormant_sell_orders_.erase(sell_it);
//             
//             // 重新送入撮合
//             ob_._omap[activated->order_id] = activated;
//             match(activated);
//             if (activated->remaining_volume() == 0) continue;
//             
//             // 剩余部分挂入盘口
//             int idx = ob_.pxToIdx(activated->price);
//             auto& side = ob_._sell;
//             side[idx].orders.push_back(activated);
//             activated->level_iter = std::prev(side[idx].orders.end());
//             
//             if (activated->broker == "BRK") {
//                 ob_.bucketAdd(idx, false, activated->remaining_volume());
//             } else {
//                 side[idx].user_vol_sum += activated->remaining_volume();
//             }
//             ob_._loc[activated->order_id] = { false, idx, activated->level_iter };
//         } else {
//             ++sell_it;
//         }
//     }
// }

} // namespace wangcai 