// === src/call_auction_engine.cpp ===
#include "../include/call_auction_engine.hpp"
#include <algorithm>
#include <cmath>
#include <iostream> // Added for debugging output

namespace wangcai_orderbook_cpp {

/* 构造函数：初始化集合竞价引擎，绑定订单簿、前收盘价、交易所、回调等 */
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

/* 接收新订单：加入集合竞价队列，更新树状数组、订单簿、位置映射 */
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

    // 如果订单的本地ID是数字，建立输入ID到系统ID的映射
    try {
        uint64_t input_id = std::stoull(od->order_local_id);
        _input_id_to_system_id[input_id] = od->order_id;
    } catch (const std::exception&) {
        std::cout << "输入订单ID不是数字" << std::endl;
        // 如果本地ID不是数字，忽略
    }

    // 实时发布预测价
    publish();
}

/* 撤单：从集合竞价队列和订单簿移除订单，更新树状数组和映射 */
void CallAuctionEngine::cancel(uint64_t oid)
{
    auto it=ob_._loc.find(oid);
    if(it==ob_._loc.end()) {
        // 订单不存在，撤单失败
        if(on_cancel_) on_cancel_(oid, false, "订单不存在");
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
    
    // 撤单成功回调
    if(on_cancel_) on_cancel_(oid, true, "撤单成功");
    
    // 实时发布预测价
    publish();
}

/* 通过输入订单ID撤单 */
void CallAuctionEngine::cancel_by_input_id(uint64_t input_id)
{
    auto it = _input_id_to_system_id.find(input_id);
    if (it != _input_id_to_system_id.end()) {
        // 找到对应的系统订单ID，调用标准撤单方法
        cancel(it->second);
        // 从映射中移除
        _input_id_to_system_id.erase(it);
    } else {
        // 输入订单ID不存在
        if (on_cancel_) on_cancel_(input_id, false, "输入订单ID不存在");
    }
}

/* 预测价计算：实时计算当前集合竞价的理论成交价和成交量 */
Price CallAuctionEngine::calcPredict()
{
    // 若买卖盘一方无挂单，预测成交量为0，价格为0
    if(_tot_buy==0||_tot_sell==0){ 
        _predict_vol=0; 
        return 0; 
    }
    int N=int(ob_._buy.size());
    if(N==0) {
        _predict_vol=0;
        return 0;
    }
    
    // 寻找有效的搜索范围（有买单或卖单的区间）
    int min_idx = N, max_idx = -1;
    for(int i = 0; i < N; i++) {
        if(ob_._buy[i].vol_sum > 0 || ob_._sell[i].vol_sum > 0) {
            min_idx = std::min(min_idx, i);
            max_idx = std::max(max_idx, i);
        }
    }
    
    if(min_idx > max_idx) {
        _predict_vol = 0;
        return 0;
    }
    
    int lo=min_idx,hi=max_idx,meet=-1;

    // 二分查找"买卖量相遇点"索引
    while(lo<=hi){
        int mid=(lo+hi)>>1;
        int64_t bu=_tot_buy - _bit_buy.prefixSum(mid-1);   // mid及以上买量
        int64_t sd=_bit_sell.prefixSum(mid);               // mid及以下卖量
        
        // 若买量大于等于卖量，说明成交价可能更低，向左收缩
        if(bu>=sd) {
            meet=mid;
            hi=mid-1;
        } else {
            lo=mid+1;
        }
    }
    
    if(meet==-1) {
        _predict_vol=0;
        return 0;
    }

    // 遍历所有有效价格桶，寻找最优成交价（而不是固定窗口）
    uint64_t bestVol=0;         // 最大可成交量
    uint64_t bestDiff=~0ULL;    // 买卖剩余量绝对差最小
    int bestIdx=meet;           // 最优价格索引

    // 用于评估每个价格idx的成交情况
    auto eval=[&](int idx){
        if(idx<0||idx>=N) return;
        // 计算idx及以上买量
        uint64_t bu=_tot_buy - _bit_buy.prefixSum(idx-1);
        // 计算idx及以下卖量
        uint64_t sd=_bit_sell.prefixSum(idx);
        // 当前价位可成交量 = min(买方累计+本桶, 卖方累计+本桶)
        uint64_t tv=std::min(bu+ob_._buy[idx].vol_sum,
                             sd+ob_._sell[idx].vol_sum);
        // 买卖剩余量绝对值
        uint64_t diff=std::llabs(int64_t(
            (bu+ob_._buy[idx].vol_sum) - (sd+ob_._sell[idx].vol_sum)));
        // 是否满足集合竞价撮合条件
        bool ok=(bu<=sd+ob_._sell[idx].vol_sum)&&
                (sd<=bu+ob_._buy[idx].vol_sum);
        
        if(!ok) return;
        // 优先最大成交量，其次最小剩余量
        if(tv>bestVol||(tv==bestVol&&diff<bestDiff)){
            bestVol=tv; bestDiff=diff; bestIdx=idx;
        }
        // 深交所平分时靠近前收盘价优先
        else if(tv==bestVol&&diff==bestDiff&&_exch=="SZ"){
            if(std::llabs(ob_.idxToPx(idx)-_prev_close)<
               std::llabs(ob_.idxToPx(bestIdx)-_prev_close))
               bestIdx=idx;
        }
    };
    
    // 遍历所有有效价格桶（从min_idx到max_idx），确保不遗漏任何可能的最优价格
    for(int i=min_idx;i<=max_idx;++i) eval(i);

    // 上交所平分时取中位价
    if(_exch=="SH"){
        std::vector<int> cand;
        for(int i=min_idx;i<=max_idx;++i){
            if(i<0||i>=N) continue;
            uint64_t bu=_tot_buy - _bit_buy.prefixSum(i-1);
            uint64_t sd=_bit_sell.prefixSum(i);
            uint64_t tv=std::min(bu+ob_._buy[i].vol_sum,
                                 sd+ob_._sell[i].vol_sum);
            uint64_t diff=std::llabs(int64_t(
              (bu+ob_._buy[i].vol_sum)-(sd+ob_._sell[i].vol_sum)));
            bool ok=(bu<=sd+ob_._sell[i].vol_sum)&&
                    (sd<=bu+ob_._buy[i].vol_sum);
            if(ok&&tv==bestVol&&diff==bestDiff) cand.push_back(i);
        }
        if(!cand.empty()){ 
            std::sort(cand.begin(),cand.end());
            bestIdx=cand[cand.size()/2]; // 取中位
        }
    }
    
    _predict_vol=bestVol;
    return ob_.idxToPx(bestIdx);
}

/* 实时发布预测价和预测成交量 */
void CallAuctionEngine::publish()
{
    _predict_px=calcPredict();
    if(on_px_) on_px_(_predict_px,_predict_vol);
}

/* 应用集合竞价撮合结果，撮合成交并更新订单状态 */
void CallAuctionEngine::applyAuctionTrade(int idx,uint64_t bu_tot,uint64_t sd_tot)
{
    // 1. 全额成交区：高于成交价的买单、低于成交价的卖单全部成交
    for(int i=idx+1;i<int(ob_._buy.size());++i){
        auto& b=ob_._buy[i];
        while(!b.orders.empty()){
            auto od=b.orders.front();
            uint64_t q=od->remaining_volume();
            od->traded_volume+=q; 
            od->status=OrderStatus::Filled;
            ob_.bucketSub(i,true,q); 
            b.orders.pop_front();
        }
    }
    for(int i=0;i<idx;++i){
        auto& b=ob_._sell[i];
        while(!b.orders.empty()){
            auto od=b.orders.front();
            uint64_t q=od->remaining_volume();
            od->traded_volume+=q; 
            od->status=OrderStatus::Filled;
            ob_.bucketSub(i,false,q); 
            b.orders.pop_front();
        }
    }

    // 2. 本价桶撮合：买卖双方剩余量依次撮合，部分成交/全成
    auto& buy_bkt = ob_._buy[idx];
    auto& sell_bkt= ob_._sell[idx];
    // 计算本价桶买卖剩余量
    uint64_t bu = bu_tot - _bit_buy.prefixSum(idx-1);  // 本价及以上买量
    uint64_t sd = _bit_sell.prefixSum(idx);            // 本价及以下卖量
    uint64_t buy_left  = (bu + buy_bkt.vol_sum)  - sd; // 本价买方剩余
    uint64_t sell_left = (sd + sell_bkt.vol_sum) - bu; // 本价卖方剩余

    // 先撮合卖方
    while(buy_left>0 && !sell_bkt.orders.empty()){
        auto s=sell_bkt.orders.front();
        uint64_t q=std::min<uint64_t>(s->remaining_volume(),buy_left);
        s->traded_volume+=q; 
        ob_.bucketSub(idx,false,q); 
        buy_left-=q;
        s->status = (s->remaining_volume()?OrderStatus::PartFilled:OrderStatus::Filled);
        if(s->status==OrderStatus::Filled) sell_bkt.orders.pop_front();
    }
    // 再撮合买方
    while(sell_left>0 && !buy_bkt.orders.empty()){
        auto b=buy_bkt.orders.front();
        uint64_t q=std::min<uint64_t>(b->remaining_volume(),sell_left);
        b->traded_volume+=q; 
        ob_.bucketSub(idx,true,q); 
        sell_left-=q;
        b->status = (b->remaining_volume()?OrderStatus::PartFilled:OrderStatus::Filled);
        if(b->status==OrderStatus::Filled) buy_bkt.orders.pop_front();
    }
}

/* 结算：集合竞价结束，撮合成交，清空树状数组和累计量 */
void CallAuctionEngine::settle()
{
    // 先计算最终成交价
    Price px = calcPredict();
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
    // 清空树状数组和累计量，准备下一轮
    _bit_buy=Fenwick{}; 
    _bit_sell=Fenwick{};
    _tot_buy=_tot_sell=0;
}

} // namespace wangcai_orderbook_cpp 