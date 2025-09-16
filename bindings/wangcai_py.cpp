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

    // 事件回调：若 Python 未实现，则返回空列表，避免崩溃
    std::vector<UserEvent> onOrderEvent(const OrderDetail& order) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onOrderEvent")) {
            try {
                py::object ret = f(order);
                return ret.cast<std::vector<UserEvent>>();
            } catch (const py::error_already_set& e) {
                py::print("[Strategy.onOrderEvent] exception:", e.what());
            }
        }
        return {};
    }

    std::vector<UserEvent> onTradeEvent(const TradeDetail& trade) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onTradeEvent")) {
            try {
                py::object ret = f(trade);
                return ret.cast<std::vector<UserEvent>>();
            } catch (const py::error_already_set& e) {
                py::print("[Strategy.onTradeEvent] exception:", e.what());
            }
        }
        return {};
    }

    std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onTickEvent")) {
            try {
                py::object ret = f(snapshot);
                return ret.cast<std::vector<UserEvent>>();
            } catch (const py::error_already_set& e) {
                py::print("[Strategy.onTickEvent] exception:", e.what());
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
    .def_readwrite("orderlocalid", &OrderCallback::orderlocalid);


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

    // Execution (read-only)
    py::class_<Execution>(m, "Execution")
        .def_property_readonly("execution_id", [](const Execution& e){ return e.execution_id; })
        .def_property_readonly("buy_order_id", [](const Execution& e){ return e.buy_order_id; })
        .def_property_readonly("sell_order_id", [](const Execution& e){ return e.sell_order_id; })
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
        .def("onOrderFilled", &Strategy::onOrderFilled)
        .def("onOrderCancelled", &Strategy::onOrderCancelled)
        .def("getStrategyId", &Strategy::getStrategyId)
        .def("onTradeCallback", &Strategy::onTradeCallback)
        .def("onOrderCallback", &Strategy::onOrderCallback);

    // BacktestEngine
    py::class_<BacktestEngine>(m, "BacktestEngine")
        .def(py::init<const std::string&, const std::string&, const std::string&>(),
             py::arg("symbol"), py::arg("date"), py::arg("data_path"))
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
             "Set a Python callable to receive MarketData during backtest");

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
    
    auto mbacktest_cls = py::class_<MultiBacktestEngine>(m, "MultiBacktestEngine");
    mbacktest_cls
        .def(py::init<const std::vector<std::string>&, const std::string&, const std::string&>(),
             py::arg("symbols"), py::arg("date"), py::arg("data_path"))
        .def("registerStrategy",
             [](MultiBacktestEngine& eng, std::shared_ptr<Strategy> s) {
                 eng.registerStrategy(std::move(s));
             },
             py::arg("strategy"), py::keep_alive<1, 2>())
        .def("run", &MultiBacktestEngine::run,
             "运行多合约同步回测",
             py::call_guard<py::gil_scoped_release>())
        .def("getPositions", &MultiBacktestEngine::getPositions)
        .def("getTotalPnL", &MultiBacktestEngine::getTotalPnL);
}