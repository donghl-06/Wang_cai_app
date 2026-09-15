// === src/con_auction_engine.cpp ===
#include "../include/con_auction_engine.hpp"
#include <algorithm>
#include <iostream>

namespace wangcai {

// ============== 真实成交替代模式函数实现 ==============

// RT模式：虚拟订单成交（支持部分成交）
void ConAuctionEngine::rt_virtual_fill(std::shared_ptr<Order>& od, Price fill_price, Quantity fill_qty)
{
    od->traded_volume += fill_qty;
    if (od->remaining_volume() == 0) {
        od->status = OrderStatus::Filled;
    } else {
        od->status = OrderStatus::PartFilled;
    }
    
    if (ob_._on_exec) {
        bool buy = (od->direction == Direction::Buy);
        Execution ex = Execution::userSynthetic(
            buy ? od->order_id : 0,    // 买方ID（虚拟订单）或0
            buy ? 0 : od->order_id,    // 卖方ID（虚拟订单）或0
            fill_price,
            fill_qty
        );
        ob_._on_exec(ex);
    }
}

// RT模式：被动虚拟订单入队
void ConAuctionEngine::accept_rt_virtual_order(std::shared_ptr<Order> od)
{
    bool buy = (od->direction == Direction::Buy);
    int idx = ob_.pxToIdx(od->price);
    auto& same_side = buy ? ob_._buy : ob_._sell;
    
    // 队列位置 = 同侧同价位历史订单的当前总量
    uint64_t queue_pos = same_side[idx].vol_sum;
    
    // 记录下单时真实成交池的累积值（只算下单之后的增量才是"属于我的"）
    auto& pool = buy ? rt_pool_buy_ : rt_pool_sell_;
    uint64_t pool_entry = pool[od->price];

    // 记录入场边界：下单瞬间同价位最后一个历史单ID
    // 后续只有 <= 该ID 的历史撤单，才会推进本虚拟单 queue_position
    uint64_t frontier_hist_order_id = 0;
    const auto& bkt = same_side[idx];
    for (auto rit = bkt.orders.rbegin(); rit != bkt.orders.rend(); ++rit) {
        if ((*rit)->is_historical) {
            frontier_hist_order_id = (*rit)->order_id;
            break;
        }
    }
    
    // (DIAG-2 RT_ACCEPT 诊断日志已移除)

    // 入队到RT虚拟订单簿
    auto& vmap = buy ? rt_virtual_buy_ : rt_virtual_sell_;
    vmap[od->price].emplace_back(RtVirtualOrder(od, queue_pos, pool_entry, frontier_hist_order_id));
    
    // 获取刚插入的迭代器
    auto it = std::prev(vmap[od->price].end());
    
    // 维护位置映射（O(1)撤单）
    rt_virtual_loc_[od->order_id] = { od->price, buy, it };
    
    // 添加到omap以便后续查找
    ob_._omap[od->order_id] = od;
    
    // 立即尝试一次撮合（避免漏掉下单后已进入池子的同价真实成交）
    try_fill_rt_virtual_orders(od->price, buy);
}

// RT模式：喂入真实成交事件
void ConAuctionEngine::feedRealTrade(Price price, Quantity volume, bool buy_side_passive)
{
    // 累加到对应侧的真实成交池
    auto& pool = buy_side_passive ? rt_pool_buy_ : rt_pool_sell_;
    pool[price] += volume;
    diag_.rt_feed_count++;
    diag_.rt_feed_volume += volume;
    
    // (DIAG-7 FEED_RT 诊断日志已移除)
    
    // 尝试成交该价位该侧的虚拟订单
    try_fill_rt_virtual_orders(price, buy_side_passive);
}

// 用户下单回调：采集同价位同侧的历史队列信息
ConAuctionEngine::QueueInfo ConAuctionEngine::getQueueInfoForUserOrder(Price price, bool is_buy) const
{
    QueueInfo info;
    if (!queue_info_enabled_) return info;

    try {
        int idx = ob_.pxToIdx(price);
        const auto& side = is_buy ? ob_._buy : ob_._sell;
        if (idx < 0 || idx >= static_cast<int>(side.size())) {
            return info;
        }

        const auto& bkt = side[idx];
        info.ahead_count = 0;
        info.ahead_volume = 0;

        std::vector<uint64_t> hist_ids;
        hist_ids.reserve(16);
        for (const auto& ord : bkt.orders) {
            if (!ord || !ord->is_historical) continue;
            info.ahead_count += 1;
            info.ahead_volume += static_cast<int64_t>(ord->remaining_volume());

            // 优先返回真实 orderid（input_id）；若缺失则退化到映射查询
            uint64_t raw_id = ord->input_id;
            if (raw_id == 0) raw_id = ob_.getInputId(ord->order_id);
            hist_ids.push_back(raw_id);
        }

        // 取最近3个前序历史订单ID（从近到远）
        for (auto rit = hist_ids.rbegin();
             rit != hist_ids.rend() && info.prev_order_ids.size() < 3;
             ++rit) {
            info.prev_order_ids.push_back(*rit);
        }
    } catch (...) {
        // 价格越界/未对齐等异常时保持默认值，避免影响主流程
        return info;
    }

    return info;
}

// RT模式：尝试成交某价位某侧的虚拟订单
void ConAuctionEngine::try_fill_rt_virtual_orders(Price price, bool is_buy_side)
{
    auto& vmap = is_buy_side ? rt_virtual_buy_ : rt_virtual_sell_;
    
    auto vit = vmap.find(price);
    if (vit == vmap.end()) return;
    
    // 获取该价位该侧的真实成交池总量
    auto& pool = is_buy_side ? rt_pool_buy_ : rt_pool_sell_;
    uint64_t pool_at_price = pool[price];
    
    auto& vlist = vit->second;
    uint64_t virtual_fills_before = 0;  // 前面所有虚拟订单的累积已成交量
    
    for (auto it = vlist.begin(); it != vlist.end(); ) {
        auto& vod = *it;
        
        // 关键：只算下单之后的真实成交增量
        // pool_delta = 下单以来该价位新增的真实成交量
        // available = pool_delta - queue_position - 前面虚拟订单的已成交总量
        uint64_t pool_delta = (pool_at_price >= vod.pool_at_entry) 
                            ? (pool_at_price - vod.pool_at_entry) : 0;
        int64_t available = static_cast<int64_t>(pool_delta)
                          - static_cast<int64_t>(vod.queue_position) 
                          - static_cast<int64_t>(virtual_fills_before);
        
        // (DIAG-3 RT_TRY_FILL 诊断日志已移除)

        if (available > 0) {
            Quantity can_fill = static_cast<Quantity>(available);
            Quantity remaining = vod.order->remaining_volume();
            Quantity fill_qty = std::min(can_fill, remaining);
            
            if (fill_qty > 0) {
                diag_.passive_fill_count++;
                // (DIAG-3 RT_FILL 诊断日志已移除)
                vod.filled_volume += fill_qty;
                rt_virtual_fill(vod.order, price, fill_qty);
            }
        }
        
        // 无论是否成交，都累加此订单的已成交量（给后面的订单计算用）
        virtual_fills_before += vod.filled_volume;
        
        // 完全成交的订单移出队列
        if (vod.order->remaining_volume() == 0) {
            rt_virtual_loc_.erase(vod.order->order_id);
            ob_._omap.erase(vod.order->order_id);
            it = vlist.erase(it);
        } else {
            ++it;
        }
    }
    
    // 清空空的价位
    if (vlist.empty()) {
        vmap.erase(vit);
    }
}

// RT模式：撤销虚拟订单（O(1)复杂度）
bool ConAuctionEngine::cancel_rt_virtual_order(uint64_t order_id)
{
    auto loc_it = rt_virtual_loc_.find(order_id);
    if (loc_it == rt_virtual_loc_.end()) return false;
    
    auto& loc = loc_it->second;
    auto& vmap = loc.is_buy ? rt_virtual_buy_ : rt_virtual_sell_;
    auto& vlist = vmap[loc.price];
    auto& vod = *loc.it;
    
    diag_.rt_cancel_count++;
    // 更新状态
    vod.order->status = OrderStatus::Cancelled;
    
    // 回调
    if (on_cancel_) on_cancel_(order_id, true, "撤单成功", vod.order);
    
    // 从各映射中移除
    ob_._omap.erase(order_id);
    vlist.erase(loc.it);
    rt_virtual_loc_.erase(loc_it);
    
    // 如果该价位为空，移除整个价位
    if (vlist.empty()) {
        vmap.erase(loc.price);
    }
    
    return true;
}

// RT模式：历史订单撤单时调整队列位置
void ConAuctionEngine::on_historical_order_cancel_rt(int bucket_idx, bool is_buy, uint64_t cancel_hist_order_id, Quantity cancel_vol)
{
    Price price = ob_._lower + bucket_idx * ob_._tick;
    auto& vmap = is_buy ? rt_virtual_buy_ : rt_virtual_sell_;
    
    auto vit = vmap.find(price);
    if (vit == vmap.end()) return;
    
    // 只推进"边界之前"的历史撤单：
    // 对每个虚拟单，只有取消单ID <= frontier_hist_order_id，才减少 queue_position
    for (auto& vod : vit->second) {
        if (vod.frontier_hist_order_id == 0 || cancel_hist_order_id > vod.frontier_hist_order_id) {
            continue;
        }
        if (vod.queue_position >= cancel_vol) {
            vod.queue_position -= cancel_vol;
        } else {
            vod.queue_position = 0;
        }
    }
    
    // 调整后可能有虚拟订单可以成交了
    try_fill_rt_virtual_orders(price, is_buy);
}

// ============== 虚拟订单处理函数实现 ==============

// 虚拟订单成交（counterparty_id是对手方历史订单ID）
void ConAuctionEngine::virtual_fill(std::shared_ptr<Order>& od, Price fill_price, uint64_t counterparty_id)
{
    od->traded_volume = od->volume;
    od->status = OrderStatus::Filled;
    
    if (ob_._on_exec) {
        bool buy = (od->direction == Direction::Buy);

        Execution ex = Execution::userHistorical(
            buy ? od->order_id : counterparty_id,   // 买方内部ID
            buy ? counterparty_id : od->order_id,   // 卖方内部ID
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
    
    // ========== 第一步：检查是否立即成交（主动单）==========
    int& best_opp = buy ? ob_._best_ask : ob_._best_bid;
    if (best_opp != -1) {
        Price best_opp_price = ob_._lower + best_opp * ob_._tick;
        bool can_trade = (buy && best_opp_price <= od->price) || 
                         (!buy && best_opp_price >= od->price);
        if (can_trade) {
            diag_.active_order_count++;
            // (DIAG-1 ACTIVE order 诊断日志已移除)
            // ========== 严格主动单模式检查 ==========
            // 如果开启了严格模式且有未还清的欠债，拒绝主动单
            if (strict_active_order_mode_ && debt_volume_ > 0) {
                diag_.debt_reject_count++;
                // (DIAG-4 DEBT_REJECT 诊断日志已移除)
                // 设置订单状态为 Rejected
                od->status = OrderStatus::Rejected;
                ob_._omap[od->order_id] = od;  // 添加到omap以便查询
                
                // 通过撤单回调通知策略订单被拒绝
                //
                // 重要说明（为什么这里用 success=true）：
                // - BacktestEngine 在注册 ConAuctionEngine 的 on_cancel_ 回调时，当前只在 success==true
                //   的情况下才会向策略发送统一回调（onTradeCallback matchtype='D'）并清理映射。
                // - 如果这里传 success=false，策略侧“看不到拒单”，且映射可能残留，影响后续测试/回测。
                // - 因此这里把“拒单”也走同一条通知链路：用 success=true + reason 明确标注为拒单。
                if (on_cancel_) {
                    on_cancel_(od->order_id, true, 
                        "严格主动单模式：存在未还清的欠债，禁止下新的主动单", od);
                }
                return;
            }
            
            // ========== 严格主动单模式：计算实际可成交量并记录欠债 ==========
            if (strict_active_order_mode_) {
                // 遍历对手方订单簿，计算虚拟主动单实际能吃掉多少量
                auto& opp_side = buy ? ob_._sell : ob_._buy;
                Quantity remaining = od->volume;
                Quantity total_available = 0;
                int current_idx = best_opp;
                Price fill_price = best_opp_price;
                uint64_t counterparty_id = 0;
                
                // 计算可成交量（扫描对手方，不再逐单记录 debt）
                while (remaining > 0 && current_idx != -1) {
                    Price px = ob_._lower + current_idx * ob_._tick;
                    if ((buy && px > od->price) || (!buy && px < od->price)) {
                        break;
                    }
                    
                    auto& bkt = opp_side[current_idx];
                    for (auto& hist_order : bkt.orders) {
                        if (!hist_order->is_historical) continue;
                        
                        if (counterparty_id == 0) {
                            counterparty_id = hist_order->order_id;
                        }
                        
                        Quantity q = std::min(remaining, hist_order->remaining_volume());
                        total_available += q;
                        remaining -= q;
                        
                        if (remaining == 0) break;
                    }
                    
                    current_idx = buy ? bkt.next : bkt.prev;
                }
                Quantity actual_fill = total_available;
                Quantity unfilled = od->volume - actual_fill;
                
                // 按量记录 debt：虚拟消耗了多少就欠多少
                uint64_t debt_before = debt_volume_;
                debt_volume_ += actual_fill;
                diag_.debt_create_total += actual_fill;

                // (DIAG-5 DEBT_CREATE 诊断日志已移除)
                
                if (actual_fill > 0) {
                    diag_.active_fill_count++;
                    diag_.active_fill_volume += actual_fill;
                    // 部分成交或全部成交
                    od->traded_volume = actual_fill;
                    
                    if (unfilled > 0) {
                        // 有剩余：部分成交 + 剩余撤单
                        od->status = OrderStatus::PartFilled;
                        
                        // 发送成交回调
                        if (ob_._on_exec) {
                            Execution ex = Execution::userAggregated(
                                buy ? od->order_id : 0,
                                buy ? 0 : od->order_id,
                                fill_price,
                                actual_fill
                            );
                            ob_._on_exec(ex);
                        }
                        
                        // 剩余部分撤单回调
                        if (on_cancel_) {
                            on_cancel_(od->order_id, true, 
                                "严格主动单模式：市场量不足，剩余" + std::to_string(unfilled) + "撤单", od);
                        }
                    } else {
                        // 全部成交
                        od->status = OrderStatus::Filled;
                        
                        // 发送成交回调
                        if (ob_._on_exec) {
                            Execution ex = Execution::userAggregated(
                                buy ? od->order_id : 0,
                                buy ? 0 : od->order_id,
                                fill_price,
                                actual_fill
                            );
                            ob_._on_exec(ex);
                        }
                    }
                } else {
                    // 市场没有任何可用量，全部撤单
                    od->status = OrderStatus::Cancelled;
                    if (on_cancel_) {
                        on_cancel_(od->order_id, true, 
                            "严格主动单模式：市场无可用量，全部撤单", od);
                    }
                }
                
                ob_._omap[od->order_id] = od;  // 添加到omap以便查询
                return;
            }
            
            // ========== 非严格模式：原有逻辑，立即全部成交 ==========
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
    
    // ========== 第二步：被动单处理 ==========
    diag_.passive_order_count++;
    // (DIAG-1 PASSIVE order 诊断日志已移除)
    // 如果启用了真实成交替代模式，走RT虚拟订单队列
    if (real_trade_match_mode_) {
        accept_rt_virtual_order(od);
        return;
    }
    
    // ========== 默认模式：找同侧同价位最后一个历史订单 ==========
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
    // ========== 严格主动单模式：按量清除欠债 ==========
    // debt 的按量扣减在 match_sh/match_sz 每次成交时完成，此处不再处理
    
    // ========== 真实成交替代模式：撤单时调整队列位置 ==========
    // 只有撤单（非成交）才需要调整，因为成交会通过 feedRealTrade 增加真实成交池
    if (real_trade_match_mode_ && !is_filled) {
        Quantity cancel_vol = removing_order->remaining_volume();
        if (cancel_vol > 0) {
            on_historical_order_cancel_rt(bucket_idx, is_buy, removing_order->order_id, cancel_vol);
        }
    }
    
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

// ============== 价格笼子功能实现 ==============

// --- 历史单暂存（反推法，行情可见性重建） ---

void ConAuctionEngine::suspendHistoricalOrder(std::shared_ptr<Order> od)
{
    od->status = OrderStatus::Pending;
    ob_._omap[od->order_id] = od;  // 供身份/查询；不在 _loc（不进簿，十档不可见）
    suspended_hist_[od->order_id] = od;
}

// 出笼判据（数值规则，创业板暂存窗口的制度语义："价格落回有效申报价格范围"）：
// 申报价落回 [基准×(1-幅度), 基准×(1+幅度)] 即出笼——出笼时可以仍然穿价
// （例：买 7.37 入笼后基准由 7.21 升至 7.23，7.37 ≤ 7.3746 出笼吃 7.23 卖单）。
// 基准价链与策略笼共用 userCageBounds。map 有序迭代 = 按入笼序（即市场委托序）恢复。
void ConAuctionEngine::activateEligibleSuspendedHistorical(const PriceCageRule& rule)
{
    if (suspended_hist_.empty() || !rule.enabled) return;
    std::vector<uint64_t> to_activate;
    for (const auto& [sid, od] : suspended_hist_) {
        const CageBounds b = userCageBounds(od->direction == Direction::Buy, rule, ob_);
        if (b.lo > 0 && od->price >= b.lo && od->price <= b.hi) to_activate.push_back(sid);
    }
    for (uint64_t sid : to_activate) {
        auto od = suspended_hist_[sid];
        suspended_hist_.erase(sid);
        accept(od);  // 出笼即正常参与撮合（穿价则立即吃对手，与真实一致）
    }
}

std::vector<std::shared_ptr<Order>> ConAuctionEngine::takeAllSuspendedHistorical()
{
    std::vector<std::shared_ptr<Order>> out;
    out.reserve(suspended_hist_.size());
    for (auto& [sid, od] : suspended_hist_) out.push_back(od);
    suspended_hist_.clear();
    return out;
}

// --- 策略单数值判定 ---

ConAuctionEngine::CageBounds
ConAuctionEngine::userCageBounds(bool is_buy, const PriceCageRule& rule, const OrderBook& ob)
{
    // 基准价链：对手方最优价 → 本方最优价 → 最新成交价 → 昨收价
    Price base = is_buy ? ob.bestAsk() : ob.bestBid();
    if (base == 0) base = is_buy ? ob.bestBid() : ob.bestAsk();
    if (base == 0) base = ob.getLastTradePrice();
    if (base == 0) base = ob.getPrevClose();
    if (base == 0) return {0, 0};  // 无任何基准（极端空市，不应发生）

    // 幅度 = max(基准 × pct%, 兜底额)；整数运算防浮点误差
    Price band = base * rule.pct_num / 10000;
    if (band < rule.min_abs) band = rule.min_abs;

    // 有效申报范围 = 基准 ± 幅度，计算结果四舍五入至最小变动价位
    //（真实案例 300026 2022-06-30 三个边界单共同锁定此语义：
    //   基准 7.49→上限 7.6398≈7.64，申报 7.64 恰合规立即成交；
    //   基准 7.72→上限 7.8744≈7.87，申报 7.88 超范围入笼；
    //   基准 7.23→上限 7.3746≈7.37，申报 7.37 恰合规出笼）
    const Price tick = ob.getTick();
    Price hi = base + band;
    Price lo = base > band ? base - band : 0;
    if (tick > 0) {
        hi = (hi + tick / 2) / tick * tick;
        lo = (lo + tick / 2) / tick * tick;
    }

    // 夹在涨跌停内
    if (hi > ob.getUpperLimit()) hi = ob.getUpperLimit();
    if (lo < ob.getLowerLimit()) lo = ob.getLowerLimit();
    return {lo, hi};
}

void ConAuctionEngine::suspendUserOrder(std::shared_ptr<Order> od)
{
    od->status = OrderStatus::Pending;
    ob_._omap[od->order_id] = od;
    user_cage_[od->order_id] = od;
}

// 策略笼单出笼：数值范围重查（暂存单的恢复条件 = 价格落回有效申报范围）
void ConAuctionEngine::activateEligibleUserCageOrders(const PriceCageRule& rule)
{
    if (user_cage_.empty() || !rule.enabled) return;
    std::vector<uint64_t> to_activate;
    for (const auto& [sid, od] : user_cage_) {
        const CageBounds b = userCageBounds(od->direction == Direction::Buy, rule, ob_);
        if (b.lo > 0 && od->price >= b.lo && od->price <= b.hi) to_activate.push_back(sid);
    }
    for (uint64_t sid : to_activate) {
        auto od = user_cage_[sid];
        user_cage_.erase(sid);
        ob_._omap.erase(sid);  // accept_virtual_order 会重新登记
        accept(od);            // 虚拟单路径（默认/RT 模式自动分流）
    }
}

std::vector<std::shared_ptr<Order>> ConAuctionEngine::takeAllUserCageOrders()
{
    std::vector<std::shared_ptr<Order>> out;
    out.reserve(user_cage_.size());
    for (auto& [sid, od] : user_cage_) out.push_back(od);
    user_cage_.clear();
    return out;
}

// 策略单废单：涨跌停/价格笼子拒单，走与严格模式拒单相同的回调链路
// （success=true + reason，由 BacktestEngine 的 on_cancel_ 转成 'D' 回调通知策略）
void ConAuctionEngine::rejectUserOrder(std::shared_ptr<Order> od, const std::string& reason)
{
    od->status = OrderStatus::Rejected;
    ob_._omap[od->order_id] = od;  // 添加到omap以便查询
    if (on_cancel_) on_cancel_(od->order_id, true, reason, od);
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
        ob_.eraseActiveMarketOrder(od->order_id);
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
        MarketOrderKey market_key{od->market_channel_no, ext_id};
        if (orig == OrderType::Market) {
            // 先看历史成交价格
            if (auto it = ob_.first_trade_px_.find(market_key); it != ob_.first_trade_px_.end())
                px = it->second;
            // 无成交 → 对手最优
            if (px == 0) px = buy ? ob_.bestAsk() : ob_.bestBid();
        }
        else if (orig == OrderType::BestOwn) {
            // 己方最优
            px = buy ? ob_.bestBid() : ob_.bestAsk();
            // 己方空簿但真实有成交(簿发散):首成交价≈入场时本方价,是最佳重建
            if (px == 0) {
                if (auto it = ob_.first_trade_px_.find(market_key); it != ob_.first_trade_px_.end())
                    px = it->second;
            }
        }

        if (px == 0) {
            // 空簿即撤:市价单对手方空簿 / 本方最优己方空簿且真实零成交 →
            // 交易所当场自动撤销(零成交,撤单记录与委托同时间戳;
            // adata 000006.SZ 2024-11-01 实证 7 笔本方最优 + 9 笔市价单)。
            // 登记身份后不撮合、不挂簿,后续真实撤单记录由引擎吸收为 no-op。
            ob_.markEntryCancelled(od->order_id);
            return;
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
    
    if (od->remaining_volume() == 0) {
        ob_.eraseActiveMarketOrder(od->order_id);
        return;
    }

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
                ob_.eraseActiveMarketOrder(oppo->order_id);
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
                Execution ex = Execution::historical(
                    buy ? inc->order_id : oppo->order_id,
                    buy ? oppo->order_id : inc->order_id,
                    px, q);
                ob_._on_exec(ex);
            }

            // RT模式：历史单撮合即"真实成交"，喂入真实成交池
            // inc=主动方, oppo=被动方; buy=inc方向 → 被动侧=!buy
            if (real_trade_match_mode_) {
                feedRealTrade(px, q, !buy);
            }

            // 严格模式：每次真实成交按量扣减 debt
            if (strict_active_order_mode_ && debt_volume_ > 0) {
                uint64_t reduce = std::min(static_cast<uint64_t>(q), debt_volume_);
                debt_volume_ -= reduce;
                diag_.debt_clear_total += reduce;
            }

            // 对手方订单完全成交，移出订单簿
            if (oppo->remaining_volume() == 0) {
                // 通知虚拟订单系统
                on_historical_order_removing(oppo, best, !buy, true);
                
                oppo->status = OrderStatus::Filled;
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
                ob_.eraseActiveMarketOrder(oppo->order_id);
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
                ob_.eraseActiveMarketOrder(oppo->order_id);
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
                Execution ex = Execution::historical(
                    buy ? inc->order_id : oppo->order_id,
                    buy ? oppo->order_id : inc->order_id,
                    px, q);
                ob_._on_exec(ex);
            }

            // RT模式：历史单撮合即"真实成交"，喂入真实成交池
            if (real_trade_match_mode_) {
                feedRealTrade(px, q, !buy);
            }

            // 严格模式：每次真实成交按量扣减 debt
            if (strict_active_order_mode_ && debt_volume_ > 0) {
                uint64_t reduce = std::min(static_cast<uint64_t>(q), debt_volume_);
                debt_volume_ -= reduce;
                diag_.debt_clear_total += reduce;
            }

            // 对手方订单完全成交，移出订单簿
            if (oppo->remaining_volume() == 0) {
                // 通知虚拟订单系统
                on_historical_order_removing(oppo, best, !buy, true);
                
                oppo->status = OrderStatus::Filled;
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
                ob_.eraseActiveMarketOrder(oppo->order_id);
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
    // 价格笼子：笼中历史单可直接撤（暂存期间投资者可撤单）
    if (auto sit = suspended_hist_.find(oid); sit != suspended_hist_.end()) {
        auto od = sit->second;
        suspended_hist_.erase(sit);
        od->status = OrderStatus::Cancelled;
        ob_._omap.erase(oid);
        ob_.eraseActiveMarketOrder(oid);
        if (on_cancel_) on_cancel_(oid, true, "撤单成功", od);
        return true;
    }
    // 价格笼子：策略笼单可直接撤
    if (auto uit = user_cage_.find(oid); uit != user_cage_.end()) {
        auto od = uit->second;
        user_cage_.erase(uit);
        od->status = OrderStatus::Cancelled;
        ob_._omap.erase(oid);
        if (on_cancel_) on_cancel_(oid, true, "撤单成功", od);
        return true;
    }

    auto it = ob_._loc.find(oid);
    if (it == ob_._loc.end()) {
        // 不在历史订单簿中，尝试撤销虚拟订单
        if (cancel_virtual_order(oid)) {
            return true;
        }
        
        // 尝试撤销RT模式虚拟订单
        if (real_trade_match_mode_ && cancel_rt_virtual_order(oid)) {
            return true;
        }
        
        // 订单不存在，撤单失败
        uint64_t cstra_id = ob_.getInputId(oid);
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

// 撤单（通过市场订单ID和通道号）
bool ConAuctionEngine::cancel_by_input_id(uint64_t input_id, int channel_no)
{
    auto system_id = ob_.findSystemOrderId(input_id, channel_no);
    if (system_id.has_value()) {
        uint64_t sys_id = *system_id;
        bool result = cancel(sys_id);
        ob_.eraseActiveMarketOrder(sys_id);
        return result;
    } else {
        // 输入订单ID不存在（未在活跃市场身份映射中找到）
        std::cerr << "❌ [连续竞价撤单失败] channel=" << channel_no
                  << " 输入订单ID=" << input_id << " -> 市场身份映射中不存在" << std::endl;
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
