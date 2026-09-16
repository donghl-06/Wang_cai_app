#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "multi_backtest_engine.h"
#include "backtest_engine.hpp"
#include "market_info.h"
#include "types.h"
#include "orderbook.h"
#include "order.h"


namespace py = pybind11;
using namespace wangcai;

// --------- Trampoline for Strategy so Python can override virtuals ---------
class PyStrategy : public Strategy {
public:
    using Strategy::Strategy;
    
    // === 用户自定义数据支持 ===
    // 存储用户传入的自定义数据列表（每个元素是一个 dict）
    void setCustomDataList(py::list data_list) {
        custom_data_list_ = data_list;
    }
    
private:
    // 用户自定义数据列表（由 Python 层设置）
    py::list custom_data_list_;
    
    // 自动跟踪处理中的事件数量
    mutable std::atomic<int> processing_count_{0};

    // onRealTimeTickEvent 覆盖探测缓存：-1=未知, 0=Python 子类未覆盖, 1=已覆盖
    // 实时 tick 频率可达每标的每日数十万次，未覆盖时避免反复拿 GIL
    mutable std::atomic<int> rt_tick_override_state_{-1};

    // onEventSnapshot 覆盖探测缓存（同上，事件快照频率与市场事件同量级）
    mutable std::atomic<int> event_snap_override_state_{-1};

    // onOrderEvent/onTradeEvent/onTickEvent 覆盖探测缓存（模式同上；
    // 这三个是市场事件主回调，频率最高，未覆盖时跳过 GIL+override 查询收益最大）
    mutable std::atomic<int> order_event_override_state_{-1};
    mutable std::atomic<int> trade_event_override_state_{-1};
    mutable std::atomic<int> tick_event_override_state_{-1};
    mutable std::atomic<int> trade_batch_override_state_{-1};
    
    // RAII辅助类：自动管理处理计数器
    class ProcessingGuard {
        std::atomic<int>& counter_;
    public:
        ProcessingGuard(std::atomic<int>& counter) : counter_(counter) {
            counter_.fetch_add(1);
        }
        ~ProcessingGuard() {
            counter_.fetch_sub(1);
        }
    };

public:
    // 事件回调：自动跟踪处理状态；未覆盖时经探测缓存直接返回，不进 GIL
    std::vector<UserEvent> onOrderEvent(const OrderDetail& order) override {
        if (order_event_override_state_.load(std::memory_order_relaxed) == 0) {
            return {};
        }
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(this, "onOrderEvent");
        if (!f) {
            order_event_override_state_.store(0, std::memory_order_relaxed);
            return {};
        }
        order_event_override_state_.store(1, std::memory_order_relaxed);
        ProcessingGuard guard(processing_count_);  // 自动管理计数器
        try {
            py::object ret = f(order);
            return ret.cast<std::vector<UserEvent>>();
        } catch (const py::error_already_set& e) {
            py::print("[Strategy.onOrderEvent] exception:", e.what());
        }
        return {};
    }

    std::vector<UserEvent> onTradeEvent(const TradeDetail& trade) override {
        if (trade_event_override_state_.load(std::memory_order_relaxed) == 0) {
            return {};
        }
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(this, "onTradeEvent");
        if (!f) {
            trade_event_override_state_.store(0, std::memory_order_relaxed);
            return {};
        }
        trade_event_override_state_.store(1, std::memory_order_relaxed);
        ProcessingGuard guard(processing_count_);  // 自动管理计数器
        try {
            py::object ret = f(trade);
            return ret.cast<std::vector<UserEvent>>();
        } catch (const py::error_already_set& e) {
            py::print("[Strategy.onTradeEvent] exception:", e.what());
        }
        return {};
    }

    // 批量成交推送:Python 子类覆写了 onTradeEventsBatch 时整批一次跨边界;
    // 未覆写时回落基类默认实现(逐条 onTradeEvent,可再被 P0 缓存短路)
    std::vector<UserEvent> onTradeEventsBatch(const std::vector<TradeDetail>& trades) override {
        if (trade_batch_override_state_.load(std::memory_order_relaxed) == 0) {
            return Strategy::onTradeEventsBatch(trades);
        }
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(this, "onTradeEventsBatch");
        if (!f) {
            trade_batch_override_state_.store(0, std::memory_order_relaxed);
            return Strategy::onTradeEventsBatch(trades);
        }
        trade_batch_override_state_.store(1, std::memory_order_relaxed);
        ProcessingGuard guard(processing_count_);
        try {
            py::object ret = f(trades);
            return ret.cast<std::vector<UserEvent>>();
        } catch (const py::error_already_set& e) {
            py::print("[Strategy.onTradeEventsBatch] exception:", e.what());
        }
        return {};
    }

