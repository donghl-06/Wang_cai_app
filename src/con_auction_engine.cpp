// === src/con_auction_engine.cpp ===
#include "../include/con_auction_engine.hpp"
#include <algorithm>

namespace wangcai_orderbook_cpp {

/* match (连续竞价核心) */
void ConAuctionEngine::match(std::shared_ptr<Order>& inc)
{
    bool buy=inc->direction==Direction::Buy;
    auto& opp=buy?ob_._sell:ob_._buy;
    int&  best=buy?ob_._best_ask:ob_._best_bid;

    while(inc->remaining_volume()>0 && best!=-1){
        Price px=ob_._lower+best*ob_._tick;
        if((buy&&inc->price<px)||(!buy&&inc->price>px)) break;
        auto& bkt=opp[best];
        while(inc->remaining_volume()>0 && !bkt.orders.empty()){
            auto oppo=bkt.orders.front();
            Quantity q=std::min(inc->remaining_volume(),oppo->remaining_volume());
            inc ->traded_volume+=q;
            oppo->traded_volume+=q;
            ob_.bucketSub(best,!buy,q);
            if(ob_._on_exec){
                Execution ex(buy?inc->order_id:oppo->order_id,
                             buy?oppo->order_id:inc->order_id,
                             px,q);
                ob_._on_exec(ex);
            }
            if(oppo->remaining_volume()==0){
                oppo->status=OrderStatus::Filled;
                bkt.orders.pop_front();
                ob_._loc.erase(oppo->order_id);
                ob_._omap.erase(oppo->order_id);
            }else oppo->status=OrderStatus::PartFilled;
        }
        best=buy?ob_._best_ask:ob_._best_bid;
    }
    if(inc->remaining_volume()==0) inc->status=OrderStatus::Filled;
    else if(inc->traded_volume)    inc->status=OrderStatus::PartFilled;
}

void ConAuctionEngine::accept(std::shared_ptr<Order> od)
{
    bool buy=od->direction==Direction::Buy;
    int idx=ob_.pxToIdx(od->price);
    ob_._omap[od->order_id]=od;

    if(od->order_type==OrderType::Market){
        match(od);
        if(od->remaining_volume()) od->status=OrderStatus::Cancelled;
        return;
    }
    match(od);
    if(od->remaining_volume()==0) return;

    auto& side=buy?ob_._buy:ob_._sell;
    side[idx].orders.push_back(od);
    od->level_iter=std::prev(side[idx].orders.end());
    ob_.bucketAdd(idx,buy,od->remaining_volume());
    ob_._loc[od->order_id]={buy,idx,od->level_iter};
    
    // 如果订单的本地ID是数字，建立输入ID到系统ID的映射
    try {
        uint64_t input_id = std::stoull(od->order_local_id);
        _input_id_to_system_id[input_id] = od->order_id;
    } catch (const std::exception&) {
        // 如果本地ID不是数字，忽略
    }
}

bool ConAuctionEngine::cancel(uint64_t oid)
{
    auto it=ob_._loc.find(oid);
    if(it==ob_._loc.end()) {
        // 订单不存在，撤单失败
        if(on_cancel_) on_cancel_(oid, false, "订单不存在");
        return false;
    }
    
    auto loc=it->second;
    auto& side=loc.is_buy?ob_._buy:ob_._sell;
    auto ord=*loc.it;
    
    if(ord->status==OrderStatus::Cancelled||ord->status==OrderStatus::Filled) {
        // 订单已经撤销或已成交
        std::string reason = (ord->status == OrderStatus::Cancelled) ? "订单已撤销" : "订单已成交";
        if(on_cancel_) on_cancel_(oid, false, reason);
        return false;
    }
    
    Quantity rem=ord->remaining_volume();
    side[loc.idx].orders.erase(loc.it);
    ob_.bucketSub(loc.idx,loc.is_buy,rem);
    ord->status=OrderStatus::Cancelled;
    ob_._loc.erase(it); 
    ob_._omap.erase(oid);
    
    // 撤单成功回调
    if(on_cancel_) on_cancel_(oid, true, "撤单成功");
    
    return true;
}

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