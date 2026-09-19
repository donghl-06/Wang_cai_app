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
#include <optional>
#include <stdexcept>

namespace wangcai {
struct MarketOrderKey {
    int channel_no{-1};
    uint64_t market_order_id{0};

    bool operator==(const MarketOrderKey&) const = default;
};

struct MarketOrderKeyHash {
    std::size_t operator()(const MarketOrderKey& key) const noexcept {
        const auto h1 = std::hash<int>{}(key.channel_no);
        const auto h2 = std::hash<uint64_t>{}(key.market_order_id);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct MarketOrderIdentity {
    int trading_day{-1};
    std::string instrument;
    int channel_no{-1};
    uint64_t market_order_id{0};
    Direction direction{Direction::Buy};
};

class MarketIdentityError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// "YYYY-MM-DD HH:MM:SS[.fraction]" -> 自 1970-01-01 起的毫秒数(UTC 无关,仅作排序键)
// 手工扫描解析,每事件一次;之后排序/归并全部用 int64 比较,不再做 23 字符字符串比较。
// 小数部分取前 3 位(毫秒),不足补零;解析失败返回 -1。
inline int64_t parseDatetimeMs(const std::string& dt) {
    if (dt.size() < 19) return -1;
    auto d2 = [&](std::size_t p) { return (dt[p] - '0') * 10 + (dt[p + 1] - '0'); };
    const int y = (dt[0]-'0')*1000 + (dt[1]-'0')*100 + d2(2);
    const int m = d2(5), d = d2(8);
    // days-from-civil (Howard Hinnant 算法)
    const int yy = (m <= 2) ? y - 1 : y;
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097LL + static_cast<int64_t>(doe) - 719468;
    int64_t ms = (static_cast<int64_t>(d2(11)) * 3600 +
                  static_cast<int64_t>(d2(14)) * 60 + d2(17)) * 1000;
    if (dt.size() > 20 && dt[19] == '.') {
        int frac = 0, nd = 0;
        for (std::size_t p = 20; p < dt.size() && nd < 3 &&
             dt[p] >= '0' && dt[p] <= '9'; ++p, ++nd)
            frac = frac * 10 + (dt[p] - '0');
        while (nd < 3) { frac *= 10; ++nd; }
        ms += frac;
    }
    return days * 86400000LL + ms;
}

// 事件结构
struct Event {
    std::string datetime;
    int64_t datetime_ms;  // datetime 的 int64 排序键(构造时解析一次)
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

    [[nodiscard]] bool isSZ() const {
        if (exchange == 1) return true;
        if (exchange == 0) return false;
        return sym.size() >= 3 && sym.ends_with(".SZ");
    }
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
        : datetime(dt), datetime_ms(parseDatetimeMs(dt)), sym(symbol), price(p), size(sz), side(sd), ordertype(ot), orderid(oid),
          channelno(ch), seqno(seq), bizindex(biz), bidorderid(bid), askorderid(ask), tradeid(tid),
          exectype(et), tradebsflag(tbf),
          prevclose(prev), open(op), high(hi), low(lo), close(cl),
          volume(vol), turnover(trn), tradecount(trcnt),
          bids_(bids), bid_sizes_(bid_sizes), asks_(asks), ask_sizes_(ask_sizes),
          avgbid(avgb), avgask(avga), totalbsize(total_b), totalasize(total_a), iopv(iopv),
          source(src), exchange(exch), trading_day(tday), action_day(aday), status(stat),
          order_kind(okind), trade_index(tidx), time_raw(traw) {}
};

inline int eventSourceRank(const Event& event) {
    if (event.source == "ord") return 0;
    if (event.source == "tra") return 1;
    return 2;  // tick
}

// AData 的深圳逐笔流共用一条递增业务序列：委托取 csord.orderid，
// 成交/撤单取 cstra.tradeid。不要依赖加载器恰好把 tradeid 复制到 orderid。
inline int64_t marketEventSequenceKey(const Event& event) {
    if (!event.isSZ()) return event.bizindex;
    return event.source == "tra" ? event.tradeid : event.orderid;
}

// 单条事件自带交易所属性，不能依赖跨标的共享的静态市场开关。
// 主键用构造时预解析的 int64 时间戳,不再做 23 字符字符串比较。
inline bool marketEventLess(const Event& a, const Event& b) {
    if (a.datetime_ms != b.datetime_ms) return a.datetime_ms < b.datetime_ms;

    const int a_rank = eventSourceRank(a);
    const int b_rank = eventSourceRank(b);
    if ((a_rank == 2) != (b_rank == 2)) return a_rank < b_rank;  // 逐笔先于快照
    if (a_rank == 2 && b_rank == 2) return false;

    if (a.isSZ() != b.isSZ()) return a.exchange < b.exchange;
    const int64_t a_key = marketEventSequenceKey(a);
    const int64_t b_key = marketEventSequenceKey(b);
    if (a_key != b_key) return a_key < b_key;
    if (a_rank != b_rank) return a_rank < b_rank;  // 同键时先注册委托，再处理成交/撤单
    if (a.channelno != b.channelno) return a.channelno < b.channelno;
    return a.seqno < b.seqno;
}


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
    std::unordered_map<MarketOrderKey, Price, MarketOrderKeyHash> first_trade_px_;

    // ② 外部 ID  ↦  "替换后真实价格"（只有 ordtype 1/3 被限价化的才会记录）
    std::unordered_map<MarketOrderKey, Price, MarketOrderKeyHash> real_mkt_orders_;

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

    // 涨跌停 / 前收盘只读接口（合成实时快照用）
    Price getUpperLimit() const { return _upper; }
    Price getLowerLimit() const { return _lower; }
    Price getPrevClose()  const { return _prev_close_price; }

    // 全簿历史挂单总量（买/卖），bucketAdd/bucketSub 增量维护，O(1) 读取
    Quantity getTotalBidVol() const { return _total_bid_vol; }
    Quantity getTotalAskVol() const { return _total_ask_vol; }

    // 沿非空桶链表填充前 10 档价量（只含历史订单，不含用户虚拟单）。
    // 买盘从 _best_bid 沿 next 价格递减，卖盘从 _best_ask 沿 next 价格递增；
    // 不足 10 档的位置清零。直接写入定长数组，避免高频调用时的堆分配。
    void fillDepth(std::array<std::uint64_t, 10>& px,
                   std::array<Quantity, 10>& sz,
                   bool is_buy) const;
    
    // 兼容内部诊断：只有已注册的历史 system-id 才会转换；禁止用于输出边界。
    uint64_t getInputId(uint64_t system_id) const {
        auto it = market_identity_by_system_id_.find(system_id);
        if (it != market_identity_by_system_id_.end()) return it->second.market_order_id;
        return system_id;
    }

    void registerHistoricalOrder(const std::shared_ptr<Order>& order, uint64_t market_order_id,
                                 int channel_no, int trading_day);
    [[nodiscard]] const MarketOrderIdentity& requireMarketIdentity(uint64_t system_id) const;
    [[nodiscard]] std::optional<uint64_t> findSystemOrderId(
        uint64_t market_order_id, int channel_no = -1) const;
    void eraseActiveMarketOrder(uint64_t system_id);
    // 进场即撤：将订单从活跃表移入进场即撤表（语义见成员声明）
    void markEntryCancelled(uint64_t system_id);
    [[nodiscard]] std::optional<uint64_t> findEntryCancelledSystemId(
        uint64_t market_order_id, int channel_no = -1) const;
    // 簿外价吸收：将订单从活跃表移入簿外价表（语义见成员声明）
    void markOutOfBookAbsorbed(uint64_t system_id);
    [[nodiscard]] std::optional<uint64_t> findOutOfBookAbsorbedSystemId(
        uint64_t market_order_id, int channel_no = -1) const;
    // 价格是否在簿边界内（accept 段簿外价吸收判据）
    bool inBookRange(Price p) const { return p >= _lower && p <= _upper; }
    
    // 获取订单信息
    std::shared_ptr<Order> getOrder(uint64_t order_id) const {
        auto it = _omap.find(order_id);
        return (it != _omap.end()) ? it->second : nullptr;
    }

    // 通过原始输入订单ID判断订单是否仍在簿（供真实成交替代模式回退判定）
    bool hasOrderByInputId(uint64_t input_id, int channel_no = -1) const {
        auto sys_id = findSystemOrderId(input_id, channel_no);
        return sys_id.has_value() && _omap.find(*sys_id) != _omap.end();
    }

    // 工厂：统一通过对象池生成订单 
    template<typename... Args>
    std::shared_ptr<Order> createOrder(Args&&... args)
    {
        // 1) 从对象池拿一块内存并原地构造
        auto od = _order_pool.acquire(std::forward<Args>(args)...);

        return od;
    }

    template<typename... Args>
    std::shared_ptr<Order> createHistoricalOrder(uint64_t market_order_id, int channel_no,
                                                 int trading_day, Args&&... args)
    {
        auto order = createOrder(std::forward<Args>(args)...);
        registerHistoricalOrder(order, market_order_id, channel_no, trading_day);
        return order;
    }
    
    // 设置前收盘价和交易所
    void setPrevClosePrice(Price price) { _prev_close_price = price; }
    void setExchange(const std::string& exchange) { _exchange = exchange; }
    // 本簿事件列表(2026-09-15 起从静态全局改为实例成员:
    // 静态共享表使多合约初始化只能串行,实例化后 MultiBacktestEngine 可并行构建)
    std::vector<Event> whole_events;
    std::vector<Event> tick_events;
    void clearTicks() { tick_events.clear(); }
    void insertTick(const Event& event);

    const std::vector<Event>& getEvents() const { return whole_events; }
    void clearEvents() { whole_events.clear(); }
    void insertEvent(const Event& event);

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
    Quantity _total_bid_vol{0};  // 全簿历史买单挂单总量（随 bucketAdd/bucketSub 维护）
    Quantity _total_ask_vol{0};  // 全簿历史卖单挂单总量
    Price _prev_close_price{0};  // 前收盘价
    Price _last_trade_price{0};  // 最新成交价（全局）
    std::string _exchange;    // 交易所标识

    std::unordered_map<uint64_t, Locator> _loc;   // 订单→位置
    std::unordered_map<uint64_t, std::shared_ptr<Order>> _omap;  //订单映射表 - 依赖对象池
    ExecCallback _on_exec;  //成交回调函数

    // system-id → 原始市场身份是持久表；订单成交/撤销后仍保留，供回调和 CSV 解析。
    std::unordered_map<uint64_t, MarketOrderIdentity> market_identity_by_system_id_;
    // 活跃市场身份 → system-id，仅用于市场撤单反查。
    std::unordered_map<MarketOrderKey, uint64_t, MarketOrderKeyHash> market_system_id_by_key_;
    // 进场即撤市场身份 → system-id：深市市价/本方最优单在定价基准侧空簿时
    // 被交易所当场自动撤销（零成交、撤单记录与委托同时间戳），身份登记于此，
    // 供后续真实撤单记录精确吸收（no-op），不复用活跃表以免掩盖真异常。
    std::unordered_map<MarketOrderKey, uint64_t, MarketOrderKeyHash> entry_cancelled_system_id_by_key_;
    // 簿外价吸收市场身份 → system-id：无涨跌幅日簿边界按成交价范围封顶后，
    // 界外历史委托（恶作剧天价卖/地板价买，永不成交，论证见 OrderLoader.cpp
    // loadPriceLimits）不进簿，身份登记于此，供后续真实撤单记录精确吸收
    // （no-op）。与进场即撤表分立：两种语义的触发原因不同，分表便于归因。
    std::unordered_map<MarketOrderKey, uint64_t, MarketOrderKeyHash> out_of_book_system_id_by_key_;

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
