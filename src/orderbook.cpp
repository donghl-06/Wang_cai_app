/*
 * @Author: linzhuoyu
 * @Date: 2025-07-07 08:43:04
 * @LastEditTime: 2025-07-08 06:01:00
 * @FilePath: /wangcai_orderbook_cpp/src/orderbook.cpp
 */

#include "../include/orderbook.h"
#include "../include/orderpool.h"   // 对象池头文件

#include <cassert>
#include <algorithm>    // std::max / std::min
#include <iostream>     // std::cout
#include <cmath>        // std::round, std::abs
#include <vector>       // std::vector
#include <cstdlib>      // std::abs

namespace wangcai_orderbook_cpp {

//构造函数
OrderBook::OrderBook(double hi, double lo, bool is_etf, ExecCallback cb)
    : _on_exec(std::move(cb))
{   
    if (hi < lo) throw std::invalid_argument("hi 必须大于 lo");
    
    // 1. 最小价格单位 tick（整数价 1 = 0.0001 元）
    _tick  = is_etf ? 10 : 100;  // ETF = 0.001 元；股票 = 0.01 元

    // 2. 涨跌停区间（整型价格）
    _lower = static_cast<Price>(lo * 10'000);
    _upper = static_cast<Price>(hi * 10'000); 

    // 3. 预分配桶数组
    const int bucket_cnt = (_upper - _lower) / _tick + 1; // 桶数 = (最大价格 - 最小价格) / 最小价格单位 + 1
    _buy .resize(bucket_cnt); // 买盘桶
    _sell.resize(bucket_cnt); // 卖盘桶

    _best_bid = _best_ask = -1; //最优价索引
    _prev_close_price = 0; // 前收盘价
}

//链表维护
inline void OrderBook::attachBucket(int idx, bool is_buy)//挂入桶
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
inline void OrderBook::detachBucket(int idx, bool is_buy)
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
inline void OrderBook::bucketAdd(int idx, bool is_buy, Quantity q)
{
    auto& side = is_buy ? _buy : _sell;      // 选择买方或卖方桶数组
    Bucket& b  = side[idx];                  // 获取目标桶
    bool was0 = (b.vol_sum == 0);            // 记录增量前是否为空桶
    b.vol_sum += q;                          // 增加桶内总量
    if (was0) attachBucket(idx, is_buy);     // 若原本为空，则挂入链表
}

// 桶减量操作：减少指定桶的总量，并在减至0时自动从链表摘除
inline void OrderBook::bucketSub(int idx, bool is_buy, Quantity q)
{
    auto& side = is_buy ? _buy : _sell;      // 选择买方或卖方桶数组
    Bucket& b  = side[idx];                  // 获取目标桶
    b.vol_sum -= q;                          // 减少桶内总量
    if (b.vol_sum == 0) detachBucket(idx, is_buy); // 若减至0，则从链表摘除
}

//下单入口
void OrderBook::addOrder(std::shared_ptr<Order> ord)
{
    // 建立映射索引
    _omap[ord->order_id] = ord;

    // 市价单：直接撮合，余量撤单
    if (ord->order_type == OrderType::Market) {
        matchIncoming(ord);
        if (ord->remaining_volume() > 0) ord->status = OrderStatus::Cancelled; //如果市价单未成交，则撤单
        return;
    }

    // 限价 / 对手价 / 本方价：先撮合
    matchIncoming(ord);

    // 全部成交直接返回
    if (ord->remaining_volume() == 0) return;

    // 剩余量挂入本方桶
    bool is_buy = ord->direction == Direction::Buy;
    int idx = pxToIdx(ord->price);
    Bucket&   bkt = (is_buy ? _buy : _sell)[idx];

    bkt.orders.push_back(ord); // 将订单挂入本方桶
    ord->level_iter = std::prev(bkt.orders.end()); // 关键：初始化 level_iter
    bucketAdd(idx, is_buy, ord->remaining_volume()); // 更新本方桶

    _loc[ord->order_id] = {is_buy, idx, ord->level_iter}; // 维护订单位置
}

//撤单入口
bool OrderBook::cancel(uint64_t oid)
{
    // 获取订单位置
    auto it = _loc.find(oid);
    if (it == _loc.end()) return false;  // 未找到订单
    const Locator& loc = it->second;
    
    // 获取订单信息
    auto& side = loc.is_buy ? _buy : _sell; // 根据订单方向选择买/卖桶
    Bucket& bkt = side[loc.idx]; // 获取订单所在桶
    auto   iter = loc.it; // 获取订单位置
    std::shared_ptr<Order> ord = *iter;

    // 如果订单已成交或已撤销，则返回false
    if (ord->status == OrderStatus::Filled ||
        ord->status == OrderStatus::Cancelled) return false; 

    Quantity remaining_volume = ord->remaining_volume();
    bkt.orders.erase(iter); // 从链表中移除该订单
    // 更新桶的剩余量
    bucketSub(loc.idx, loc.is_buy, remaining_volume); 

    // 更新订单状态
    ord->status = OrderStatus::Cancelled; 
    _omap.erase(oid); // 移除索引；shared_ptr 引用 ‑‑, 自动归还池
    _loc.erase(it); // 移除订单位置
    return true;  // 撤单成功
}

//查询最优价
Price OrderBook::bestBid() const { return _best_bid == -1 ? 0 : idxToPx(_best_bid); }
Price OrderBook::bestAsk() const { return _best_ask == -1 ? 0 : idxToPx(_best_ask); }

// 撮合核心函数：处理传入订单与对手方订单的撮合逻辑
void OrderBook::matchIncoming(std::shared_ptr<Order>& inc)
{
    //获取订单信息
    bool inc_buy = (inc->direction == Direction::Buy); // 判断传入订单方向（买/卖）
    auto& opp_side = inc_buy ? _sell : _buy;  // 选择对手方桶数组（买单撮合卖方，卖单撮合买方）

    // 选择对手方最优价索引的引用（买单对应卖方最优价，卖单对应买方最优价）
    int&  opp_best = inc_buy ? _best_ask : _best_bid;

    // 只要传入订单还有剩余量，且对手方有可撮合的价格档
    while (inc->remaining_volume() > 0 && opp_best != -1) {
        // 获取当前对手方最优价
        Price opp_px = idxToPx(opp_best);

        // 判断价格是否可成交（买单价格低于卖方最优价，或卖单价格高于买方最优价则停止撮合）
        if ((inc_buy && inc->price < opp_px) ||
            (!inc_buy && inc->price > opp_px)) break;

        // 获取对手方当前最优价桶
        Bucket& bkt = opp_side[opp_best];

        // 在该价格档内，逐个撮合订单（时间优先，FIFO）
        while (inc->remaining_volume() > 0 && !bkt.orders.empty()) {
            // 取出对手方队首订单
            auto   oppo = bkt.orders.front();

            // 计算本次可成交数量（取双方剩余量的较小值）
            Quantity qty = std::min(inc->remaining_volume(), oppo->remaining_volume());

            // 更新双方已成交数量
            inc ->traded_volume  += qty;
            oppo->traded_volume += qty;

            // 更新对手方桶的剩余量（自动维护链表）
            bucketSub(opp_best, !inc_buy, qty);

            // 如果设置了成交回调，则回调成交信息
            if (_on_exec) {
                // 构造成交记录（买方订单ID，卖方订单ID，成交价，成交量）
                Execution ex(inc_buy ? inc->order_id : oppo->order_id,
                             inc_buy ? oppo->order_id : inc->order_id,
                             opp_px, qty);
                _on_exec(ex);
            }

            // 如果对手方订单已全部成交
            if (oppo->remaining_volume() == 0) {
                oppo->status = OrderStatus::Filled;   // 标记为已成交
                bkt.orders.pop_front();               // 从桶链表移除
                if (_loc.count(oppo->order_id)) _loc.erase(oppo->order_id);
                if (_omap.count(oppo->order_id)) _omap.erase(oppo->order_id);
            } else {
                oppo->status = OrderStatus::PartFilled; // 部分成交
            }
        }

        // 跳转到下一个非空对手方价格档（detachBucket已自动维护链表）
        opp_best = inc_buy ? _best_ask : _best_bid;
    }

    // 根据撮合结果更新传入订单状态
    if (inc->remaining_volume() == 0)
        inc->status = OrderStatus::Filled;         // 全部成交
    else if (inc->traded_volume > 0)
        inc->status = OrderStatus::PartFilled;     // 部分成交
    else
        inc->status = OrderStatus::Submitted;      // 未成交

    // 如果传入订单已全部成交，则从映射表中移除
    if (inc->status == OrderStatus::Filled) {
        if (_omap.count(inc->order_id)) _omap.erase(inc->order_id);
        if (_loc.count(inc->order_id)) _loc.erase(inc->order_id);
    }
}

} // namespace wangcai_orderbook_cpp