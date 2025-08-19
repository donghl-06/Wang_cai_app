/*
 * @Author: chenlisen
 * @Date: 2025-08-18 11:38:11
 * @LastEditTime: 2025-08-19 09:13:44
 * @FilePath: /wangcai_cpp/src/multi_backtest_engine.cpp
 */
#include "multi_backtest_engine.h"

namespace wangcai {

MultiBacktestEngine::MultiBacktestEngine(const std::vector<std::string>& symbols,
                                         const std::string& date,
                                         const std::string& data_path)
    : date_(date), data_path_(data_path)
{
    engines_.reserve(symbols.size());

    for (const auto& sym : symbols) {
        EngineManager se;
        se.symbol = sym;

        se.engine = std::make_unique<BacktestEngine>(sym, date, data_path);
        std::string trade_output_file = data_path + "/backtest_trades_" + sym + "_" + date + ".csv";
        se.engine->enableTradeRecording(trade_output_file);

        // 双指针扫描所有事件 归并事件
        std::vector<Event> merged;
        merged.reserve(OrderBook::whole_events.size() + OrderBook::tick_events.size());
        auto it_ord = OrderBook::whole_events.begin();
        auto it_tick = OrderBook::tick_events.begin();
        while (it_ord != OrderBook::whole_events.end() || it_tick != OrderBook::tick_events.end()) {
            if (it_ord == OrderBook::whole_events.end()) {
                merged.push_back(*it_tick);
                ++it_tick;
            } else if (it_tick == OrderBook::tick_events.end()) {
                merged.push_back(*it_ord);
                ++it_ord;
            } else {
                if (it_tick->datetime < it_ord->datetime) {
                    merged.push_back(*it_tick);
                    ++it_tick;
                } else {
                    merged.push_back(*it_ord);
                    ++it_ord;
                }
            }
        }
        se.events = std::move(merged);
        se.idx = 0;
        OrderBook::clearEvents();
        OrderBook::clearTicks();
        engines_.push_back(std::move(se));
    }
}

void MultiBacktestEngine::registerStrategy(std::shared_ptr<Strategy> strategy) {
    strategies_.push_back(strategy);
    for (auto& se : engines_) {
        se.engine->registerStrategy(strategy);
    }
}


void MultiBacktestEngine::run() {

    std::priority_queue<QueueEvent, std::vector<QueueEvent>, std::greater<>> pq;

    // init：每个引擎推入第一个事件
    for (std::size_t i = 0; i < engines_.size(); ++i) {
        auto& se = engines_[i];
        if (se.idx < se.events.size()) {
            QueueEvent item{se.events[se.idx].datetime, i, se.events[se.idx]};
            pq.push(item);
            se.idx ++;
        }
    }

    tf::Executor executor;
    /*
        A: [09:30:01, 09:30:03, 09:30:06]
        B: [09:30:02, 09:30:03, 09:30:05]
        C: [09:30:01, 09:30:04, 09:30:06]
        处理顺序：
        9:30:01
        处理A
        处理C
        B跳过

        9:30:02
        处理B
        AC跳过

        9:30:03
        处理A
        处理B
        C跳过

        9:30:04
        处理C
        AB跳过

        9:30:05
        处理B
        AC跳过

        9:30:06
        处理A
        处理C
        B跳过
     */
    while (!pq.empty()) {
        // 取出当前最小时间戳
        const auto top_item = pq.top();
        std::string current_time = top_item.datetime;

        // 映射每个引擎索引到其在该时间戳的所有事件
        std::map<std::size_t, std::vector<Event>> engine_events;
        
        // 收集所有时间相同的事件
        while (!pq.empty() and pq.top().datetime == current_time) {
            auto item = pq.top();
            pq.pop();
            engine_events[item.engineIndex].push_back(item.event);
            // 取下一事件并放入堆
            auto& se = engines_[item.engineIndex];
            if (se.idx < se.events.size()) {
                QueueEvent next_item{se.events[se.idx].datetime, item.engineIndex, se.events[se.idx]};
                pq.push(next_item);
                se.idx ++;
            }
        }

        // Taskflow 并行处理每个引擎的事件
        tf::Taskflow taskflow;
        for (const auto& [engine_idx, events] : engine_events) {
            auto events_copy = events;
            taskflow.emplace([this, engine_idx, events_copy]() mutable {
                auto& eng_ref = *engines_[engine_idx].engine;
                for (const auto& ev : events_copy) {
                    eng_ref.processEvent(ev);
                }
            });
        }
        // 执行当前时间戳的所有任务并阻塞等待完成
        executor.run(taskflow).wait();
    }
    // 所有事件处理完成后，调用各引擎的结算
    std::ranges::for_each(engines_, [&](auto &se) {
        se.engine->finish();
    });
}

std::map<std::string, Position> MultiBacktestEngine::getPositions() const {
    std::map<std::string, Position> allpos;
    for (const auto& se : engines_) {
        auto pos = se.engine->getPositions();
        for (const auto& p : pos) {
            allpos[p.first] = p.second;
        }
    }
    return allpos;
}

double MultiBacktestEngine::getTotalPnL() const {
    double sum = 0.0;
    for (const auto& se : engines_) {
        sum += se.engine->getTotalPnL();
    }
    return sum;
}

}; // namespace wangcai