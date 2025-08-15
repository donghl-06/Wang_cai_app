#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

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
    std::vector<UserEvent> onOrderEvent(const Event& event) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onOrderEvent")) {
            try {
                py::object ret = f(event);
                return ret.cast<std::vector<UserEvent>>();
            } catch (const py::error_already_set& e) {
                py::print("[Strategy.onOrderEvent] exception:", e.what());
            }
        }
        return {};
    }

    std::vector<UserEvent> onTradeEvent(const Execution& execution, const std::string& datetime) override {
        py::gil_scoped_acquire gil;
        if (py::function f = py::get_override(this, "onTradeEvent")) {
            try {
                py::object ret = f(execution, datetime);
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

    // --------- Minimal type registrations to allow argument conversion ---------
    py::class_<Snapshot>(m, "Snapshot")          
    .def_readwrite("Instrument", &Snapshot::Instrument)
    .def_readwrite("datetime", &Snapshot::datetime)
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
    .def_readwrite("Iopv", &Snapshot::Iopv);


    py::class_<TradeCallback>(m, "TradeCallback");  // 同上
    py::class_<OrderCallback>(m, "OrderCallback");  // 同上

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

    // Event (minimal fields needed for onOrderEvent)
    py::class_<Event>(m, "Event")
        .def_property_readonly("datetime", [](const Event& e){ return e.datetime; })
        .def_property_readonly("sym", [](const Event& e){ return e.sym; })
        .def_property_readonly("price", [](const Event& e){ return e.price; })
        .def_property_readonly("size", [](const Event& e){ return e.size; })
        .def_property_readonly("side", [](const Event& e){ return e.side; })
        .def_property_readonly("ordertype", [](const Event& e){ return e.ordertype; });

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
        .def("run", &BacktestEngine::run,
             "Run backtest.",
             py::call_guard<py::gil_scoped_release>())
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
}