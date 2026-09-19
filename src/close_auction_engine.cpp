// === src/close_auction_engine.cpp ===
#include "../include/close_auction_engine.hpp"
#include <algorithm>
#include <cmath>
#include <iostream> // Added for debugging output
#include <fstream>

namespace wangcai {

// 构造函数实现
CloseAuctionEngine::CloseAuctionEngine(OrderBook& ob, Price pc,
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
inline void CloseAuctionEngine::fenwickAdd(int idx,bool buy,int64_t d){
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
void CloseAuctionEngine::accept(std::shared_ptr<Order> od)
{
    // 簿外价历史限价委托(无涨跌幅日恶作剧天价/地板价,论证见 OrderLoader.cpp
    // loadPriceLimits):只延伸累计曲线端点、不改变界内清算价,登记簿外价
    // 吸收表后不入簿,后续真实撤单记录由分发层凭表吸收为 no-op。
    if (od->is_historical && od->order_type == OrderType::Limit &&
        !ob_.inBookRange(od->price)) {
        ob_.markOutOfBookAbsorbed(od->order_id);
        return;
    }
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
    // 预测价改惰性计算:仅置脏标记,读取(getPredictPrice/settle)时才重算
    _predict_dirty = true;
}

// 撤单：从集合竞价队列和订单簿移除订单，更新树状数组和映射 
void CloseAuctionEngine::cancel(uint64_t oid)
{
    // std::cout << "[集合竞价撤单] 系统订单ID=" << oid;
    
    auto it=ob_._loc.find(oid);
    if(it==ob_._loc.end()) {
        // 订单不存在，撤单失败
        uint64_t cstra_id = ob_.getInputId(oid);
        std::cerr << "❌ [收盘集合竞价撤单失败] cstra_id=" << cstra_id << " -> 订单不存在" << std::endl;
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

    // 预测价改惰性计算:仅置脏标记
    _predict_dirty = true;
}

// 通过输入订单ID撤单 
void CloseAuctionEngine::cancel_by_input_id(uint64_t input_id, int channel_no)
{
    // std::cout << "[集合竞价撤单请求] 输入订单ID=" << input_id;
    
    auto system_id = ob_.findSystemOrderId(input_id, channel_no);
    if (system_id.has_value()) {
        uint64_t sys_id = *system_id;
        cancel(sys_id);
        ob_.eraseActiveMarketOrder(sys_id);
        return;
    } else {
        // 输入订单ID不存在（未在活跃市场身份映射中找到）
        std::cerr << "❌ [收盘集合竞价撤单失败] channel=" << channel_no
                  << " 输入订单ID=" << input_id << " -> 市场身份映射中不存在" << std::endl;
        if (on_cancel_) on_cancel_(input_id, false, "输入订单ID不存在", nullptr);
    }
}

// 计算深圳市场集合竞价成交价（深交所规则）
// 返回预测成交价，并设置_predict_vol为最大可成交量
Price CloseAuctionEngine::calcPredict_SZ() const
{
    // 若买卖盘一方无挂单，直接返回0，表示无法成交
    if (_tot_buy == 0 || _tot_sell == 0) {
        _predict_vol = 0;
        return 0;
    }

    // 获取价格桶数量（价位档数）
    const int N = static_cast<int>(ob_._buy.size());
    if (N == 0) {
        _predict_vol = 0;
        return 0;
    }

    uint64_t bestVol  = 0;         // 当前最大可成交量
    uint64_t bestDiff = ~0ULL;     // 当前最小买卖剩余量差
    int      bestIdx  = -1;        // 当前最优价位索引

    // 全市场买卖总量是循环不变量,提出循环只算一次
    // (原实现在循环体内每个桶重算两次全量 prefixSum,纯属浪费)
    const uint64_t total_buy  = _bit_buy.prefixSum(N - 1);   // 买盘总量
    const uint64_t total_sell = _bit_sell.prefixSum(N - 1);  // 卖盘总量

    // 遍历所有价格桶，逐一评估每个价位作为成交价的可行性
    for (int idx = 0; idx < N; ++idx) {
        // 计算idx价位之上的买量（不含本档），即高于当前价的买单总量
        uint64_t upper_buy_vol = (idx < N - 1) ? (total_buy - _bit_buy.prefixSum(idx)) : 0;
        // 计算idx价位之下的卖量（不含本档），即低于当前价的卖单总量
        uint64_t lower_sell_vol = (idx > 0) ? _bit_sell.prefixSum(idx - 1) : 0;

        // 当前价位的买卖挂单量
        uint64_t same_price_buy_vol  = ob_._buy[idx].vol_sum;
        uint64_t same_price_sell_vol = ob_._sell[idx].vol_sum;

        // 计算该价位下理论最大可成交量
        uint64_t tradable_volume = std::min(lower_sell_vol + same_price_sell_vol,
                                            upper_buy_vol + same_price_buy_vol);

        // 判断该价位是否满足可成交条件（深交所集合竞价规则）
        // buy_up <= (sell_down + sell_this) 且 sell_down <= (buy_up + buy_this)
        uint64_t tradable = upper_buy_vol <= (lower_sell_vol + same_price_sell_vol) &&
                            lower_sell_vol <= (upper_buy_vol + same_price_buy_vol);

        // 计算买卖剩余量差的绝对值
        uint64_t diff = std::llabs(static_cast<int64_t>(
            upper_buy_vol + same_price_buy_vol - (lower_sell_vol + same_price_sell_vol)));

        // 若不可成交，跳过本价位
        if (!tradable) continue;

        // 获取当前价位对应的价格
        const Price px = ob_.idxToPx(idx);

        // 参考价：优先用最新成交价，否则用前收盘价
        Price ref_px = ob_.getLastTradePrice() != 0 ? ob_.getLastTradePrice() : _prev_close;
        // 计算当前价与参考价的距离
        const uint64_t dist_prev_close = std::llabs(static_cast<int64_t>(px - ref_px));

        // 当前最优价与参考价的距离
        uint64_t cur_best_dist = (bestIdx == -1) ? ~0ULL :
                                 std::llabs(static_cast<int64_t>(ob_.idxToPx(bestIdx) - ref_px));

        // 按规则1/2/3依次比较，选出最优成交价
        // 1. 最大可成交量优先
        // 2. 可成交量相等时，买卖剩余量差最小优先
        // 3. 仍相等时，距离参考价最近优先
        const bool better = (tradable_volume > bestVol) ||
                            (tradable_volume == bestVol && diff < bestDiff) ||
                            (tradable_volume == bestVol && diff == bestDiff && dist_prev_close < cur_best_dist);

        if (better) {
            bestVol  = tradable_volume;
            bestDiff = diff;
            bestIdx  = idx;
        }
    }

    // 若未找到可成交价，返回0
    if (bestIdx == -1) {
        _predict_vol = 0;
        return 0;
    }

    // 设置最大可成交量
    _predict_vol = bestVol;
    // 返回最优价位对应的价格
    return ob_.idxToPx(bestIdx);
}

/*
 * 上交所集合竞价预测价:两个累计数组 + O(N) 全表扫描。
 * 注:此处旧注释声称有 _active_idx 活跃集优化(O(M log N)),该优化从未实现,
 * 注释与实现不符,已于 2026-09-15 更正。真正的性能修复是调用侧惰性化:
 * accept/cancel 不再逐事件 publish,仅置脏标记,读取时才重算(ensurePredict)。
 */

// 计算上海市场集合竞价成交价（上交所规则）
// 返回预测成交价，并设置_predict_vol为最大可成交量
Price CloseAuctionEngine::calcPredict_SH() const
{
    const int N = static_cast<int>(ob_._buy.size());
    if (N == 0) {
        _predict_vol = 0;
        return 0;
    }

    // 构建买量累计数组（buy_cumu[i]表示从i及更高价位的买量总和，含本档）
    // 复用成员缓冲,不再每次堆分配
    _sh_buy_cumu.assign(N + 1, 0);
    auto& buy_cumu = _sh_buy_cumu;
    for (int i = N - 1; i >= 0; --i)
        buy_cumu[i] = buy_cumu[i + 1] + ob_._buy[i].vol_sum;

    // 构建卖量累计数组（sell_cumu[i]表示从0到i-1价位的卖量总和，不含本档）
    _sh_sell_cumu.assign(N + 1, 0);
    auto& sell_cumu = _sh_sell_cumu;
    for (int i = 0; i < N; ++i)
        sell_cumu[i + 1] = sell_cumu[i] + ob_._sell[i].vol_sum;

    uint64_t bestVol  = 0;         // 当前最大可成交量
    uint64_t bestDiff = ~0ULL;     // 当前最小买卖剩余量差
    int      bestIdx  = -1;        // 当前最优价位索引
    auto& tradable_prices = _sh_tradable_prices;  // 并列最优的潜在成交价（用于后续均价计算）
    tradable_prices.clear();

    // 遍历所有价位，评估每个价位作为成交价的可行性
    for (int idx = 0; idx < N; ++idx) {
        // 当前价位的买卖挂单量
        const uint64_t same_buy  = ob_._buy[idx].vol_sum;
        const uint64_t same_sell = ob_._sell[idx].vol_sum;

        // idx之上的买量（不含本档）和idx之下的卖量（不含本档）
        const uint64_t buy_up    = buy_cumu[idx + 1];
        const uint64_t sell_down = sell_cumu[idx];

        // 判断该价位是否满足可成交条件（上交所集合竞价规则）
        // buy_up <= (sell_down + same_sell) 且 sell_down <= (buy_up + same_buy)
        const bool tradable = (buy_up <= sell_down + same_sell) &&
                              (sell_down <= buy_up + same_buy);
        if (!tradable) continue;

        // 计算该价位下理论最大可成交量
        const uint64_t tradable_volume = std::min(buy_up + same_buy,
                                                  sell_down + same_sell);

        // 计算买卖剩余量差的绝对值
        const uint64_t diff = std::llabs(static_cast<int64_t>(
            (buy_up + same_buy) - (sell_down + same_sell)));

        // 获取当前价位对应的价格
        const Price px = ob_.idxToPx(idx);

        // 潜在成交价：成交量优先；成交量相同时未成交量小者优先
        const bool better = (tradable_volume > bestVol) ||
                            (tradable_volume == bestVol && diff < bestDiff);

        if (better) {
            bestVol  = tradable_volume;
            bestDiff = diff;
            bestIdx  = idx;
            tradable_prices.clear();        // 出现更优价位，先前记录的全部作废
            tradable_prices.push_back(px);
        } else if (tradable_volume == bestVol && diff == bestDiff) {
            tradable_prices.push_back(px);  // 与当前最优完全并列，一并保留
        }
    }

    // 若未找到可成交价，返回0
    if (bestIdx == -1) {
        _predict_vol = 0;
        return 0;
    }

    // 设置最大可成交量
    _predict_vol = bestVol;

    // 并列的潜在成交价取均价（四舍五入到tick）
    if (!tradable_prices.empty()) {
        double sum = 0;
        for (Price p : tradable_prices) {
            sum += p;
        }
        double avg = sum / tradable_prices.size();
        // 四舍五入到tick（ETF=10厘, 股票=100厘）
        Price tick = ob_.getTick();
        Price rounded = static_cast<Price>(std::round(avg / tick) * tick);
        return rounded;
    }

    // 若无可成交价，返回最大成交量对应的价格
    return ob_.idxToPx(bestIdx);
}

// 惰性重算预测价/量:accept/cancel/bootstrap 只置脏标记,此处按需计算
void CloseAuctionEngine::ensurePredict() const
{
    if (!_predict_dirty) return;
    if(_exch=="SZ") _predict_px=calcPredict_SZ();
    else if(_exch=="SH") _predict_px=calcPredict_SH();
    _predict_dirty = false;
}

//发布集合竞价成交价
void CloseAuctionEngine::publish()
{
    ensurePredict();
    if(on_px_) on_px_(_predict_px,_predict_vol);
}

// 应用集合竞价撮合结果，撮合成交并更新订单状态
void CloseAuctionEngine::applyAuctionTrade(int idx, uint64_t /*bu_tot*/, uint64_t /*sd_tot*/)
{
    Price     open_price  = _predict_px;         // 预测出的开盘价
    Quantity  left_volume = _predict_vol;        // 剩余待撮合量
    if (open_price == 0 || left_volume == 0) return;

    auto log_exec = [&](uint64_t buy_sys, uint64_t sell_sys, Quantity q)
    {
        if (ob_._on_exec)
            ob_._on_exec(Execution::historical(buy_sys, sell_sys, open_price, q));
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

        if (buy->remaining_volume() == 0) ob_.eraseActiveMarketOrder(buy->order_id);
        if (sell->remaining_volume() == 0) ob_.eraseActiveMarketOrder(sell->order_id);

        if (buy ->remaining_volume() == 0) ++bi;
        if (sell->remaining_volume() == 0) ++si;
    }
    /* 剩余 buy_q / sell_q 自动留在桶里，进入连续竞价 */
}

// 结算：集合竞价结束，撮合成交，清空树状数组和累计量
void CloseAuctionEngine::settle()
{
    // 先计算最终成交价
    if(_exch=="SZ") _predict_px=calcPredict_SZ();
    else if(_exch=="SH") _predict_px=calcPredict_SH();
    _predict_dirty = false; // settle 已是最新计算,同步脏标记
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

// 从订单簿中获取挂单量，用于集合竞价结算
void CloseAuctionEngine::bootstrap_from_orderbook() {
    int N = ob_._buy.size();
    _bit_buy.reset(N);
    _bit_sell.reset(N);
    _tot_buy = _tot_sell = 0;

    for(int i=0;i<N;++i) {
        if(ob_._buy[i].vol_sum>0) {
            fenwickAdd(i,true,  ob_._buy[i].vol_sum);
        }
        if(ob_._sell[i].vol_sum>0) {
            fenwickAdd(i,false, ob_._sell[i].vol_sum);
        }
    }
    _predict_dirty = true; // 账面已重建,预测价惰性到读取时再算
}
} // namespace wangcai
