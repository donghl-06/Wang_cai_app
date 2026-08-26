/*
 * @Author: linzhuoyu
 * @Date: 2025-07-07 08:43:04
 * @LastEditTime: 2025-08-13 13:16:39
 * @FilePath: /wangcai_cpp/src/orderbook.cpp
 */

#include "../include/orderbook.h"
#include "../include/orderpool.h"   // 对象池头文件

#include <cassert>
#include <algorithm>    // std::max / std::min
#include <iostream>     // std::cout
#include <cmath>        // std::round, std::abs
#include <vector>       // std::vector
#include <cstdlib>      // std::abs
#include <map>          // std::map
#include <string>       // std::string
#include <algorithm>    // std::sort

namespace wangcai {

// 在文件开头添加静态变量定义
std::vector<Event> OrderBook::whole_events;
std::vector<Event> OrderBook::tick_events;

// 构造函数
OrderBook::OrderBook(double hi, double lo, bool is_etf, ExecCallback cb)
    : _on_exec(std::move(cb))
{   
    if (hi < lo) throw std::invalid_argument("hi 必须大于 lo");
    
    // 1. 最小价格单位 tick（整数价 1 = 0.0001 元）
    _tick  = is_etf ? 10 : 100;  // ETF = 0.001 元；股票 = 0.01 元

    // 2. 涨跌停区间（整型价格）
    _lower = static_cast<Price>(lo);
    _upper = static_cast<Price>(hi); 

    // 3. 预分配桶数组
    const int bucket_cnt = (_upper - _lower) / _tick + 1; // 桶数 = (最大价格 - 最小价格) / 最小价格单位 + 1
    _buy .resize(bucket_cnt); // 买盘桶
    _sell.resize(bucket_cnt); // 卖盘桶

    _best_bid = _best_ask = -1; //最优价索引
    _prev_close_price = 0; // 前收盘价
}

void OrderBook::registerHistoricalOrder(const std::shared_ptr<Order>& order,
                                        uint64_t market_order_id,
                                        int channel_no,
                                        int trading_day) {
    if (!order || market_order_id == 0 || channel_no < 0) {
        throw MarketIdentityError("历史订单身份缺少 market_order_id 或 channel_no");
    }

    MarketOrderIdentity identity{
        trading_day, order->instrument, channel_no, market_order_id, order->direction
    };
    MarketOrderKey key{channel_no, market_order_id};

    auto [identity_it, identity_inserted] =
        market_identity_by_system_id_.emplace(order->order_id, identity);
    if (!identity_inserted) {
        throw MarketIdentityError("重复注册历史 system-id=" + std::to_string(order->order_id));
    }

    auto [active_it, active_inserted] = market_system_id_by_key_.emplace(key, order->order_id);
    if (!active_inserted && active_it->second != order->order_id) {
        market_identity_by_system_id_.erase(identity_it);
        throw MarketIdentityError(
            "同一通道存在重复的活跃市场订单: channel=" + std::to_string(channel_no) +
            ", order_id=" + std::to_string(market_order_id));
    }

    order->is_historical = true;
    order->input_id = market_order_id;
    order->market_channel_no = channel_no;
    order->market_trading_day = trading_day;
}

const MarketOrderIdentity& OrderBook::requireMarketIdentity(uint64_t system_id) const {
    auto it = market_identity_by_system_id_.find(system_id);
    if (it == market_identity_by_system_id_.end()) {
        throw MarketIdentityError("无法把内部 system-id 还原为市场订单 ID: " +
                                  std::to_string(system_id));
    }
    return it->second;
}

std::optional<uint64_t> OrderBook::findSystemOrderId(uint64_t market_order_id,
                                                     int channel_no) const {
    if (market_order_id == 0) return std::nullopt;
    if (channel_no >= 0) {
        auto it = market_system_id_by_key_.find({channel_no, market_order_id});
        if (it == market_system_id_by_key_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<uint64_t> result;
    for (const auto& [key, system_id] : market_system_id_by_key_) {
        if (key.market_order_id != market_order_id) continue;
        if (result.has_value() && *result != system_id) {
            throw MarketIdentityError("裸市场订单 ID 在多个通道中有歧义: " +
                                      std::to_string(market_order_id));
        }
        result = system_id;
    }
    return result;
}

void OrderBook::eraseActiveMarketOrder(uint64_t system_id) {
    auto identity_it = market_identity_by_system_id_.find(system_id);
    if (identity_it == market_identity_by_system_id_.end()) return;
    MarketOrderKey key{identity_it->second.channel_no, identity_it->second.market_order_id};
    auto active_it = market_system_id_by_key_.find(key);
    if (active_it != market_system_id_by_key_.end() && active_it->second == system_id) {
        market_system_id_by_key_.erase(active_it);
    }
}

// 插入事件到有序列表
void OrderBook::insertEvent(const Event& event) {
    // 时间过滤：只处理集合竞价时间段 09:15:00 到 09:25:00  
    std::string time_part = event.datetime.substr(11); // 提取时间部分 HH:MM:SS
    if (time_part < "09:15:00" ) {
         return; // 跳过不在集合竞价时间段的事件
    }  
    whole_events.push_back(event);
}

void OrderBook::insertTick(const Event& event) {
    // 时间过滤：只处理集合竞价时间段 09:15:00 到 09:25:00  
    std::string time_part = event.datetime.substr(11); // 提取时间部分 HH:MM:SS
    if (time_part < "09:15:00" ) {
         return; // 跳过不在集合竞价时间段的事件
    }  
    tick_events.push_back(event);
}

//链表维护
void OrderBook::attachBucket(int idx, bool is_buy)//挂入桶
{
    // 选择买方或卖方桶数组
    auto& side = is_buy ? _buy : _sell;
    Bucket& bkt = side[idx];
    if (bkt.vol_sum == 0) return;  // 桶内无订单，不挂入链表

    // 如果该桶已经在链表中（prev/next已设置，或正好是最优价头节点），则无需重复挂入
    if (bkt.prev != -1 || bkt.next != -1 || idx == (is_buy ? _best_bid : _best_ask))
        return;  // 已在链中，直接返回

    if (is_buy) {  // 买盘：链表按idx降序排列，头节点为最高价
        if (_best_bid == -1 || idx > _best_bid) {
            // 当前无最优买价，或新桶价格更高，直接插到头部
            bkt.next = _best_bid;
            if (_best_bid != -1) _buy[_best_bid].prev = idx; // 原头节点prev指向新桶
            _best_bid = idx; // 更新最优买价索引
        } else {
            // 找到合适的插入位置（保持降序）
            int cur = _best_bid;
            while (_buy[cur].next != -1 && _buy[cur].next > idx)
                cur = _buy[cur].next;
            int nxt = _buy[cur].next;
            bkt.prev = cur;  // 设置新桶的前驱
            bkt.next = nxt;  // 设置新桶的后继
            _buy[cur].next = idx;  // 更新前驱节点的后继
            if (nxt != -1) _buy[nxt].prev = idx;
        }
    } else { // 卖盘：链表按idx升序排列，头节点为最低价
        if (_best_ask == -1 || idx < _best_ask) {
            // 当前无最优卖价，或新桶价格更低，直接插到头部
            bkt.next = _best_ask;
            if (_best_ask != -1) _sell[_best_ask].prev = idx; // 原头节点prev指向新桶
            _best_ask = idx; // 更新最优卖价索引
        } else {
            // 找到合适的插入位置（保持升序）
            int cur = _best_ask;
            while (_sell[cur].next != -1 && _sell[cur].next < idx)
                cur = _sell[cur].next;
            int nxt = _sell[cur].next;
            bkt.prev = cur;  // 设置新桶的前驱
            bkt.next = nxt;  // 设置新桶的后继
            _sell[cur].next = idx;  // 更新前驱节点的后继
            if (nxt != -1) _sell[nxt].prev = idx;
        }
    }
}

// 从链表中摘除桶
void OrderBook::detachBucket(int idx, bool is_buy)
{
    auto& side = is_buy ? _buy : _sell;
    Bucket& bkt = side[idx];
    // 如果桶不在链表中（prev/next均为-1，且不是头节点），直接返回
    if (bkt.prev == -1 && bkt.next == -1 &&
        idx != (is_buy ? _best_bid : _best_ask))
        return;  // 不在链

    // 处理前驱节点
    if (bkt.prev != -1)
        side[bkt.prev].next = bkt.next;
    else
        // 如果没有前驱，说明是头节点，更新最优价索引
        (is_buy ? _best_bid : _best_ask) = bkt.next;

    // 处理后继节点
    if (bkt.next != -1)
        side[bkt.next].prev = bkt.prev;

    // 清空自身的prev/next指针
    bkt.prev = bkt.next = -1;
}

// 桶增量操作：增加指定桶的总量，并在原本为空时自动挂入链表
void OrderBook::bucketAdd(int idx, bool is_buy, Quantity q)
{
    auto& side = is_buy ? _buy : _sell;      // 选择买方或卖方桶数组
    Bucket& b  = side[idx];                  // 获取目标桶
    bool was0 = (b.vol_sum == 0);            // 记录增量前是否为空桶
    b.vol_sum += q;                          // 增加桶内总量
    if (was0) attachBucket(idx, is_buy);     // 若原本为空，则挂入链表
}

// 桶减量操作：减少指定桶的总量，并在减至0时自动从链表摘除
void OrderBook::bucketSub(int idx, bool is_buy, Quantity q)
{
    auto& side = is_buy ? _buy : _sell;      // 选择买方或卖方桶数组
    Bucket& b  = side[idx];                  // 获取目标桶
    b.vol_sum -= q;                          // 减少桶内总量
    if (b.vol_sum == 0) detachBucket(idx, is_buy); // 若减至0，则从链表摘除
}

//查询最优价
Price OrderBook::bestBid() const { return _best_bid == -1 ? 0 : idxToPx(_best_bid); }
Price OrderBook::bestAsk() const { return _best_ask == -1 ? 0 : idxToPx(_best_ask); }

} // namespace wangcai