    std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) override {
        if (tick_event_override_state_.load(std::memory_order_relaxed) == 0) {
            return {};
        }
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(this, "onTickEvent");
        if (!f) {
            tick_event_override_state_.store(0, std::memory_order_relaxed);
            return {};
        }
        tick_event_override_state_.store(1, std::memory_order_relaxed);
        ProcessingGuard guard(processing_count_);  // 自动管理计数器
        try {
            py::object ret = f(snapshot);
            return ret.cast<std::vector<UserEvent>>();
        } catch (const py::error_already_set& e) {
            py::print("[Strategy.onTickEvent] exception:", e.what());
        }
        return {};
    }

    // 实时合成 tick 回调：首次调用探测 Python 子类是否覆盖并缓存结果，
    // 未覆盖时后续调用直接返回，不再进入 GIL
    std::vector<UserEvent> onRealTimeTickEvent(const Snapshot& snapshot) override {
        if (rt_tick_override_state_.load(std::memory_order_relaxed) == 0) {
            return {};
        }
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(this, "onRealTimeTickEvent");
        if (!f) {
            rt_tick_override_state_.store(0, std::memory_order_relaxed);
            return {};
        }
        rt_tick_override_state_.store(1, std::memory_order_relaxed);
        ProcessingGuard guard(processing_count_);
        try {
            py::object ret = f(snapshot);
            return ret.cast<std::vector<UserEvent>>();
        } catch (const py::error_already_set& e) {
            py::print("[Strategy.onRealTimeTickEvent] exception:", e.what());
        }
        return {};
    }

    // 事件驱动快照回调：探测缓存模式同 onRealTimeTickEvent
    std::vector<UserEvent> onEventSnapshot(const Snapshot& snapshot) override {
        if (event_snap_override_state_.load(std::memory_order_relaxed) == 0) {
            return {};
        }
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(this, "onEventSnapshot");
        if (!f) {
            event_snap_override_state_.store(0, std::memory_order_relaxed);
            return {};
        }
        event_snap_override_state_.store(1, std::memory_order_relaxed);
        ProcessingGuard guard(processing_count_);
        try {
            py::object ret = f(snapshot);
            return ret.cast<std::vector<UserEvent>>();
        } catch (const py::error_already_set& e) {
            py::print("[Strategy.onEventSnapshot] exception:", e.what());
        }
        return {};
    }

    // 用户自定义事件回调
    // C++ 层传入 index，这里转换为实际的 dict 数据后调用 Python 回调
    std::vector<UserEvent> onCustomEvent(size_t index) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onCustomEvent")) {
            ProcessingGuard guard(processing_count_);
            try {
                // 从 custom_data_list_ 中获取对应索引的数据
                py::dict data;
                if (!custom_data_list_.is_none() && index < py::len(custom_data_list_)) {
                    data = custom_data_list_[index].cast<py::dict>();
                }
                py::object ret = f(data);
                return ret.cast<std::vector<UserEvent>>();
            } catch (const py::error_already_set& e) {
                py::print("[Strategy.onCustomEvent] exception:", e.what());
            }
        }
        return {};
    }

    // 通知类回调：未覆写则忽略
    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onOrderFilled")) {
            try { f(order_id, price, volume); }
            catch (const py::error_already_set& e) { py::print("[Strategy.onOrderFilled] exception:", e.what()); }
        }
    }

    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onOrderCancelled")) {
            try { f(order_id, reason); }
            catch (const py::error_already_set& e) { py::print("[Strategy.onOrderCancelled] exception:", e.what()); }
        }
    }

    // 仍要求必须实现：策略 ID
    std::string getStrategyId() const override {
        PYBIND11_OVERRIDE_PURE(
            std::string,
            Strategy,
            getStrategyId,
        );
    }

    // 统一交易/下单回调：未覆写则忽略
    void onTradeCallback(const TradeCallback& callback) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onTradeCallback")) {
            try { f(callback); }
            catch (const py::error_already_set& e) { py::print("[Strategy.onTradeCallback] exception:", e.what()); }
        }
    }

    void onOrderCallback(const OrderCallback& callback) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onOrderCallback")) {
            try { f(callback); }
            catch (const py::error_already_set& e) { py::print("[Strategy.onOrderCallback] exception:", e.what()); }
        }
    }

    // 自动检测处理完成状态
    bool isProcessingComplete() const override {
        // 如果Python重写了此方法，调用Python版本
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "isProcessingComplete")) {
            try { 
                return f().cast<bool>(); 
            }
            catch (const py::error_already_set& e) { 
                py::print("[Strategy.isProcessingComplete] exception:", e.what()); 
            }
        }
        // 否则使用自动计数器检查
        return processing_count_.load() == 0;
    }
};

