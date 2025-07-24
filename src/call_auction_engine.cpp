// === src/call_auction_engine.cpp ===
#include "../include/call_auction_engine.hpp"
#include <algorithm>
#include <cmath>
#include <iostream> // Added for debugging output
#include <fstream>

namespace wangcai_orderbook_cpp {

// 构造函数：初始化集合竞价引擎，绑定订单簿、前收盘价、交易所、回调等 
CallAuctionEngine::CallAuctionEngine(OrderBook& ob, Price pc,
                                     std::string_view ex, PxCallback px_cb, CancelCallback cancel_cb)
    : ob_(ob), on_px_(std::move(px_cb)), on_cancel_(std::move(cancel_cb)),
      _prev_close(pc), _exch(ex)
{
    // 获取订单簿买盘桶数量，初始化买卖盘树状数组
    int n=int(ob_._buy.size());
    _bit_buy.reset(n); 
    _bit_sell.reset(n);
}

/* 树状数组增量操作：用于集合竞价期间统计买卖盘挂单量
 * idx: 价格桶索引
 * buy: true为买盘，false为卖盘
 * d:   增量（可为负，撤单时用）
 */
inline void CallAuctionEngine::fenwickAdd(int idx,bool buy,int64_t d){
    if(!d) return; // 增量为0直接返回
    if(buy){ 
        _bit_buy.add(idx,d);   // 买盘树状数组加d
        _tot_buy += d;         // 总买量加d
    }
    else   { 
        _bit_sell.add(idx,d);  // 卖盘树状数组加d
        _tot_sell+= d;         // 总卖量加d
    }
}


// 接收新订单：加入集合竞价队列，更新树状数组、订单簿、位置映射 
void CallAuctionEngine::accept(std::shared_ptr<Order> od)
{
    bool buy = od->direction==Direction::Buy;      // 判断买卖方向
    int  idx = ob_.pxToIdx(od->price);             // 价格转桶索引
    
    // 只有限价单才计入树状数组，市价单不计入
    if(od->order_type!=OrderType::Market)
        fenwickAdd(idx,buy,od->volume);

    // 加入订单簿桶链表
    auto& side=buy?ob_._buy:ob_._sell;
    side[idx].orders.push_back(od); // 将订单加入桶链表
    od->level_iter=std::prev(side[idx].orders.end()); // 更新订单迭代器
    ob_.bucketAdd(idx,buy,od->volume); // 更新桶挂单量
    ob_._loc[od->order_id]={buy,idx,od->level_iter}; // 更新订单位置映射
    // 实时发布预测价
    publish();
}

// 撤单：从集合竞价队列和订单簿移除订单，更新树状数组和映射 
void CallAuctionEngine::cancel(uint64_t oid)
{
    // std::cout << "[集合竞价撤单] 系统订单ID=" << oid;
    
    auto it=ob_._loc.find(oid);
    if(it==ob_._loc.end()) {
        // 订单不存在，撤单失败
        std::cout << " -> 失败：订单不存在" << std::endl;
        if(on_cancel_) on_cancel_(oid, false, "订单不存在", nullptr);
        return;
    }
    
    auto loc=it->second;
    auto& side = loc.is_buy?ob_._buy:ob_._sell;
    auto ord=*loc.it;
    int64_t rem=ord->remaining_volume();

    // 树状数组减去剩余未成交量
    fenwickAdd(loc.idx,loc.is_buy,-rem);

    // 从桶链表移除订单
    side[loc.idx].orders.erase(loc.it);
    ob_.bucketSub(loc.idx,loc.is_buy,rem);
    ob_._loc.erase(it);
    
    // 撤单成功回调，传递订单信息
    if(on_cancel_) on_cancel_(oid, true, "撤单成功", ord);
    
    // 实时发布预测价
    publish();
}

// 通过输入订单ID撤单 
void CallAuctionEngine::cancel_by_input_id(uint64_t input_id)
{
    // std::cout << "[集合竞价撤单请求] 输入订单ID=" << input_id;
    
    auto it = ob_.input2sys_.find(input_id);          // 查共享表
    if (it != ob_.input2sys_.end()) {
        // std::cout << " -> 找到系统订单ID=" << it->second << std::endl;
        // 找到对应的系统订单ID，调用标准撤单方法
        cancel(it->second);
        // 从映射中移除
        ob_.input2sys_.erase(it);                     // 从共享表删
        ob_.sys2input_.erase(it->second);             // 从共享表删
        return;
    } else {
        // 输入订单ID不存在
        std::cout << " -> 失败：输入订单ID不存在" << std::endl;
        if (on_cancel_) on_cancel_(input_id, false, "输入订单ID不存在", nullptr);
    }
}

// 计算深圳市场集合竞价成交价 
 Price CallAuctionEngine::calcPredict_SZ()
 {
     // 若买卖盘一方无挂单，直接返回 0
     if (_tot_buy == 0 || _tot_sell == 0) {
         _predict_vol = 0;
         return 0;
     }
 
     const int N = static_cast<int>(ob_._buy.size());
     if (N == 0) {
         _predict_vol = 0;
         return 0;
     }
 
     uint64_t bestVol  = 0;          // 最大可成交量
     uint64_t bestDiff = ~0ULL;      // 买卖剩余量差的最小值
     int       bestIdx = -1;         // 最优价位索引
 
     // 遍历所有价格桶，逐一评估（简单 for-loop 版本）
     for (int idx = 0; idx < N; ++idx) {
        int N = ob_._buy.size();
        uint64_t total_buy = _bit_buy.prefixSum(N-1);
        uint64_t total_sell = _bit_sell.prefixSum(N-1);
        uint64_t upper_buy_vol = (idx < N-1) ? (total_buy - _bit_buy.prefixSum(idx)) : 0;
        uint64_t lower_sell_vol = (idx > 0) ? _bit_sell.prefixSum(idx-1) : 0;
        uint64_t same_price_buy_vol = ob_._buy[idx].vol_sum;
        uint64_t same_price_sell_vol = ob_._sell[idx].vol_sum;
        uint64_t tradable_volume = std::min(lower_sell_vol + same_price_sell_vol, upper_buy_vol + same_price_buy_vol);
        // buy_up <= (sell_down + sell_this) 且 sell_down <= (buy_up + buy_this)
        uint64_t tradable = upper_buy_vol <= (lower_sell_vol + same_price_sell_vol) && lower_sell_vol <= (upper_buy_vol + same_price_buy_vol);
        uint64_t diff = std::llabs(static_cast<int64_t>(upper_buy_vol + same_price_buy_vol - (lower_sell_vol + same_price_sell_vol)));
        if (!tradable) continue;

         // 依据规则 1/2/3 依次比较
         const Price px = ob_.idxToPx(idx);

         const uint64_t dist_prev_close = std::llabs(static_cast<int64_t>(px - _prev_close));

         uint64_t cur_best_dist = (bestIdx == -1) ? ~0ULL :
                                  std::llabs(static_cast<int64_t>(ob_.idxToPx(bestIdx) - _prev_close));
 
         const bool better = (tradable_volume > bestVol) ||
                             (tradable_volume == bestVol && diff < bestDiff) ||
                             (tradable_volume == bestVol && diff == bestDiff && dist_prev_close < cur_best_dist);
 
         if (better) {
             bestVol  = tradable_volume;
             bestDiff = diff;
             bestIdx  = idx;
         }
     }
 
     if (bestIdx == -1) {
         _predict_vol = 0;
         return 0; // 无可撮合价
     }
 
     _predict_vol = bestVol;
     return ob_.idxToPx(bestIdx);
 }

/*
 * Optimised version: iterate only over价位桶里当前“活跃”的索引，而不再
 * 从 0‥N-1 全表扫描。活跃索引集合 _active_idx 在 accept()/cancel()
 * 时维护，因此大部分时间 M ≪ N，复杂度降为 O(M log N)。
 *
 * - accept():   _active_idx.insert(idx)  (若首单进入)
 * - cancel():   若桶清空则 _active_idx.erase(idx)
 *
 * 仍用 Fenwick 取前缀和，每价位两次 prefixSum，
 * 整体复杂度 O(M log N)。在深交所日常盘前场景，M 通常 <500。
 */
Price CallAuctionEngine::calcPredict_SH()
{
    const int N = static_cast<int>(ob_._buy.size());
    if (N == 0) { _predict_vol = 0; return 0; }

    // 构建累计数组（含本价位）
    std::vector<uint64_t> buy_cumu(N + 1, 0);   // 从右往左累加买量
    for (int i = N - 1; i >= 0; --i)
        buy_cumu[i] = buy_cumu[i + 1] + ob_._buy[i].vol_sum;

    std::vector<uint64_t> sell_cumu(N + 1, 0);  // 从左往右累加卖量
    for (int i = 0; i < N; ++i)
        sell_cumu[i + 1] = sell_cumu[i] + ob_._sell[i].vol_sum;

    uint64_t bestVol  = 0;
    uint64_t bestDiff = ~0ULL;
    int      bestIdx  = -1;

    for (int idx = 0; idx < N; ++idx) {
        const uint64_t same_buy  = ob_._buy[idx].vol_sum;
        const uint64_t same_sell = ob_._sell[idx].vol_sum;

        // idx 之上的买量（不含本档）& idx 之下的卖量（不含本档）
        const uint64_t buy_up    = buy_cumu[idx + 1];
        const uint64_t sell_down = sell_cumu[idx];

        // tradable 判断
        const bool tradable = (buy_up <= sell_down + same_sell) &&
                              (sell_down <= buy_up + same_buy);
        if (!tradable) continue;

        const uint64_t tradable_volume = std::min(buy_up + same_buy,
                                                   sell_down + same_sell);
        const uint64_t diff = std::llabs(static_cast<int64_t>((buy_up + same_buy) -
                                                              (sell_down + same_sell)));

        const Price px = ob_.idxToPx(idx);
        const uint64_t dist_prev_close = std::llabs(static_cast<int64_t>(px - _prev_close));
        uint64_t cur_best_dist = (bestIdx == -1) ? ~0ULL :
                                 std::llabs(static_cast<int64_t>(ob_.idxToPx(bestIdx) - _prev_close));

        const bool better = (tradable_volume > bestVol) ||
                            (tradable_volume == bestVol && diff < bestDiff) ||
                            (tradable_volume == bestVol && diff == bestDiff && dist_prev_close < cur_best_dist);

        if (better) {
            bestVol  = tradable_volume;
            bestDiff = diff;
            bestIdx  = idx;
        }
    }

    if (bestIdx == -1) { _predict_vol = 0; return 0; }

    _predict_vol = bestVol;
    return ob_.idxToPx(bestIdx);
}

//发布集合竞价成交价
void CallAuctionEngine::publish()
{
    if(_exch=="SZ") _predict_px=calcPredict_SZ();
    else if(_exch=="SH") _predict_px=calcPredict_SH();
    if(on_px_) on_px_(_predict_px,_predict_vol);
}

// 应用集合竞价撮合结果，撮合成交并更新订单状态
void CallAuctionEngine::applyAuctionTrade(int idx, uint64_t /*bu_tot*/, uint64_t /*sd_tot*/)
{
    Price     open_price  = _predict_px;         // 预测出的开盘价
    Quantity  left_volume = _predict_vol;        // 剩余待撮合量
    if (open_price == 0 || left_volume == 0) return;

    auto log_exec = [&](uint64_t buy_sys, uint64_t sell_sys, Quantity q)
    {
        uint64_t buy_input  = ob_.sys2input_.count(buy_sys)  ? ob_.sys2input_[buy_sys]  : 0;
        uint64_t sell_input = ob_.sys2input_.count(sell_sys) ? ob_.sys2input_[sell_sys] : 0;

        if (ob_._on_exec)
            ob_._on_exec(Execution(buy_input, sell_input, open_price, q));
    };

    /* === 1. 先取 >= 开盘价的买单，按交易所规则排序 === */
    std::vector<std::shared_ptr<Order>> buy_q;
    for (int i = ob_._buy.size() - 1; i >= idx; --i) {          // 价格递减
        // 收集同价格的订单
        std::vector<std::shared_ptr<Order>> same_price_orders;
        for (auto& od : ob_._buy[i].orders) {
            if (od->status == OrderStatus::Submitted || od->status == OrderStatus::PartFilled) {
                same_price_orders.push_back(od);
            }
        }
        
        // 根据交易所类型排序同价格订单
        if (_exch == "SZ") {
            // 深圳：按orderid排序（时间优先）
            std::sort(same_price_orders.begin(), same_price_orders.end(),
                [](const std::shared_ptr<Order>& a, const std::shared_ptr<Order>& b) {
                    uint64_t a_id = std::stoull(a->order_local_id);
                    uint64_t b_id = std::stoull(b->order_local_id);
                    return a_id < b_id;
                });
        } else {
            // 上海：按bizindex排序（时间优先）
            std::sort(same_price_orders.begin(), same_price_orders.end(),
                [](const std::shared_ptr<Order>& a, const std::shared_ptr<Order>& b) {
                    return a->bizindex < b->bizindex;
                });
        }
        
        // 添加到买单队列
        for (auto& od : same_price_orders) {
            buy_q.push_back(od);
        }
    }

    /* === 2. 再取 <= 开盘价的卖单，按交易所规则排序 === */
    std::vector<std::shared_ptr<Order>> sell_q;
    for (int i = 0; i <= idx; ++i) {                            // 价格递增
        // 收集同价格的订单
        std::vector<std::shared_ptr<Order>> same_price_orders;
        for (auto& od : ob_._sell[i].orders) {
            if (od->status == OrderStatus::Submitted || od->status == OrderStatus::PartFilled) {
                same_price_orders.push_back(od);
            }
        }
        
        // 根据交易所类型排序同价格订单
        if (_exch == "SZ") {
            // 深圳：按orderid排序（时间优先）
            std::sort(same_price_orders.begin(), same_price_orders.end(),
                [](const std::shared_ptr<Order>& a, const std::shared_ptr<Order>& b) {
                    uint64_t a_id = std::stoull(a->order_local_id);
                    uint64_t b_id = std::stoull(b->order_local_id);
                    return a_id < b_id;
                });
        } else {
            // 上海：按bizindex排序（时间优先）
            std::sort(same_price_orders.begin(), same_price_orders.end(),
                [](const std::shared_ptr<Order>& a, const std::shared_ptr<Order>& b) {
                    return a->bizindex < b->bizindex;
                });
        }
        
        // 添加到卖单队列
        for (auto& od : same_price_orders) {
            sell_q.push_back(od);
        }
    }

    // 撮合逻辑保持不变
    size_t bi = 0, si = 0;
    while (left_volume > 0 && bi < buy_q.size() && si < sell_q.size())
    {
        auto  buy  = buy_q [bi];
        auto  sell = sell_q[si];

        Quantity trade_qty = std::min({ buy->remaining_volume(),
                                        sell->remaining_volume(),
                                        left_volume                         });

        /* 执行成交 */
        buy ->traded_volume  += trade_qty;
        sell->traded_volume += trade_qty;
        left_volume          -= trade_qty;
        ob_.bucketSub(ob_.pxToIdx(buy ->price), true , trade_qty);
        ob_.bucketSub(ob_.pxToIdx(sell->price), false, trade_qty);

        buy ->status  = (buy ->remaining_volume()  == 0) ? OrderStatus::Filled : OrderStatus::PartFilled;
        sell->status = (sell->remaining_volume() == 0) ? OrderStatus::Filled : OrderStatus::PartFilled;

        log_exec(buy->order_id, sell->order_id, trade_qty);

        if (buy ->remaining_volume() == 0) ++bi;
        if (sell->remaining_volume() == 0) ++si;
    }
    /* 剩余 buy_q / sell_q 自动留在桶里，进入连续竞价 */
}

// 结算：集合竞价结束，撮合成交，清空树状数组和累计量
void CallAuctionEngine::settle()
{
    // 先计算最终成交价
    if(_exch=="SZ") _predict_px=calcPredict_SZ();
    else if(_exch=="SH") _predict_px=calcPredict_SH();
    Price px = _predict_px; // 最终成交价
    if(px == 0) {
        // 无法确定集合竞价成交价，直接清空并返回
        _bit_buy=Fenwick{}; 
        _bit_sell=Fenwick{};
        _tot_buy=_tot_sell=0;
        return;
    }
    int idx=ob_.pxToIdx(px);
    
    // 应用撮合
    applyAuctionTrade(idx,_tot_buy,_tot_sell);
    
    // 清空树状数组和累计量
    _bit_buy=Fenwick{}; 
    _bit_sell=Fenwick{};
    _tot_buy=_tot_sell=0;
}

// 增加一个调试函数，打印出当时订单簿的挂单情况到csv
void CallAuctionEngine::print_orderbook_to_csv(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "无法打开文件: " << filename << std::endl;
        return;
    }
    
