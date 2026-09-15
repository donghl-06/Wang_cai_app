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
    // 注意:g_order_id_counter 是原子变量,下面的并行构建线程安全
    reset_order_id_counter();

    const std::size_t n = symbol_data_list.size();
    engines_.resize(n);

    // 每个合约独立构建(解析 CSV → 建簿 → 合并排序),互相无共享状态
    // (OrderBook 事件表已从静态全局改为实例成员),Taskflow 并行。
    // 4 核机器上 6~8 只一批时初始化近线性加速。
    tf::Taskflow taskflow;
    for (std::size_t i = 0; i < n; ++i) {
        taskflow.emplace([this, &symbol_data_list, i]() {
            auto& data = symbol_data_list[i];
            EngineManager se;
            se.symbol = data.symbol;

            se.engine = std::make_unique<BacktestEngine>(data.symbol, data.cstick_csv, data.order_csv, data.trade_csv, data.csbar1d_csv, data.is_etf);

            // BacktestEngine 已完成解析，立即释放 CSV 字符串节省内存
            data.cstick_csv.clear();  data.cstick_csv.shrink_to_fit();
            data.order_csv.clear();   data.order_csv.shrink_to_fit();
            data.trade_csv.clear();   data.trade_csv.shrink_to_fit();
            data.csbar1d_csv.clear(); data.csbar1d_csv.shrink_to_fit();

            // 合并所有事件到一个数组(移动语义,不再逐事件拷贝 ~700B 结构)
            auto& ob = se.engine->orderbook();
            std::vector<Event> merged;
            merged.reserve(ob.whole_events.size() + ob.tick_events.size());

            merged.insert(merged.end(),
                          std::make_move_iterator(ob.whole_events.begin()),
                          std::make_move_iterator(ob.whole_events.end()));
            merged.insert(merged.end(),
                          std::make_move_iterator(ob.tick_events.begin()),
                          std::make_move_iterator(ob.tick_events.end()));

            // 排序逻辑：
            // 主排序键：datetime（所有事件先按时间排序）
            // 次排序键：
            //   - 相同时间时，ord/tra 优先于 tick
            //   - 相同时间且都是 ord/tra 时，按 orderid（SZ）或 bizindex（SH）排序
            //   - 相同时间且都是 tick 时，保持原顺序
            std::stable_sort(merged.begin(), merged.end(), marketEventLess);

            se.events = std::move(merged);
            se.idx = 0;
            ob.clearEvents();
            ob.clearTicks();
            engines_[i] = std::move(se);
        });
    }
    tf::Executor executor;
    executor.run(taskflow).wait();
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

    // init：每个引擎推入队首事件(只放排序键+引擎号,事件本体不搬动)
    for (std::size_t i = 0; i < engines_.size(); ++i) {
        auto& se = engines_[i];
        if (se.idx < se.events.size()) {
            pq.push(QueueEvent{se.events[se.idx].datetime_ms, i});
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
        // 取出当前最小时间戳(int64)
        const int64_t current_ms = pq.top().datetime_ms;
        // 收盘 cutoff:语义与原 "HH:MM:SS" > "15:00:00" 字符串比较一致
        // (15:00:00.xxx 仍处理,15:00:01 起停止) → 当日秒数严格大于 54000
        if (current_ms / 1000 % 86400 > 54000) {
            break;
        }
        // 每个引擎 → 该时间戳的事件连续区间 [begin, end)
        // (单引擎事件已按时间排序,同时间戳事件必然连续,无需再拷贝分组)
        struct EngineRange { std::size_t engine_idx, begin, end; };
        std::vector<EngineRange> engine_ranges;
        // 当前时间戳的 datetime 字符串(供跨标的/自定义事件路由用),
        // 指向事件本体,不拷贝
        const std::string* current_time = nullptr;

        // 收集所有时间相同的引擎(每引擎一次 pop,不再每事件一次)
        while (!pq.empty() and pq.top().datetime_ms == current_ms) {
            const std::size_t eng = pq.top().engineIndex;
            pq.pop();

            auto& se = engines_[eng];
            const std::size_t begin = se.idx - 1;  // 队首事件(即刚取出的那个)
            std::size_t end = begin + 1;
            while (end < se.events.size() &&
                   se.events[end].datetime_ms == current_ms) ++end;
            engine_ranges.push_back(EngineRange{eng, begin, end});
            if (!current_time) current_time = &se.events[begin].datetime;

            // 取下一事件并放入堆(不变式:pq 条目指向 se.idx-1)
            if (end < se.events.size()) {
                pq.push(QueueEvent{se.events[end].datetime_ms, eng});
                se.idx = end + 1;
            } else {
                se.idx = end;
            }
        }

        loop_count++;

        // Taskflow 并行处理每个引擎的事件(按区间引用,零拷贝)
        tf::Taskflow taskflow;
        for (const auto& range : engine_ranges) {
            const std::size_t engine_idx = range.engine_idx;
            const auto& events = engines_[engine_idx].events;

            // === 价格笼子预扫描（仅SZ市场）===
            // 收集同一时间戳内会成交的订单ID
            std::unordered_set<int64_t> will_trade_ids;
            if (range.begin < range.end && events[range.begin].isSZ()) {
                for (std::size_t k = range.begin; k < range.end; ++k) {
                    const auto& ev = events[k];
                    if (ev.source == "tra" && ev.exectype == "1") {
                        // exectype="1" 表示成交，收集买卖双方订单ID
                        if (ev.bidorderid > 0) will_trade_ids.insert(ev.bidorderid);
                        if (ev.askorderid > 0) will_trade_ids.insert(ev.askorderid);
                    }
                }
            }

            const std::size_t begin = range.begin, end = range.end;
            taskflow.emplace([this, engine_idx, begin, end, will_trade_ids]() mutable {
                auto& eng_ref = *engines_[engine_idx].engine;
                const auto& evs = engines_[engine_idx].events;
                for (std::size_t k = begin; k < end; ++k)
                    eng_ref.processEvent(evs[k], will_trade_ids);
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
                        engines_[it->second].engine->setCurrentDatetimeForCustomEvent(*current_time);
                        engines_[it->second].engine->submitUserEvent(ue);
                    } else {
                        std::cerr << "[跨标的下单失败] 未找到合约: "
                                  << ue.order.symbol << std::endl;
                    }
                } else { // CANCEL
                    bool handled = false;
                    for (auto& se : engines_) {
                        if (se.engine->hasUserOrder(ue.cancel.order_id)) {
                            se.engine->setCurrentDatetimeForCustomEvent(*current_time);
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
                   custom_events_[custom_event_idx_].datetime <= *current_time) {
                
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
