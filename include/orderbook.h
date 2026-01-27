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
#include <iostream>

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
    std::string source; // "ord" 或 "tra" 或 "tick"
    
    // === 新增：原始市场数据字段（撮合不会产生的） ===
    int exchange;           // 交易所代码 (0=上海, 1=深圳)
    int trading_day;        // 交易日 (格式：YYYYMMDD)  
    int action_day;         // 自然日 (格式：YYYYMMDD)
    std::string status;     // 状态字符串
    char order_kind;        // 委托类别 ('1'市价/'2'限价/'U'本方最优)
    int64_t trade_index;    // 成交索引 (TradeDetail中的TradeIndex)
    int time_raw;           // 原始时间字段 (市场数据中的Time字段)

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
          const std::string& src,
          // === 新增：原始市场数据字段 ===
          int exch = -1, int tday = -1, int aday = -1, const std::string& stat = "",
          char okind = '\0', int64_t tidx = -1, int traw = -1) 
        : datetime(dt), sym(symbol), price(p), size(sz), side(sd), ordertype(ot), orderid(oid),
          channelno(ch), seqno(seq), bizindex(biz), bidorderid(bid), askorderid(ask), tradeid(tid),
          exectype(et), tradebsflag(tbf),
          prevclose(prev), open(op), high(hi), low(lo), close(cl),
          volume(vol), turnover(trn), tradecount(trcnt),
          bids_(bids), bid_sizes_(bid_sizes), asks_(asks), ask_sizes_(ask_sizes),
          avgbid(avgb), avgask(avga), totalbsize(total_b), totalasize(total_a), iopv(iopv),
          source(src), exchange(exch), trading_day(tday), action_day(aday), status(stat),
          order_kind(okind), trade_index(tidx), time_raw(traw) {}
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
    
    // 获取 tick 大小（ETF=10, 股票=100）
    Price getTick() const { return _tick; }
    
    // 获取原始订单ID（通过 Order::input_id）
    uint64_t getInputId(uint64_t system_id) const {
        auto it = _omap.find(system_id);
        if (it != _omap.end() && it->second->input_id != 0) {
            return it->second->input_id;
        }
        return system_id;
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

        // 2) 如果 order_local_id 是纯数字，缓存到 input_id 并建立映射
        char* endptr = nullptr;
        uint64_t num = std::strtoull(od->order_local_id.c_str(), &endptr, 10);
        if (endptr != od->order_local_id.c_str() && *endptr == '\0') {
            od->input_id = num;  // 缓存到 Order 对象
            if (num != 0) {
                input2sys_[num] = od->order_id;  // 正向映射（撤单用）
            }
        }
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
        Quantity vol_sum{0};         // 历史订单量统计
        Quantity user_vol_sum{0};    // USER订单量统计
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

    // 原始输入ID → 系统ID（撤单时 O(1) 查找）
    std::unordered_map<uint64_t, uint64_t> input2sys_;

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