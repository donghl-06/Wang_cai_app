/*
 * @Author: linzhuoyu
 * @Date: 2025-07-07 08:43:04
 * @LastEditTime: 2025-08-14 03:13:23
 * @FilePath: /wangcai_cpp/include/orderbook.h
 */

#pragma once
#include "order.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include "orderpool.h"
#include <utility>
#include <map>

namespace wangcai {
// 事件结构
struct Event {
    std::string datetime;
    std::string sym;
    int64_t price;
    int64_t size;
    int64_t side;
    int64_t ordertype;
    int64_t orderid;
    int64_t channelno;
    int64_t seqno;
    int64_t bizindex;
    int64_t bidorderid;
    int64_t askorderid;
    int64_t tradeid;
    std::string exectype;
    std::string tradebsflag;

    // tick 数据 
    int64_t prevclose;
    int64_t open;
    int64_t high;
    int64_t low;
    int64_t close;
    int64_t volume;
    int64_t turnover;
    int64_t tradecount;
    std::array<std::uint64_t, 10> bids_{};
    std::array<Quantity, 10> bid_sizes_{};
    std::array<std::uint64_t, 10> asks_{};
    std::array<Quantity, 10> ask_sizes_{};
    int64_t avgbid;
    int64_t avgask;
    int64_t totalbsize;
    int64_t totalasize;
    int64_t iopv;
    // tick end
    std::string source; // "ord" 或 "tra" 或 “tick"

    static bool is_SZ;
    // uint64_t sort_key; // 排序键：SZ用orderid，SH用bizindex
    
    Event(const std::string& dt, const std::string& symbol, int64_t p, int64_t sz, int64_t sd, 
          int64_t ot, int64_t oid, int64_t ch, int64_t seq, int64_t biz, int64_t bid, int64_t ask, 
          int64_t tid, const std::string& et, const std::string& tbf,
          // tick data 
          int64_t prev, int64_t op, int64_t hi, int64_t lo, int64_t cl,
          int64_t vol, int64_t trn, int64_t trcnt,
          const std::array<std::uint64_t, 10>& bids, const std::array<Quantity, 10>& bid_sizes,
          const std::array<std::uint64_t, 10>& asks, const std::array<Quantity, 10>& ask_sizes,
          int64_t avgb, int64_t avga, int64_t total_b, int64_t total_a, int64_t iopv,
          // tick data end
          const std::string& src) 
        : datetime(dt), sym(symbol), price(p), size(sz), side(sd), ordertype(ot), orderid(oid),
          channelno(ch), seqno(seq), bizindex(biz), bidorderid(bid), askorderid(ask), tradeid(tid),
          exectype(et), tradebsflag(tbf),
          prevclose(prev), open(op), high(hi), low(lo), close(cl),
          volume(vol), turnover(trn), tradecount(trcnt),
          bids_(bids), bid_sizes_(bid_sizes), asks_(asks), ask_sizes_(ask_sizes),
          avgbid(avgb), avgask(avga), totalbsize(total_b), totalasize(total_a), iopv(iopv),
        
          source(src) {}
};


class CallAuctionEngine;   // friend
class ConAuctionEngine;    // friend

class OrderBook {
    friend class CallAuctionEngine;
    friend class ConAuctionEngine;
    friend class CloseAuctionEngine;
    friend class DataManager;
public:
    // 定义回调函数类型
    using ExecCallback = std::function<void(const Execution&)>;
    std::string getExchange() const { return _exchange; }
    OrderBook(double hi, double lo, bool is_etf, ExecCallback cb = nullptr);

    // 用于 SZ 市价单特殊逻辑的共享表
    // ① 外部 ID  ↦  最优价成交（0 表示尚未出现成交）
    std::unordered_map<uint64_t, Price> first_trade_px_;

    // ② 外部 ID  ↦  "替换后真实价格"（只有 ordtype 1/3 被限价化的才会记录）
    std::unordered_map<uint64_t, Price> real_mkt_orders_;

    // （可选）简单的 getter，供外部只读
    const auto& firstTradePx()   const { return first_trade_px_;   }
    const auto& realMktOrders()  const { return real_mkt_orders_;  }   

    //查询
    [[nodiscard]] Price bestBid() const;
    [[nodiscard]] Price bestAsk() const;

    // 设置并获取最新成交价（用于比较距离）
    void setLastTradePrice(Price p) { _last_trade_price = p; }
    Price getLastTradePrice() const { return _last_trade_price; }
    
    // 获取原始订单ID（如果存在映射的话）
    uint64_t getOriginalOrderId(uint64_t system_id) const {
        auto it = sys2input_.find(system_id);
        return (it != sys2input_.end()) ? it->second : system_id;
    }
    
    // 获取订单信息
    std::shared_ptr<Order> getOrder(uint64_t order_id) const {
        auto it = _omap.find(order_id);
        return (it != _omap.end()) ? it->second : nullptr;
    }

    // 工厂：统一通过对象池生成订单 
    template<typename... Args>
    std::shared_ptr<Order> createOrder(Args&&... args)
    {
        // 1) 从对象池拿一块内存并原地构造
        auto od = _order_pool.acquire(std::forward<Args>(args)...);

        //
        // ① 字符串 → 系统 ID（无论是否全数字都写）
        sys2str_[od->order_id] = od->order_local_id;
        str2sys_[od->order_local_id] = od->order_id;

        // ② 如果字符串是纯数字，再额外写 uint64_t ↔ uint64_t
        char* endptr = nullptr;
        uint64_t num = std::strtoull(od->order_local_id.c_str(), &endptr, 10);
        if (endptr != od->order_local_id.c_str() && *endptr == '\0') {
            if (num != 0) { // 0 号单直接忽略
                input2sys_[num] = od->order_id;
                sys2input_[od->order_id] = num;
            }
        }
        /* -------------------------------------- */
        return od;
    }
    
    // 设置前收盘价和交易所
    void setPrevClosePrice(Price price) { _prev_close_price = price; }
    void setExchange(const std::string& exchange) { _exchange = exchange; }
    // 全局有序列表
    // static std::map<uint64_t, std::vector<Event>> whole_events; // 全局事件列表
    // const std::map<uint64_t, std::vector<Event>>& getEvents() const { return whole_events; }
    static std::vector<Event> whole_events;
    static std::vector<Event> tick_events;
    static void clearTicks() { tick_events.clear(); }
    static void insertTick(const Event& event);

    const std::vector<Event>& getEvents() const { return whole_events; }
    static void clearEvents() { whole_events.clear(); }
    static void insertEvent(const Event& event);

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
    Price _last_trade_price{0};  // 最新成交价（全局）
    std::string _exchange;    // 交易所标识

    std::unordered_map<uint64_t, Locator> _loc;   // 订单→位置
    std::unordered_map<uint64_t, std::shared_ptr<Order>> _omap;  //订单映射表 - 依赖对象池
    ExecCallback _on_exec;  //成交回调函数

    // >>> 共享：原始输入ID  →  系统ID  (盘前+盘中都用)
    std::unordered_map<uint64_t, uint64_t> input2sys_;
    // 系统id → 原始输入id
    std::unordered_map<uint64_t, uint64_t> sys2input_;
    // 新增：支持字符串单号（以后策略想用 “MR_0001” 也 OK）
    std::unordered_map<std::string, uint64_t> str2sys_;
    std::unordered_map<uint64_t, std::string> sys2str_;

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

} // namespace wangcai