    // 写入表头
    file << "price,buy_orders_volume,sell_orders_volume, upper_buy_vol, lower_sell_vol, same_price_buy_vol, same_price_sell_vol, tradable_volume, tradable\n";
    
    int N = ob_._buy.size();
    uint64_t total_buy = _bit_buy.prefixSum(N-1);
    uint64_t total_sell = _bit_sell.prefixSum(N-1);

    for (int i = 0; i < N; ++i) {
        uint64_t upper_buy_vol = (i < N-1) ? (total_buy - _bit_buy.prefixSum(i)) : 0;
        uint64_t lower_sell_vol = (i > 0) ? _bit_sell.prefixSum(i-1) : 0;
        uint64_t same_price_buy_vol = ob_._buy[i].vol_sum;
        uint64_t same_price_sell_vol = ob_._sell[i].vol_sum;
        uint64_t tradable_volume = std::min(lower_sell_vol + same_price_sell_vol, upper_buy_vol + same_price_buy_vol);
        // buy_up <= (sell_down + sell_this) 且 sell_down <= (buy_up + buy_this)
        uint64_t tradable = upper_buy_vol <= (lower_sell_vol + same_price_sell_vol) && lower_sell_vol <= (upper_buy_vol + same_price_buy_vol);
        file << ob_.idxToPx(i) << ","
             << same_price_buy_vol << ","
             << same_price_sell_vol << ","
             << upper_buy_vol << ","
             << lower_sell_vol << ","
             << same_price_buy_vol << ","
             << same_price_sell_vol << ","
             << tradable_volume << ","
             << tradable << "\n";
    }
}
} // namespace wangcai_orderbook_cpp 