// Helper: wrap setMarketDataCallback to accept a Python callable safely
static void set_md_callback(BacktestEngine& eng, py::function cb) {
    std::function<void(const MarketData&)> fn = [cb](const MarketData& md) {
        py::gil_scoped_acquire gil;
        try {
            cb(md);
        } catch (const py::error_already_set& e) {
            // Print Python exception to stderr but don't crash engine
            py::print("MarketData callback error:", e.what());
        }
    };
    eng.setMarketDataCallback(std::move(fn));
}

PYBIND11_MODULE(wangcai_cpp, m) {
    m.doc() = "Pybind11 bindings for WangCai backtest engine";

    // Enums
    py::enum_<Direction>(m, "Direction")
        .value("Buy", Direction::Buy)
        .value("Sell", Direction::Sell)
        .export_values();

    py::enum_<OrderType>(m, "OrderType")
        .value("Limit", OrderType::Limit)
        .value("Market", OrderType::Market)
        .value("BestCounterpart", OrderType::BestCounterpart)
        .value("BestOwn", OrderType::BestOwn)
        .export_values();

    py::enum_<LatencyEntryPosition>(m, "LatencyEntryPosition")
        .value("Head", LatencyEntryPosition::Head)
        .value("Tail", LatencyEntryPosition::Tail)
        .export_values();

    // Basic typedefs (expose as Python ints)
    m.attr("PriceIsInt") = py::bool_(true);
    m.attr("QuantityIsInt") = py::bool_(true);

    // --------- 完整的原始市场数据结构 ---------
    
    // OrderDetail - 逐笔委托行情 (csord数据)
    py::class_<OrderDetail>(m, "OrderDetail")
        .def_readwrite("Exchange", &OrderDetail::Exchange)
        .def_readwrite("Instrument", &OrderDetail::Instrument)
        .def_readwrite("Time", &OrderDetail::Time)
        .def_readwrite("ChannelNo", &OrderDetail::ChannelNo)
        .def_readwrite("OrderNo", &OrderDetail::OrderNo)
        .def_readwrite("Price", &OrderDetail::Price)
        .def_readwrite("Volume", &OrderDetail::Volume)
        .def_readwrite("Side", &OrderDetail::Side)
        .def_property("OrderKind",
            [](const OrderDetail& o){ return std::string(1, o.OrderKind); },
            [](OrderDetail& o, const std::string& s){ o.OrderKind = s.empty()? '\0' : s[0]; })
        .def_readwrite("SeqNo", &OrderDetail::SeqNo)
        .def_readwrite("BizIndex", &OrderDetail::BizIndex);
    
    // TradeDetail - 逐笔成交行情 (cstra数据)
    py::class_<TradeDetail>(m, "TradeDetail")
        .def_readwrite("Exchange", &TradeDetail::Exchange)
        .def_readwrite("Instrument", &TradeDetail::Instrument)
        .def_readwrite("ChannelNo", &TradeDetail::ChannelNo)
        .def_readwrite("TradeIndex", &TradeDetail::TradeIndex)
        .def_readwrite("Time", &TradeDetail::Time)
        .def_readwrite("Price", &TradeDetail::Price)
        .def_readwrite("Volume", &TradeDetail::Volume)
        .def_property("ExecType",
            [](const TradeDetail& t){ return std::string(1, t.ExecType); },
            [](TradeDetail& t, const std::string& s){ t.ExecType = s.empty()? '\0' : s[0]; })
        .def_readwrite("BuyNo", &TradeDetail::BuyNo)
        .def_readwrite("SellNo", &TradeDetail::SellNo)
        .def_property("TradeBSFlag",
            [](const TradeDetail& t){ return std::string(1, t.TradeBSFlag); },
            [](TradeDetail& t, const std::string& s){ t.TradeBSFlag = s.empty()? '\0' : s[0]; })
        .def_readwrite("BizIndex", &TradeDetail::BizIndex);
    
    // Snapshot - 行情快照 (cstick数据) - 完整字段
    py::class_<Snapshot>(m, "Snapshot")
        .def_readwrite("Exchange", &Snapshot::Exchange)
        .def_readwrite("Instrument", &Snapshot::Instrument)
        .def_readwrite("TradingDay", &Snapshot::TradingDay)
        .def_readwrite("ActionDay", &Snapshot::ActionDay)
        .def_readwrite("Time", &Snapshot::Time)
        .def_readwrite("datetime", &Snapshot::datetime)
        .def_readwrite("Status", &Snapshot::Status)
        .def_readwrite("PreClose", &Snapshot::PreClose)
        .def_readwrite("Open", &Snapshot::Open)
        .def_readwrite("High", &Snapshot::High)
        .def_readwrite("Low", &Snapshot::Low)
        .def_readwrite("Close", &Snapshot::Close)
        .def_readwrite("last_price", &Snapshot::last_price)
        .def_readwrite("Volume", &Snapshot::Volume)
        .def_readwrite("Turnover", &Snapshot::Turnover)
        .def_readwrite("bids", &Snapshot::bids)
        .def_readwrite("asks", &Snapshot::asks)
        .def_readwrite("bid_sizes", &Snapshot::bid_sizes)
        .def_readwrite("ask_sizes", &Snapshot::ask_sizes)
        .def_readwrite("NumTrades", &Snapshot::NumTrades)
        .def_readwrite("UpperLimit", &Snapshot::UpperLimit)
        .def_readwrite("LowerLimit", &Snapshot::LowerLimit)
        .def_readwrite("TotalAskVol", &Snapshot::TotalAskVol)
        .def_readwrite("TotalBidVol", &Snapshot::TotalBidVol)
        .def_readwrite("OpenInterest", &Snapshot::OpenInterest)
        .def_readwrite("PreOpenInterest", &Snapshot::PreOpenInterest)
        .def_readwrite("Delta", &Snapshot::Delta)
        .def_readwrite("PreDelta", &Snapshot::PreDelta)
        .def_readwrite("SettlePrice", &Snapshot::SettlePrice)
        .def_readwrite("PreSettlePrice", &Snapshot::PreSettlePrice)
        .def_readwrite("AuctionPrice", &Snapshot::AuctionPrice)
        .def_readwrite("AuctionQty", &Snapshot::AuctionQty)
        .def_readwrite("Iopv", &Snapshot::Iopv)
        .def_readwrite("WeightedAvgBidPrice", &Snapshot::WeightedAvgBidPrice)
        .def_readwrite("WeightedAvgAskPrice", &Snapshot::WeightedAvgAskPrice)
        .def_readwrite("ETFCreateVol", &Snapshot::ETFCreateVol)
        .def_readwrite("ETFRedeemVol", &Snapshot::ETFRedeemVol);


   // --- TradeCallback ---
py::class_<TradeCallback>(m, "TradeCallback")
    .def(py::init<const std::string&, char, Quantity, Price, double, int64_t,
                  const std::string&, char>())
    .def_readwrite("localid", &TradeCallback::localid)
    .def_property("direction",
        [](const TradeCallback& t){ return std::string(1, t.direction); },
        [](TradeCallback& t, const std::string& s){ t.direction = s.empty()? '\0' : s[0]; })
    .def_readwrite("volume", &TradeCallback::volume)
    .def_readwrite("price", &TradeCallback::price)
    .def_readwrite("matchamount", &TradeCallback::matchamount)
    .def_readwrite("deltapos", &TradeCallback::deltapos)
    .def_readwrite("matchtime", &TradeCallback::matchtime)
    .def_property("matchtype",
        [](const TradeCallback& t){ return std::string(1, t.matchtype); },
        [](TradeCallback& t, const std::string& s){ t.matchtype = s.empty()? '\0' : s[0]; });

// --- OrderCallback ---
py::class_<OrderCallback>(m, "OrderCallback")
    .def(py::init<const std::string&, int, Price, Price, int64_t, Price,
                  Quantity, int, const std::string&>())
    .def_readwrite("time", &OrderCallback::time)
    .def_readwrite("exchange", &OrderCallback::exchange)
    .def_readwrite("ask1", &OrderCallback::ask1)
    .def_readwrite("bid1", &OrderCallback::bid1)
    .def_readwrite("deltapos", &OrderCallback::deltapos)
    .def_readwrite("price", &OrderCallback::price)
    .def_readwrite("volume", &OrderCallback::volume)
    .def_readwrite("direction", &OrderCallback::direction)
    .def_readwrite("orderlocalid", &OrderCallback::orderlocalid)
    .def_readwrite("queue_ahead_count", &OrderCallback::queue_ahead_count)
    .def_readwrite("queue_ahead_volume", &OrderCallback::queue_ahead_volume)
    .def_readwrite("prev_order_ids", &OrderCallback::prev_order_ids);


    // Structs
    py::class_<UserOrder>(m, "UserOrder")
        .def(py::init<>())
        .def_readwrite("order_id", &UserOrder::order_id)
        .def_readwrite("symbol", &UserOrder::symbol)
        .def_readwrite("direction", &UserOrder::direction)
        .def_readwrite("order_type", &UserOrder::order_type)
        .def_readwrite("price", &UserOrder::price)
        .def_readwrite("volume", &UserOrder::volume)
        .def_readwrite("strategy_id", &UserOrder::strategy_id);

    py::class_<UserCancel>(m, "UserCancel")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("order_id"), py::arg("strategy_id"))
        .def_readwrite("order_id", &UserCancel::order_id)
        .def_readwrite("strategy_id", &UserCancel::strategy_id);

    py::class_<UserEvent> user_event(m, "UserEvent");
    py::enum_<UserEvent::Type>(user_event, "Type")
        .value("ORDER", UserEvent::Type::ORDER)
        .value("CANCEL", UserEvent::Type::CANCEL)
        .export_values();
    user_event
        .def(py::init<const UserOrder&>())
        .def(py::init<const UserCancel&>())
        .def_readwrite("type", &UserEvent::type)
        .def_readwrite("order", &UserEvent::order)
        .def_readwrite("cancel", &UserEvent::cancel);

    py::class_<Position>(m, "Position")
        .def(py::init<>())
        .def_readwrite("symbol", &Position::symbol)
        .def_readwrite("quantity", &Position::quantity)
        .def_readwrite("avg_cost", &Position::avg_cost)
        .def_readwrite("unrealized_pnl", &Position::unrealized_pnl)
        .def_readwrite("realized_pnl", &Position::realized_pnl)
        .def("__repr__", [](const Position& p) {
            return py::str("Position(symbol='{}', quantity={}, avg_cost={:.4f}, "
                        "realized_pnl={:.2f}, unrealized_pnl={:.2f})")
                .format(p.symbol, p.quantity, p.avg_cost, 
                    p.realized_pnl, p.unrealized_pnl);
        })
        .def("__str__", [](const Position& p) {
            return py::str("持仓{}: 数量={}, 成本={:.4f}, 已实现盈亏={:.2f}")
                .format(p.symbol, p.quantity, p.avg_cost, p.realized_pnl);
        });

    // MarketData (a light read-only view for Python)
    py::class_<MarketData>(m, "MarketData")
        .def(py::init<>())
        .def_readonly("datetime", &MarketData::datetime)
        .def_readonly("symbol", &MarketData::symbol)
        .def_readonly("best_bid", &MarketData::best_bid)
        .def_readonly("best_ask", &MarketData::best_ask)
        .def_readonly("bid_volume", &MarketData::bid_volume)
        .def_readonly("ask_volume", &MarketData::ask_volume)
        .def_readonly("last_price", &MarketData::last_price);

    // Execution 的买卖订单号是纯内核 system-id，禁止跨 Python 边界暴露。
    // 用户可见的订单关联只通过 OrderDetail/TradeDetail 的原始市场 ID 表达。
    py::class_<Execution>(m, "Execution")
        .def_property_readonly("execution_id", [](const Execution& e){ return e.execution_id; })
        .def_property_readonly("price", [](const Execution& e){ return e.price; })
        .def_property_readonly("volume", [](const Execution& e){ return e.volume; });

    // Event (包含原始市场数据字段)
    py::class_<Event>(m, "Event")
        .def_property_readonly("datetime", [](const Event& e){ return e.datetime; })
        .def_property_readonly("sym", [](const Event& e){ return e.sym; })
        .def_property_readonly("price", [](const Event& e){ return e.price; })
        .def_property_readonly("size", [](const Event& e){ return e.size; })
        .def_property_readonly("side", [](const Event& e){ return e.side; })
        .def_property_readonly("ordertype", [](const Event& e){ return e.ordertype; })
        .def_property_readonly("orderid", [](const Event& e){ return e.orderid; })
        .def_property_readonly("channelno", [](const Event& e){ return e.channelno; })
        .def_property_readonly("seqno", [](const Event& e){ return e.seqno; })
        .def_property_readonly("bizindex", [](const Event& e){ return e.bizindex; })
        .def_property_readonly("exectype", [](const Event& e){ return e.exectype; })
        .def_property_readonly("tradebsflag", [](const Event& e){ return e.tradebsflag; })
        // === 新增：原始市场数据字段 ===
        .def_property_readonly("exchange", [](const Event& e){ return e.exchange; })
        .def_property_readonly("trading_day", [](const Event& e){ return e.trading_day; })
        .def_property_readonly("action_day", [](const Event& e){ return e.action_day; })
        .def_property_readonly("status", [](const Event& e){ return e.status; })
        .def_property_readonly("order_kind", [](const Event& e){ return std::string(1, e.order_kind); })
        .def_property_readonly("trade_index", [](const Event& e){ return e.trade_index; })
        .def_property_readonly("time_raw", [](const Event& e){ return e.time_raw; })
        .def_property_readonly("source", [](const Event& e){ return e.source; });

    // Strategy (abstract)
    py::class_<Strategy, PyStrategy, std::shared_ptr<Strategy>>(m, "Strategy")
        .def(py::init<>()) // 若 C++ 构造非默认，这里改真实签名并确保 PyStrategy 可构造
        .def("onOrderEvent", &Strategy::onOrderEvent)
        .def("onTradeEvent", &Strategy::onTradeEvent)
        .def("onTickEvent", &Strategy::onTickEvent)
        .def("onRealTimeTickEvent", &Strategy::onRealTimeTickEvent,
             "接收由内部订单簿合成的高频实时 Tick（需 setRealTimeTickInterval 开启）")
        .def("onEventSnapshot", &Strategy::onEventSnapshot,
             "接收事件驱动快照（需 setEventSnapshotEnabled 开启）：每个市场事件（ord/tra）"
             "全部处理（含策略响应下单/撤单）结束后从内部订单簿合成推送一次")
        .def("onCustomEvent", &Strategy::onCustomEvent,
             "接收用户自定义事件（参数为 index，PyStrategy 会自动转换为 dict）")
        .def("onOrderFilled", &Strategy::onOrderFilled)
        .def("onOrderCancelled", &Strategy::onOrderCancelled)
        .def("getStrategyId", &Strategy::getStrategyId)
        .def("setOrderLatencyMs", &Strategy::setOrderLatencyMs,
             "设置下单/撤单的交易所链路延迟（毫秒）：自动四舍五入对齐到市场最小"
             "事件粒度 10ms（14→10，15→20）。策略单延迟到 发出时刻+延迟 才进订单簿"
             "参与撮合，下单确认回调同时延迟；撤单同通道保 FIFO。"
             "须在 run_backtest 前设置；0=关闭（默认），行为与旧版一致")
        .def("getOrderLatencyMs", &Strategy::getOrderLatencyMs)
        .def("setLatencyEntryPosition", &Strategy::setLatencyEntryPosition,
             "延迟单进簿位置：release 时刻有多笔订单同时到达时，本策略单排在"
             "同时间订单的头部（LatencyEntryPosition.Head，默认）还是尾部（.Tail）")
        .def("getLatencyEntryPosition", &Strategy::getLatencyEntryPosition)
        .def("onTradeCallback", &Strategy::onTradeCallback)
        .def("onOrderCallback", &Strategy::onOrderCallback)
        .def("isProcessingComplete", &Strategy::isProcessingComplete)
        // === 用户自定义数据支持 ===
        .def("setCustomDataList", 
             [](Strategy& s, py::list data_list) {
                 // 向下转型为 PyStrategy 并设置数据列表
                 if (auto* ps = dynamic_cast<PyStrategy*>(&s)) {
                     ps->setCustomDataList(data_list);
                 }
             },
             py::arg("data_list"),
             "设置用户自定义数据列表（每个元素是一个 dict）");

    // BacktestEngine - 从CSV字符串初始化
    py::class_<BacktestEngine>(m, "BacktestEngine")
        .def(py::init<const std::string&, const std::string&, const std::string&, const std::string&, const std::string&>(),
             py::arg("symbol"), py::arg("cstick_csv"), py::arg("order_csv"), py::arg("trade_csv"), py::arg("csbar1d_csv"),
             "从CSV字符串初始化回测引擎 (DataFrame.to_csv())")
        .def(
            "registerStrategy",
            [](BacktestEngine& eng, std::shared_ptr<Strategy> s) {
                eng.registerStrategy(std::move(s));
            },
            py::arg("strategy"),
            py::keep_alive<1, 2>() // 引擎持有策略，保证生命周期
        )
        .def("getPositions", &BacktestEngine::getPositions)
        .def("getTotalPnL", &BacktestEngine::getTotalPnL)
        .def("setMarketDataCallback", &set_md_callback, py::arg("callback"),
             "Set a Python callable to receive MarketData during backtest")
        .def("processEvent", [](BacktestEngine& eng, const Event& ev) {
                 eng.processEvent(ev);
             }, py::arg("event"),
             "Process a single event (for manual event feeding)")
        .def("finish", &BacktestEngine::finish,
             "Finalize backtest and write results")
        // === 严格主动单模式（欠债限制功能）===
        .def("setStrictActiveOrderMode", &BacktestEngine::setStrictActiveOrderMode, py::arg("enabled"),
             "开启/关闭严格主动单模式：开启后，虚拟主动单吃掉的历史订单需要被真实市场消耗后才能下新的主动单")
        .def("isStrictActiveOrderMode", &BacktestEngine::isStrictActiveOrderMode,
             "检查是否开启了严格主动单模式")
        .def("hasDebt", &BacktestEngine::hasDebt,
             "检查是否有未还清的欠债（被虚拟吃掉但未被真实市场消耗的历史订单）")
        // === 真实成交替代模式 ===
        .def("setRealTradeMatchMode", &BacktestEngine::setRealTradeMatchMode, py::arg("enabled"),
             "开启/关闭真实成交替代模式：主动单自动使用严格模式，被动单使用队列位置+真实成交池匹配")
        .def("isRealTradeMatchMode", &BacktestEngine::isRealTradeMatchMode,
             "检查是否开启了真实成交替代模式")
        // === 用户下单队列回报 ===
        .def("setQueueInfoEnabled", &BacktestEngine::setQueueInfoEnabled, py::arg("enabled"),
             "开启/关闭下单回调中的队列信息字段")
        .def("isQueueInfoEnabled", &BacktestEngine::isQueueInfoEnabled,
             "检查是否开启了下单回调中的队列信息字段")
        // === 实时合成 Tick ===
        .def("setRealTimeTickInterval", &BacktestEngine::setRealTimeTickInterval, py::arg("interval_ms"),
             "设置实时合成 Tick 推送间隔（毫秒），0=关闭；开启后按间隔触发 onRealTimeTickEvent")
        .def("getRealTimeTickInterval", &BacktestEngine::getRealTimeTickInterval,
             "获取实时合成 Tick 推送间隔（毫秒，0=关闭）")
        // === 事件驱动快照 ===
        .def("setEventSnapshotEnabled", &BacktestEngine::setEventSnapshotEnabled, py::arg("enabled"),
             "开启/关闭事件驱动快照：开启后每个市场事件（ord/tra）处理完成后触发 onEventSnapshot")
        .def("isEventSnapshotEnabled", &BacktestEngine::isEventSnapshotEnabled,
             "检查事件驱动快照是否开启")
        // === 价格笼子 ===
        .def("setUserCageEnabled", &BacktestEngine::setUserCageEnabled, py::arg("enabled"),
             "开启/关闭策略单价格笼子数值判定（规则按数据日期×板块自动判定，默认开启）")
        .def("isUserCageEnabled", &BacktestEngine::isUserCageEnabled,
             "检查策略单价格笼子判定是否开启")
        .def("isPriceCageEnabled", &BacktestEngine::isPriceCageEnabled,
             "当前数据（日期×板块）规则矩阵是否启用价格笼子")
        .def("isCageInferenceActive", &BacktestEngine::isCageInferenceActive,
             "历史单消息流反推状态机是否激活（仅深市创业板 2020.8-2023.4 暂存窗口）")
        .def("getSuspendedHistoricalCount", &BacktestEngine::getSuspendedHistoricalCount,
             "当前历史笼单数量")
        .def("getUserCageCount", &BacktestEngine::getUserCageCount,
             "当前策略笼单数量");
    
    // InfoLoader - 从CSV字符串加载数据
    py::class_<InfoLoader>(m, "InfoLoader")
        .def(py::init<>())
        .def("load_sh_info", &InfoLoader::load_sh_info,
             py::arg("order_csv_content"), py::arg("trade_csv_content"), py::arg("order_book"),
             "Load Shanghai market data from CSV strings (DataFrame.to_csv())")
        .def("load_sz_info", &InfoLoader::load_sz_info,
             py::arg("order_csv_content"), py::arg("trade_csv_content"), py::arg("order_book"),
             "Load Shenzhen market data from CSV strings (DataFrame.to_csv())")
        .def("load_cstick", &InfoLoader::load_cstick,
             py::arg("csv_content"), py::arg("order_book"),
             "Load tick data from CSV string (DataFrame.to_csv())")
        .def("loadPrevClosePrice", &InfoLoader::loadPrevClosePrice,
             py::arg("csv_content"), py::arg("is_etf") = false,
             "Load previous close price from CSV string")
        .def("loadOpenPrice", &InfoLoader::loadOpenPrice,
             py::arg("csv_content"), py::arg("is_etf") = false,
             "Load open price from CSV string");

    // Convenience makers (optional)
    m.def("make_order_event",
          [](const std::string& order_id,
             const std::string& symbol,
             Direction dir,
             OrderType ot,
             Price price,
             Quantity volume,
             const std::string& strategy_id) {
              UserOrder o;
              o.order_id = order_id;
              o.symbol = symbol;
              o.direction = dir;
              o.order_type = ot;
              o.price = price;
              o.volume = volume;
              o.strategy_id = strategy_id;
              return UserEvent{o};
          },
          py::arg("order_id"), py::arg("symbol"), py::arg("direction"), py::arg("order_type"),
          py::arg("price"), py::arg("volume"), py::arg("strategy_id"));

    m.def("make_cancel_event",
          [](const std::string& order_id, const std::string& strategy_id) {
              UserCancel c(order_id, strategy_id);
              return UserEvent{c};
          },
          py::arg("order_id"), py::arg("strategy_id"));
    
    // SymbolData - 单个合约的CSV数据
    py::class_<SymbolData>(m, "SymbolData")
        .def(py::init<>())
        .def(py::init([](const std::string& symbol, const std::string& cstick_csv,
                         const std::string& order_csv, const std::string& trade_csv,
                         const std::string& csbar1d_csv, bool is_etf) {
            SymbolData data;
            data.symbol = symbol;
            data.cstick_csv = cstick_csv;
            data.order_csv = order_csv;
            data.trade_csv = trade_csv;
            data.csbar1d_csv = csbar1d_csv;
            data.is_etf = is_etf;
            return data;
        }), py::arg("symbol"), py::arg("cstick_csv"), py::arg("order_csv"), py::arg("trade_csv"), py::arg("csbar1d_csv"), py::arg("is_etf") = false)
        .def_readwrite("symbol", &SymbolData::symbol)
        .def_readwrite("cstick_csv", &SymbolData::cstick_csv)
        .def_readwrite("order_csv", &SymbolData::order_csv)
        .def_readwrite("trade_csv", &SymbolData::trade_csv)
        .def_readwrite("csbar1d_csv", &SymbolData::csbar1d_csv)
        .def_readwrite("is_etf", &SymbolData::is_etf);

    // MultiBacktestEngine - 从CSV字符串初始化
    auto mbacktest_cls = py::class_<MultiBacktestEngine>(m, "MultiBacktestEngine");
    mbacktest_cls
        .def(py::init<std::vector<SymbolData>>(),
             py::arg("symbol_data_list"),
             // 构造函数逐只解析 CSV(纯 C++ 计算),释放 GIL 让 Python 侧
             // 可以继续做下一批数据的 to_csv 转换
             py::call_guard<py::gil_scoped_release>(),
             "从多个合约的CSV字符串初始化，处理完每只后自动释放其CSV字符串")
        .def("registerStrategy",
             [](MultiBacktestEngine& eng, std::shared_ptr<Strategy> s) {
                 eng.registerStrategy(std::move(s));
             },
             py::arg("strategy"), py::keep_alive<1, 2>())
        .def("run", &MultiBacktestEngine::run,
             "运行多合约同步回测",
             py::call_guard<py::gil_scoped_release>())
        .def("getPositions", &MultiBacktestEngine::getPositions)
        .def("getTotalPnL", &MultiBacktestEngine::getTotalPnL)
        // === 严格主动单模式（欠债限制功能）===
        .def("setStrictActiveOrderMode", &MultiBacktestEngine::setStrictActiveOrderMode, py::arg("enabled"),
             "开启/关闭严格主动单模式：开启后，虚拟主动单吃掉的历史订单需要被真实市场消耗后才能下新的主动单")
        .def("isStrictActiveOrderMode", &MultiBacktestEngine::isStrictActiveOrderMode,
             "检查是否开启了严格主动单模式")
        .def("hasDebt", &MultiBacktestEngine::hasDebt,
             "检查是否有未还清的欠债（被虚拟吃掉但未被真实市场消耗的历史订单）")
        // === 真实成交替代模式 ===
        .def("setRealTradeMatchMode", &MultiBacktestEngine::setRealTradeMatchMode, py::arg("enabled"),
             "开启/关闭真实成交替代模式：主动单自动使用严格模式，被动单使用队列位置+真实成交池匹配")
        .def("isRealTradeMatchMode", &MultiBacktestEngine::isRealTradeMatchMode,
             "检查是否开启了真实成交替代模式")
        // === 用户下单队列回报 ===
        .def("setQueueInfoEnabled", &MultiBacktestEngine::setQueueInfoEnabled, py::arg("enabled"),
             "开启/关闭下单回调中的队列信息字段")
        .def("isQueueInfoEnabled", &MultiBacktestEngine::isQueueInfoEnabled,
             "检查是否开启了下单回调中的队列信息字段")
        // === 实时合成 Tick ===
        .def("setRealTimeTickInterval", &MultiBacktestEngine::setRealTimeTickInterval, py::arg("interval_ms"),
             "设置实时合成 Tick 推送间隔（毫秒），0=关闭；扇出到所有子引擎")
        .def("getRealTimeTickInterval", &MultiBacktestEngine::getRealTimeTickInterval,
             "获取实时合成 Tick 推送间隔（毫秒，0=关闭）")
        // === 事件驱动快照 ===
        .def("setEventSnapshotEnabled", &MultiBacktestEngine::setEventSnapshotEnabled, py::arg("enabled"),
             "开启/关闭事件驱动快照，扇出到所有子引擎；开启后每个市场事件处理完成后触发 onEventSnapshot")
        .def("isEventSnapshotEnabled", &MultiBacktestEngine::isEventSnapshotEnabled,
             "检查事件驱动快照是否开启")
        // === 价格笼子 ===
        .def("setUserCageEnabled", &MultiBacktestEngine::setUserCageEnabled, py::arg("enabled"),
             "开启/关闭策略单价格笼子数值判定，扇出到所有子引擎（默认开启）")
        .def("isUserCageEnabled", &MultiBacktestEngine::isUserCageEnabled,
             "检查策略单价格笼子判定是否开启")
        // === 用户自定义数据推送功能 ===
        .def("loadCustomEventTimes", &MultiBacktestEngine::loadCustomEventTimes, py::arg("datetimes"),
             "加载自定义事件时间戳列表（格式如 '2025-11-17 09:35:00'）")
        .def("setCustomDataEnabled", &MultiBacktestEngine::setCustomDataEnabled, py::arg("enabled"),
             "开启/关闭自定义数据推送功能（默认关闭）")
        .def("isCustomDataEnabled", &MultiBacktestEngine::isCustomDataEnabled,
             "检查是否开启了自定义数据推送功能");
}
