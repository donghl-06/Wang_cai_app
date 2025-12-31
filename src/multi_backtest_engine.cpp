/*
 * @Author: chenlisen
 * @Date: 2025-08-18 11:38:11
 * @LastEditTime: 2025-08-21 10:41:54
 * @FilePath: /workspace/wangcai_cpp/src/multi_backtest_engine.cpp
 */
#include "multi_backtest_engine.h"
#include "types.h"
#include <iostream>
#include <algorithm>

namespace wangcai {

MultiBacktestEngine::MultiBacktestEngine(const std::vector<SymbolData>& symbol_data_list)
{
    // 自动重置全局订单ID计数器（支持多次回测）
    reset_order_id_counter();
    
    engines_.reserve(symbol_data_list.size());

    for (const auto& data : symbol_data_list) {
        EngineManager se;
        se.symbol = data.symbol;

        se.engine = std::make_unique<BacktestEngine>(data.symbol, data.cstick_csv, data.order_csv, data.trade_csv, data.csbar1d_csv);

        // 合并所有事件到一个数组
        std::vector<Event> merged;
        merged.reserve(OrderBook::whole_events.size() + OrderBook::tick_events.size());
        
        // 直接添加所有事件
        merged.insert(merged.end(), OrderBook::whole_events.begin(), OrderBook::whole_events.end());
        merged.insert(merged.end(), OrderBook::tick_events.begin(), OrderBook::tick_events.end());
        
        // 排序逻辑：
        // 主排序键：datetime（所有事件先按时间排序）
        // 次排序键：
        //   - 相同时间时，ord/tra 优先于 tick
        //   - 相同时间且都是 ord/tra 时，按 orderid（SZ）或 bizindex（SH）排序
        //   - 相同时间且都是 tick 时，保持原顺序
        std::sort(merged.begin(), merged.end(), [](const Event& a, const Event& b) {
            // 1. 主排序键：datetime
            if (a.datetime != b.datetime) {
                return a.datetime < b.datetime;
            }
            
            // 2. datetime 相同时的次排序键
            bool a_is_tick = (a.source == "tick");
            bool b_is_tick = (b.source == "tick");
            
            // 2.1 ord/tra 优先于 tick
            if (!a_is_tick && b_is_tick) {
                return true;  // a(ord/tra) 排在 b(tick) 前面
            }
            if (a_is_tick && !b_is_tick) {
                return false; // a(tick) 排在 b(ord/tra) 后面
            }
            
            // 2.2 都是 tick：保持原顺序（返回 false 以保持稳定排序）
            if (a_is_tick && b_is_tick) {
                return false;
            }
            
            // 2.3 都是 ord/tra：按 orderid（SZ）或 bizindex（SH）排序
            if (Event::is_SZ) {
                return a.orderid < b.orderid;
            } else {
                return a.bizindex < b.bizindex;
            }
        });
        
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
    int loop_count = 0;
    while (!pq.empty()) {
        // 取出当前最小时间戳
        const auto top_item = pq.top();
        std::string current_time = top_item.datetime;
        if (current_time.substr(11, 8) > "15:00:00") {
            break;
        }
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
        
        loop_count++;

        // Taskflow 并行处理每个引擎的事件
        tf::Taskflow taskflow;
        for (const auto& [engine_idx, events] : engine_events) {
            auto events_copy = events;
            
            taskflow.emplace([this, engine_idx, events_copy]() mutable {
                auto& eng_ref = *engines_[engine_idx].engine;
                for (const auto& ev : events_copy) {
                    try {
                    eng_ref.processEvent(ev);
                    } catch (const std::exception& e) {
                        std::cerr << "❌ [processEvent失败] bizindex=" << ev.bizindex 
                                  << " datetime=" << ev.datetime 
                                  << " error=" << e.what() << std::endl;
                        // 不重新抛出异常，让其他事件继续处理
                    }
                }
            });
        }
        // 执行当前时间戳的所有任务并阻塞等待完成
        executor.run(taskflow).wait();
    }
    
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