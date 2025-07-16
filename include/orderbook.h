/*
 * @Author: linzhuoyu
 * @Date: 2025-07-07 08:43:04
 * @LastEditTime: 2025-07-08 03:01:00
 * @FilePath: /wangcai_orderbook_cpp/include/orderbook.h
 */

#pragma once
#include "order.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include "orderpool.h"
#include <utility>
#include <map>

namespace wangcai_orderbook_cpp {

class CallAuctionEngine;   // friend
class ConAuctionEngine;    // friend

class OrderBook {
    friend class CallAuctionEngine;
    friend class ConAuctionEngine;
public:

    using ExecCallback = std::function<void(const Execution&)>;
    std::string getExchange() const { return _exchange; }
    OrderBook(double hi, double lo, bool is_etf, ExecCallback cb = nullptr);

    //查询
    [[nodiscard]] Price bestBid() const;
    [[nodiscard]] Price bestAsk() const;

    // 工厂：统一通过对象池生成订单 
    template<typename... Args>
    std::shared_ptr<Order> createOrder(Args&&... args) {
        return _order_pool.acquire(std::forward<Args>(args)...);
     }
    
    // 设置前收盘价和交易所
    void setPrevClosePrice(Price price) { _prev_close_price = price; }
    void setExchange(const std::string& exchange) { _exchange = exchange; }

private:
    //桶结构体
    struct Bucket {
        std::list<std::shared_ptr<Order>> orders;   // 时间顺序
        Quantity vol_sum{0};
        int  prev{-1};     // 非空桶链表 prev
        int  next{-1};     // 非空桶链表 next
    };

    //订单位置
    struct Locator {
        bool  is_buy;
        int   idx;
        std::list<std::shared_ptr<Order>>::iterator it;
    };

    //数据成员
    OrderPool _order_pool;  //订单池 - 必须先声明，后析构
    Price _lower, _upper, _tick;
    std::vector<Bucket> _buy;   // 买盘桶（价格低→高）
    std::vector<Bucket> _sell;  // 卖盘桶（价格低→高）
    int _best_bid{-1}, _best_ask{-1};  //最优价索引
    Price _prev_close_price{0};  // 前收盘价
    std::string _exchange;    // 交易所标识

    std::unordered_map<uint64_t, Locator> _loc;   // 订单→位置
    std::unordered_map<uint64_t, std::shared_ptr<Order>> _omap;  //订单映射表 - 依赖对象池
    ExecCallback _on_exec;  //成交回调函数

    //订单价格转换为桶索引
    int  pxToIdx(Price p) const { 
        if (p < _lower || p > _upper)
            throw std::out_of_range("price out of limit up/down"); // 价格超出范围
        if ((p - _lower) % _tick != 0)
            throw std::invalid_argument("price not aligned with tick"); // 价格未对齐
        return int((p - _lower) / _tick);
    }
    //桶索引转换为订单价格
    Price idxToPx(int i) const  { return _lower + i * _tick; }

    //链表维护
    void attachBucket(int idx, bool is_buy);
    void detachBucket(int idx, bool is_buy);

    //统一增/减桶量，自动维护链和最优价
    void bucketAdd(int idx, bool is_buy, Quantity q);
    void bucketSub(int idx, bool is_buy, Quantity q);

    
};

} // namespace wangcai_orderbook_cpp