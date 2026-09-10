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
#include <unordered_map>

namespace wangcai {

MultiBacktestEngine::MultiBacktestEngine(std::vector<SymbolData> symbol_data_list)
{
    // 自动重置全局订单ID计数器（支持多次回测）
    reset_order_id_counter();
    
    engines_.reserve(symbol_data_list.size());

    for (auto& data : symbol_data_list) {
        EngineManager se;
        se.symbol = data.symbol;

        se.engine = std::make_unique<BacktestEngine>(data.symbol, data.cstick_csv, data.order_csv, data.trade_csv, data.csbar1d_csv, data.is_etf);

        // BacktestEngine 已完成解析，立即释放 CSV 字符串节省内存
        data.cstick_csv.clear();  data.cstick_csv.shrink_to_fit();
        data.order_csv.clear();   data.order_csv.shrink_to_fit();
        data.trade_csv.clear();   data.trade_csv.shrink_to_fit();
        data.csbar1d_csv.clear(); data.csbar1d_csv.shrink_to_fit();

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
        std::stable_sort(merged.begin(), merged.end(), marketEventLess);
        
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

    // 构建 symbol -> 引擎索引映射，用于自定义事件的订单路由
    std::unordered_map<std::string, std::size_t> symbol_to_engine;
    symbol_to_engine.reserve(engines_.size());
    for (std::size_t i = 0; i < engines_.size(); ++i) {
        symbol_to_engine[engines_[i].symbol] = i;
    }

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
            
            // === 价格笼子预扫描（仅SZ市场）===
            // 收集同一时间戳内会成交的订单ID
            std::unordered_set<int64_t> will_trade_ids;
            if (!events_copy.empty() && events_copy.front().isSZ()) {
                for (const auto& ev : events_copy) {
                    if (ev.source == "tra" && ev.exectype == "1") {
                        // exectype="1" 表示成交，收集买卖双方订单ID
                        if (ev.bidorderid > 0) will_trade_ids.insert(ev.bidorderid);
                        if (ev.askorderid > 0) will_trade_ids.insert(ev.askorderid);
                    }
                }
            }
            
            taskflow.emplace([this, engine_idx, events_copy, will_trade_ids]() mutable {
                auto& eng_ref = *engines_[engine_idx].engine;
                for (const auto& ev : events_copy) eng_ref.processEvent(ev, will_trade_ids);
            });
        }
        // 执行当前时间戳的所有任务并阻塞等待完成
        executor.run(taskflow).get();

        // === 跨标的用户事件汇总与路由 ===
        // 策略在本轮任意引擎的回调里返回了归属其它标的的订单/撤单，
        // 已经暂存在各子引擎的 cross_symbol_events_，这里 join 之后串行按 symbol 路由。
        {
            std::vector<UserEvent> cross_events;
            for (auto& se : engines_) {
                auto v = se.engine->drainCrossSymbolEvents();
                cross_events.insert(cross_events.end(),
                                    std::make_move_iterator(v.begin()),
                                    std::make_move_iterator(v.end()));
            }
            for (const auto& ue : cross_events) {
                if (ue.type == UserEvent::ORDER) {
                    auto it = symbol_to_engine.find(ue.order.symbol);
                    if (it != symbol_to_engine.end()) {
                        engines_[it->second].engine->setCurrentDatetimeForCustomEvent(current_time);
                        engines_[it->second].engine->submitUserEvent(ue);
                    } else {
                        std::cerr << "[跨标的下单失败] 未找到合约: "
                                  << ue.order.symbol << std::endl;
                    }
                } else { // CANCEL
                    bool handled = false;
                    for (auto& se : engines_) {
                        if (se.engine->hasUserOrder(ue.cancel.order_id)) {
                            se.engine->setCurrentDatetimeForCustomEvent(current_time);
                            se.engine->submitUserEvent(ue);
                            handled = true;
                            break;
                        }
                    }
                    if (!handled) {
                        std::cerr << "[跨标的撤单失败] 找不到订单: "
                                  << ue.cancel.order_id << std::endl;
                    }
                }
            }
        }

        // === 用户自定义数据推送 ===
        // 在当前时间戳的市场事件处理完成后，检查是否有需要推送的自定义事件
        if (custom_data_enabled_ && !custom_events_.empty()) {
            // 推送所有 datetime <= current_time 的自定义事件
            while (custom_event_idx_ < custom_events_.size() && 
                   custom_events_[custom_event_idx_].datetime <= current_time) {
                
                size_t event_index = custom_events_[custom_event_idx_].index;
                const std::string& event_time = custom_events_[custom_event_idx_].datetime;
                
                // 调用所有策略的 onCustomEvent 回调
                for (auto& strategy : strategies_) {
                    try {
                        auto user_events = strategy->onCustomEvent(event_index);
                        // 处理策略返回的下单/撤单请求
                        for (const auto& ue : user_events) {
                            if (ue.type == UserEvent::ORDER) {
                                // 下单：按 symbol 路由到对应引擎
                                auto it = symbol_to_engine.find(ue.order.symbol);
                                if (it != symbol_to_engine.end()) {
                                    engines_[it->second].engine->setCurrentDatetimeForCustomEvent(event_time);
                                    engines_[it->second].engine->submitUserEvent(ue);
                                } else {
                                    std::cerr << "❌ [自定义事件下单失败] 未找到合约: " 
                                              << ue.order.symbol << std::endl;
                                }
                            } else if (ue.type == UserEvent::CANCEL) {
                                // 撤单：在所有引擎中查找并撤单
                                bool handled = false;
                                for (auto& se : engines_) {
                                    if (se.engine->hasUserOrder(ue.cancel.order_id)) {
                                        se.engine->setCurrentDatetimeForCustomEvent(event_time);
                                        se.engine->submitUserEvent(ue);
                                        handled = true;
                                        break;
                                    }
                                }
                                if (!handled) {
                                    std::cerr << "❌ [自定义事件撤单失败] 找不到订单: " 
                                              << ue.cancel.order_id << std::endl;
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "❌ [onCustomEvent失败] index=" << event_index 
                                  << " error=" << e.what() << std::endl;
                    }
                }
                
                custom_event_idx_++;
            }
        }
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

// === 严格主动单模式（欠债限制功能）===
void MultiBacktestEngine::setStrictActiveOrderMode(bool enabled) {
    for (auto& se : engines_) {
        se.engine->setStrictActiveOrderMode(enabled);
    }
}

bool MultiBacktestEngine::isStrictActiveOrderMode() const {
    if (!engines_.empty()) {
        return engines_[0].engine->isStrictActiveOrderMode();
    }
    return false;
}

bool MultiBacktestEngine::hasDebt() const {
    for (const auto& se : engines_) {
        if (se.engine->hasDebt()) {
            return true;
        }
    }
    return false;
}

// === 真实成交替代模式 ===
void MultiBacktestEngine::setRealTradeMatchMode(bool enabled) {
    for (auto& se : engines_) {
        se.engine->setRealTradeMatchMode(enabled);
    }
}

bool MultiBacktestEngine::isRealTradeMatchMode() const {
    if (!engines_.empty()) {
        return engines_[0].engine->isRealTradeMatchMode();
    }
    return false;
}

void MultiBacktestEngine::setQueueInfoEnabled(bool enabled) {
    queue_info_enabled_ = enabled;
    for (auto& se : engines_) {
        se.engine->setQueueInfoEnabled(enabled);
    }
}

bool MultiBacktestEngine::isQueueInfoEnabled() const {
    if (!engines_.empty()) {
        return engines_[0].engine->isQueueInfoEnabled();
    }
    return queue_info_enabled_;
}

void MultiBacktestEngine::setRealTimeTickInterval(int interval_ms) {
    realtime_tick_interval_ms_ = interval_ms > 0 ? interval_ms : 0;
    for (auto& se : engines_) {
        se.engine->setRealTimeTickInterval(realtime_tick_interval_ms_);
    }
}

int MultiBacktestEngine::getRealTimeTickInterval() const {
    if (!engines_.empty()) {
        return engines_[0].engine->getRealTimeTickInterval();
    }
    return realtime_tick_interval_ms_;
}

void MultiBacktestEngine::setEventSnapshotEnabled(bool enabled) {
    event_snapshot_enabled_ = enabled;
    for (auto& se : engines_) {
        se.engine->setEventSnapshotEnabled(enabled);
    }
}

bool MultiBacktestEngine::isEventSnapshotEnabled() const {
    if (!engines_.empty()) {
        return engines_[0].engine->isEventSnapshotEnabled();
    }
    return event_snapshot_enabled_;
}

void MultiBacktestEngine::setUserCageEnabled(bool enabled) {
    for (auto& se : engines_) {
        se.engine->setUserCageEnabled(enabled);
    }
}

bool MultiBacktestEngine::isUserCageEnabled() const {
    if (!engines_.empty()) {
        return engines_[0].engine->isUserCageEnabled();
    }
    return true;
}

// === 用户自定义数据推送功能 ===
void MultiBacktestEngine::loadCustomEventTimes(const std::vector<std::string>& datetimes) {
    custom_events_.clear();
    custom_events_.reserve(datetimes.size());
    
    // 将时间戳列表转换为 CustomEventTime 对象，并记录索引
    for (size_t i = 0; i < datetimes.size(); ++i) {
        custom_events_.emplace_back(datetimes[i], i);
    }
    
    // 按时间戳排序（确保按时间顺序推送）
    std::sort(custom_events_.begin(), custom_events_.end(), 
              [](const CustomEventTime& a, const CustomEventTime& b) {
                  if (a.datetime != b.datetime) {
                      return a.datetime < b.datetime;
                  }
                  // 时间相同保持原始输入顺序
                  return a.index < b.index;
              });
    
    // 重置索引
    custom_event_idx_ = 0;
}

}; // namespace wangcai
