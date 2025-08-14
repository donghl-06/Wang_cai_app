#include "../include/backtest_engine.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <fstream> // Added for file operations
#include <ranges>
namespace wangcai {
static OrderType toOrderType(const Event& ev) {
    // 仅深市有 1/2/3 的区分；沪市全用限价
    if (ev.sym.size() >= 2 && ev.sym.substr(ev.sym.size()-2) == "SZ") {
        switch (ev.ordertype) {
            case 1: return OrderType::Market;
           case 2: return OrderType::Limit;
            case 3: return OrderType::BestOwn;
            default: return OrderType::Limit;
        }
    }
    return OrderType::Limit;
}

// 构造函数，初始化回测引擎，设置合约、日期、数据路径等基本参数
BacktestEngine::BacktestEngine(const std::string& symbol, const std::string& date, const std::string& data_path)
    : symbol_(symbol), date_(date), data_path_(data_path), continuous_mode_(false), 
      next_order_id_(1000000), next_trade_id_(1000000), recording_enabled_(false)
{
    initialize();
}

// 初始化函数，完成回测环境的所有准备工作
void BacktestEngine::initialize() {
    // 1. 读取前收盘价和真实开盘价
    std::string cstick_file = data_path_ + "/cstick_" + symbol_ + "_" + date_ + ".csv";
    prev_close_ = loadPrevClosePrice(cstick_file);
    actual_open_ = loadOpenPrice(cstick_file);
    if (symbol_.find("SZ") != std::string::npos) {
        Event::is_SZ = true;
    }

    if (prev_close_ == 0) {
        // 如果前收盘价读取失败，抛出异常
        throw std::runtime_error("无法读取前收盘价");
    }
    
    // 2. 计算涨跌停价格（四舍五入到分，单位为厘）
    double upper_raw = prev_close_ * 1.1 / 10000.0; // 涨停价（元）
    double lower_raw = prev_close_ * 0.9 / 10000.0; // 跌停价（元）
    double upper = std::round(upper_raw * 100) / 100.0; // 四舍五入到分
    double lower = std::floor(lower_raw * 100) / 100.0; // 向下取整到分
    upper_limit_ = static_cast<Price>(upper * 10000); // 转回厘
    lower_limit_ = static_cast<Price>(lower * 10000);

    // 3. 初始化订单簿，注册成交回调
    orderbook_ = std::make_unique<OrderBook>(upper_limit_, lower_limit_, false,
        [this](const Execution& ex) {
            std::string trade_datetime = continuous_mode_ ? last_brk_datetime_ : current_datetime_;
            
            // 更新最新成交价（供后续集合竞价使用）
            orderbook_->setLastTradePrice(ex.price);
            
            // 检查是否涉及虚拟订单（通过虚拟订单映射）
            bool buy_is_virtual = virtual_order_strategy_.count(ex.buy_order_id) > 0;
            bool sell_is_virtual = virtual_order_strategy_.count(ex.sell_order_id) > 0;
            
            std::string virtual_strategy_id;
            std::string virtual_order_id;
            
            if (buy_is_virtual) {
                virtual_strategy_id = virtual_order_strategy_[ex.buy_order_id];
                virtual_order_id = virtual_order_local_id_[ex.buy_order_id];
            } else if (sell_is_virtual) {
                virtual_strategy_id = virtual_order_strategy_[ex.sell_order_id];
                virtual_order_id = virtual_order_local_id_[ex.sell_order_id];
            }
            
            if (buy_is_virtual || sell_is_virtual) {
                // 虚拟成交：只通知对应策略，不记录到CSV
                for (auto& strategy : strategies_) {
                    if (strategy->getStrategyId() == virtual_strategy_id) {
                        strategy->onOrderFilled(virtual_order_id, ex.price, ex.volume);
                        std::cout << "[虚拟成交] 策略 " << virtual_strategy_id 
                                  << " 订单 " << virtual_order_id 
                                  << " 成交 " << ex.volume << "@" << ex.price / 10000.0 << std::endl;
                        break;
                    }
                }
                
                // 成交后清理虚拟订单映射
                if (buy_is_virtual) {
                    virtual_order_strategy_.erase(ex.buy_order_id);
                    virtual_order_local_id_.erase(ex.buy_order_id);
                }
                if (sell_is_virtual) {
                    virtual_order_strategy_.erase(ex.sell_order_id);
                    virtual_order_local_id_.erase(ex.sell_order_id);
                }
                
                // 注意：虚拟成交不记录到CSV，不调用recordTrade()
            } else {
                // 历史订单成交：正常处理并记录到CSV
                
                // 1. 检查并通知策略订单成交
                notifyStrategiesOnExecution(ex);
                
                // 2. 推送成交事件给所有策略，收集新事件（暂存到pending队列）
                for (auto& strategy : strategies_) {
                    auto user_events = strategy->onTradeEvent(ex, trade_datetime);
                    for (const auto& event : user_events) {
                        pending_trade_events_.push_back(event);
                    }
                }
                
                // 3. 记录交易信息（如果启用了记录）- 只记录历史订单成交
                if (recording_enabled_) {
                    recordTrade(ex, trade_datetime);
                }
            }
        });
    
    // 设置前收盘价和交易所代码
    orderbook_->setPrevClosePrice(prev_close_);
    orderbook_->setExchange(symbol_.substr(symbol_.size() - 2)); // 取合约最后两位作为交易所代码

    // 4. 初始化集合竞价撮合引擎
    call_engine_ = std::make_unique<CallAuctionEngine>(*orderbook_, prev_close_, 
                                                       orderbook_->getExchange(),
                                                       nullptr, // 价格回调
                                                       [this](uint64_t order_id, bool success, const std::string& reason, 
                                                              std::shared_ptr<Order> order_info) {
                                                           // 集合竞价撤单回调
                                                           if (success && recording_enabled_ && order_info) {
                                                               uint64_t original_id = orderbook_->getOriginalOrderId(order_id);
                                                               recordCancelWithOrderInfo(original_id, current_datetime_, order_info);
                                                           }
                                                       });
    
    // 5. 初始化连续竞价撮合引擎，注册撤单回调和虚拟成交回调
    MarketType market_type = (orderbook_->getExchange() == "SH") ? MarketType::SH : MarketType::SZ;
    con_engine_ = std::make_unique<ConAuctionEngine>(*orderbook_, market_type,
        [this](uint64_t order_id, bool success, const std::string& reason, 
               std::shared_ptr<Order> order_info) {
            // 撤单回调，区分虚拟订单和历史订单
            if (success && order_info) {
                if (order_info->broker == "BRK") {
                    // 历史订单撤单：只记录撤单到CSV，不通知策略
                    if (recording_enabled_) {
                        uint64_t original_id = orderbook_->getOriginalOrderId(order_id);
                        recordCancelWithOrderInfo(original_id, continuous_mode_ ? last_brk_datetime_ : current_datetime_, order_info);
                    }
                    // 注意：历史订单撤单不通知策略，因为策略不应该关心历史订单的撤单
                } else {
                    // 虚拟订单撤单：只通知对应策略，不记录到CSV
                    for (auto& strategy : strategies_) {
                        if (strategy->getStrategyId() == order_info->account) {
                            strategy->onOrderCancelled(order_info->order_local_id, reason);
                            std::cout << "[虚拟撤单] 策略 " << order_info->account 
                                      << " 订单 " << order_info->order_local_id 
                                      << " 已撤单，原因: " << reason << std::endl;
                            break;
                        }
                    }
                    // 注意：虚拟订单撤单不记录到CSV，不调用recordCancel()
                }
            }
        });

    // 6. 初始化收盘集合竞价撮合引擎（逻辑基本同开盘集合竞价）
    close_engine_ = std::make_unique<CloseAuctionEngine>(*orderbook_, prev_close_,
                                                        orderbook_->getExchange(),
                                                        nullptr, // 价格回调
                                                        [this](uint64_t order_id, bool success, const std::string& reason,
                                                               std::shared_ptr<Order> order_info) {
                                                            if (success && recording_enabled_ && order_info) {
                                                                uint64_t original_id = orderbook_->getOriginalOrderId(order_id);
                                                                recordCancelWithOrderInfo(original_id, current_datetime_, order_info);
                                                            }
                                                        });


    data_manager_ = std::make_unique<DataManager>(orderbook_.get(), call_engine_.get(), con_engine_.get(), close_engine_.get());
    // 7. 加载历史订单和成交数据，合并为事件流
    OrderBook::clearEvents(); // 清空事件
    OrderBook::clearTicks(); // 清空tick事件
    std::string ord_file = data_path_ + "/csord_" + symbol_ + "_" + date_ + ".csv";
    std::string tra_file = data_path_ + "/cstra_" + symbol_ + "_" + date_ + ".csv";
    load_orders_from_csv(ord_file, *orderbook_);
    load_traders_from_csv(tra_file, *orderbook_);
    if (Event::is_SZ) {
        std::sort(OrderBook::whole_events.begin(), OrderBook::whole_events.end(), [&](auto a, auto b) {
            return a.orderid < b.orderid; // SZ
        });
    } else {
        std::sort(OrderBook::whole_events.begin(), OrderBook::whole_events.end(), [&](auto a, auto b) {
            return a.bizindex < b.bizindex; // SH
        });
    }
    
    load_cstick_from_csv(cstick_file, *orderbook_);
    // std::vector<Event> tmp;
    // auto record_it = OrderBook::whole_events.begin();
    // auto tick_it = OrderBook::tick_events.begin();

    // while (record_it != OrderBook::whole_events.end() and tick_it != OrderBook::tick_events.end()) {
    //     if (tick_it->datetime < record_it->datetime) {
    //         tmp.push_back(*tick_it);
    //         tick_it ++;
    //     } else {
    //         tmp.push_back(*record_it);
    //         record_it ++;
    //     }
    // }
    // tmp.insert(tmp.end(), record_it, OrderBook::whole_events.end());
    // tmp.insert(tmp.end(), tick_it, OrderBook::tick_events.end());
    // OrderBook::whole_events = std::move(tmp);

    size_t total_events = OrderBook::whole_events.size() + OrderBook::tick_events.size();
    std::cout << "加载了 " << total_events << " 个历史事件" << std::endl;
}

// 注册策略，支持多策略回测
void BacktestEngine::registerStrategy(std::shared_ptr<Strategy> strategy) {
    strategies_.push_back(strategy);
    // 为每个策略初始化持仓映射
    positions_[strategy->getStrategyId()] = std::map<std::string, Position>();
}

// 回测主循环，实现简单的同步事件处理
void BacktestEngine::run() {
    std::cout << "开始同步交互式回测 " << symbol_ << " " << date_ << std::endl;
    
    static constexpr const char* Call_Open_Time = "09:25:00";
    static constexpr const char* Call_Close_Time = "14:57:00";
    
    auto order_it = OrderBook::whole_events.begin();
    auto tick_it = OrderBook::tick_events.begin();
    
    while (order_it != OrderBook::whole_events.end() || 
           tick_it != OrderBook::tick_events.end()) {
        
        const Event* ev = nullptr;
        
        if (order_it == OrderBook::whole_events.end()) {
            ev = &(*tick_it);
            ++tick_it;
        } else if (tick_it == OrderBook::tick_events.end()) {
            ev = &(*order_it);
            ++order_it;
        } else {
            if (tick_it->datetime < order_it->datetime) {
                ev = &(*tick_it);
                ++tick_it;
            } else {
                ev = &(*order_it);
                ++order_it;
            }
        }
        
        current_datetime_ = ev->datetime;
        
        // Step 1: 推送订单事件给策略
        std::vector<UserEvent> strategy_events;
        
        // 收集所有策略的事件响应
        for (auto& strategy : strategies_) {
            auto user_events = strategy->onOrderEvent(*ev);
            for (const auto& event : user_events) {
                strategy_events.push_back(event);
            }
        }
        
        // Step 2: 处理当前历史事件
        if (!continuous_mode_) {
            // 集合竞价阶段
            std::string tm_cur = ev->datetime.substr(11, 8);
            
            if (ev->source == "ord") {
                Direction dir = (ev->side == 1 ? Direction::Buy : Direction::Sell);
                auto ord = orderbook_->createOrder("BRK", "AC", orderbook_->getExchange(), 
                                                    ev->sym, std::to_string(ev->orderid),
                                                    toOrderType(*ev), dir, ev->price, ev->size, ev->bizindex);
                call_engine_->accept(ord);
                data_manager_->updateOrderDetail(*ev, 'A');
            } else if (ev->source == "tra") {
                uint64_t oid_raw = ev->bidorderid ? ev->bidorderid : ev->askorderid;
                call_engine_->cancel_by_input_id(oid_raw);
            } else {
                // tick事件
                data_manager_->updateSnapshot(*ev);
                auto snapshot = data_manager_->getSnapshot();
                for (auto& strategy : strategies_) {
                    auto user_events = strategy->onTickEvent(snapshot);
                    for (const auto& event : user_events) {
                        pending_trade_events_.push_back(event);
                    }
                }
            }
            
            // 检查是否结束集合竞价
            if (tm_cur >= Call_Open_Time) {
                std::string auction_time = ev->datetime.substr(0, 11) + "09:25:00.000";
                current_datetime_ = auction_time;
                
                call_engine_->settle();
                continuous_mode_ = true;
                std::cout << "[09:25] 集合竞价完成，开盘价=" << call_engine_->getPredictPrice() / 10000.0 
                          << " 真实开盘价=" << actual_open_ / 10000.0
                          << " 开盘成交量=" << call_engine_->getPredictVolume() << std::endl;
                std::cout << "连续竞价开始" << std::endl;
                
                current_datetime_ = ev->datetime;
            }
        } else {
            // 连续竞价阶段的处理逻辑（类似修改）
            std::string tm_cur = ev->datetime.substr(11,8);
            if (!closing_mode_ && tm_cur >= Call_Close_Time) {
                closing_mode_ = true;
                close_engine_->bootstrap_from_orderbook();
                std::cout << "[" << tm_cur << "] 进入收盘集合竞价阶段" << std::endl;
            }

            if (!closing_mode_) {
                // 普通连续竞价处理
                if (ev->source == "ord") {
                    Direction dir = (ev->side == 1 ? Direction::Buy : Direction::Sell);
                    auto ord = orderbook_->createOrder("BRK", "AC", orderbook_->getExchange(),
                                                        ev->sym, std::to_string(ev->orderid),
                                                        toOrderType(*ev), dir, ev->price, ev->size, ev->bizindex);
                    con_engine_->accept(ord);
                    data_manager_->updateOrderDetail(*ev, 'A');
                    last_brk_datetime_ = ev->datetime;
                } else if (ev->source == "tra") {
                    uint64_t oid_raw = ev->bidorderid ? ev->bidorderid : ev->askorderid;
                    con_engine_->cancel_by_input_id(oid_raw);
                } else {
                    // tick事件
                    data_manager_->updateSnapshot(*ev);
                    auto snapshot = data_manager_->getSnapshot();
                    for (auto& strategy : strategies_) {
                        auto user_events = strategy->onTickEvent(snapshot);
                        for (const auto& event : user_events) {
                            pending_trade_events_.push_back(event);
                        }
                    }
                }
            } else {
                // 收盘集合竞价期间的处理（类似修改）
                if (ev->source == "ord") {
                    Direction dir = (ev->side == 1 ? Direction::Buy : Direction::Sell);
                    auto ord = orderbook_->createOrder("BRK", "AC", orderbook_->getExchange(),
                                                        ev->sym, std::to_string(ev->orderid),
                                                        toOrderType(*ev), dir, ev->price, ev->size, ev->bizindex);
                    close_engine_->accept(ord);
                    data_manager_->updateOrderDetail(*ev, 'A');
                } else if (ev->source == "tra") {
                    uint64_t oid_raw = ev->bidorderid ? ev->bidorderid : ev->askorderid;
                    close_engine_->cancel_by_input_id(oid_raw);
                } else {
                    // tick事件
                    data_manager_->updateSnapshot(*ev);
                    auto snapshot = data_manager_->getSnapshot();
                    for (auto& strategy : strategies_) {
                        auto user_events = strategy->onTickEvent(snapshot);
                        for (const auto& event : user_events) {
                            pending_trade_events_.push_back(event);
                        }
                    }
                }
            }
        }
        
        // Step 3: 处理策略事件
        for (const auto& user_event : strategy_events) {
            if (continuous_mode_) {
                processUserEvent(user_event);
            }
        }
        
        // Step 4: 处理成交事件产生的策略事件
        for (const auto& user_event : pending_trade_events_) {
            if (continuous_mode_) {
                processUserEvent(user_event);
            }
        }
        pending_trade_events_.clear();
        strategy_events.clear();
    }
    // 收盘集合竞价结算
    if (closing_mode_) {
        close_engine_->settle();
        std::cout << "[收盘集合竞价] 成交价=" << close_engine_->getPredictPrice() / 10000.0
                  << " 成交量=" << close_engine_->getPredictVolume() << std::endl;
    }

    std::cout << "同步交互式回测完成" << std::endl;
    
    // 输出交易记录
    if (recording_enabled_) {
        writeTradeRecords();
        std::cout << "交易记录已输出到: " << trade_output_file_ << std::endl;
        std::cout << "总交易笔数: " << trade_records_.size() << std::endl;
    }
    
    printResults();
}

// 尝试立即成交USER订单
bool BacktestEngine::tryFillImmediately(std::shared_ptr<Order> user_order) {
    bool is_buy = user_order->direction == Direction::Buy;
    Price best_opp_price = is_buy ? orderbook_->bestAsk() : orderbook_->bestBid();
    
    // 检查是否可以立即成交
    bool can_fill = false;
    if (is_buy && best_opp_price > 0 && user_order->price >= best_opp_price) {
        can_fill = true;
    } else if (!is_buy && best_opp_price > 0 && user_order->price <= best_opp_price) {
        can_fill = true;
    }
    
    if (can_fill) {
        // 立即全部成交
        Price fill_price = best_opp_price;
        Quantity fill_qty = user_order->volume;
        
        user_order->traded_volume = fill_qty;
        user_order->status = OrderStatus::Filled;
        
        // 虚拟成交只通知对应策略，不触发其他回调
        // 找到对应的策略并通知成交
        for (auto& strategy : strategies_) {
            if (strategy->getStrategyId() == user_order->account) { // account字段存储的是strategy_id
                strategy->onOrderFilled(user_order->order_local_id, fill_price, fill_qty);
                break;
            }
        }
        
        std::cout << "[虚拟成交] USER订单 " << user_order->order_local_id 
                  << " 立即成交 " << fill_qty << "@" << fill_price / 10000.0 << std::endl;
        return true;
    }
    
    return false;
}


// 处理用户策略下发的订单，仅在连续竞价阶段允许
void BacktestEngine::processUserOrder(const UserOrder& user_order) {
    if (!continuous_mode_) {
        std::cout << "集合竞价期间不允许用户下单" << std::endl;
        return;
    }
    
    try {
        // 创建订单并送入连续竞价撮合引擎
        auto order = orderbook_->createOrder(
            "USER", user_order.strategy_id, orderbook_->getExchange(),
            user_order.symbol, user_order.order_id,
            user_order.order_type, user_order.direction,
            user_order.price, user_order.volume, next_order_id_++
        );
        
        // 记录用户订单号与系统订单号的映射
        user_order_mapping_[user_order.order_id] = order->order_id;
        
        // 记录虚拟订单映射，用于成交回调时识别
        virtual_order_strategy_[order->order_id] = user_order.strategy_id;
        virtual_order_local_id_[order->order_id] = user_order.order_id;
        
        // 先尝试立即成交
        if (!tryFillImmediately(order)) {
            // 如果无法立即成交，则送入撮合引擎
            con_engine_->accept(order);
        }
        
    } catch (const std::exception& e) {
        std::cout << "用户订单处理失败: " << e.what() << std::endl;
    }
}

// 更新策略持仓信息，包括开仓、加仓、平仓、反向开仓等多种情况
void BacktestEngine::updatePosition(const std::string& strategy_id, const std::string& symbol,
                                   Direction direction, Quantity volume, Price price) {
    auto& position = positions_[strategy_id][symbol];
    
    if (position.quantity == 0) {
        // 新开仓
        position.symbol = symbol;
        position.quantity = (direction == Direction::Buy) ? volume : -volume;
        position.avg_cost = price / 10000.0;
    } else {
        // 已有持仓，需判断是加仓还是平仓
        int64_t trade_volume = (direction == Direction::Buy) ? volume : -volume;
        
        if ((position.quantity > 0 && trade_volume > 0) || (position.quantity < 0 && trade_volume < 0)) {
            // 同方向加仓，更新加权平均成本
            double total_cost = position.avg_cost * std::abs(position.quantity) + (price / 10000.0) * volume;
            position.quantity += trade_volume;
            position.avg_cost = total_cost / std::abs(position.quantity);
        } else {
            // 反方向平仓或反向开仓
            int64_t abs_pos = std::abs(position.quantity);
            int64_t abs_trade = std::abs(trade_volume);
            
            if (abs_trade >= abs_pos) {
                // 完全平仓或反向开仓
                position.realized_pnl += (price / 10000.0 - position.avg_cost) * 
                                        std::min(abs_pos, abs_trade) * 
                                        (position.quantity > 0 ? 1 : -1);
                
                if (abs_trade > abs_pos) {
                    // 反向开仓，更新新方向和成本
                    position.quantity = trade_volume + position.quantity;
                    position.avg_cost = price / 10000.0;
                } else {
                    // 完全平仓，持仓归零
                    position.quantity = 0;
                    position.avg_cost = 0;
                }
            } else {
                // 部分平仓
                position.realized_pnl += (price / 10000.0 - position.avg_cost) * abs_trade * 
                                        (position.quantity > 0 ? 1 : -1);
                position.quantity += trade_volume;
            }
        }
    }
}

// 获取所有策略的持仓快照
std::map<std::string, Position> BacktestEngine::getPositions() const {
    std::map<std::string, Position> all_positions;
    for (const auto& strategy_pos : positions_) {
        for (const auto& pos : strategy_pos.second) {
            all_positions[strategy_pos.first + "_" + pos.first] = pos.second;
        }
    }
    return all_positions;
}

// 计算所有策略的总盈亏，包括已实现和未实现部分
double BacktestEngine::getTotalPnL() const {
    double total_pnl = 0.0;
    for (const auto& strategy_pos : positions_) {
        for (const auto& pos : strategy_pos.second) {
            total_pnl += pos.second.realized_pnl;
            // 加上未实现盈亏（以当前买一价估算）
            if (pos.second.quantity != 0) {
                double current_price = orderbook_->bestBid() / 10000.0; // 简化处理
                total_pnl += (current_price - pos.second.avg_cost) * pos.second.quantity;
            }
        }
    }
    return total_pnl;
}

// 打印所有策略的回测结果，包括每个合约的持仓、成本、盈亏等
void BacktestEngine::printResults() const {
    std::cout << "\n========== 回测结果 ==========" << std::endl;
    
    for (const auto& strategy_pos : positions_) {
        std::cout << "\n策略: " << strategy_pos.first << std::endl;
        
        double strategy_pnl = 0.0;
        for (const auto& pos : strategy_pos.second) {
            const Position& position = pos.second;
            std::cout << "  持仓 " << position.symbol << ": "
                     << "数量=" << position.quantity
                     << ", 成本=" << std::fixed << std::setprecision(4) << position.avg_cost
                     << ", 已实现盈亏=" << position.realized_pnl << std::endl;
            
            strategy_pnl += position.realized_pnl;
            // 统计未实现盈亏
            if (position.quantity != 0) {
                double current_price = orderbook_->bestBid() / 10000.0;
                strategy_pnl += (current_price - position.avg_cost) * position.quantity;
            }
        }
        
        std::cout << "  策略总盈亏: " << std::fixed << std::setprecision(2) << strategy_pnl << std::endl;
    }
    
    std::cout << "\n总盈亏: " << std::fixed << std::setprecision(2) << getTotalPnL() << std::endl;
    std::cout << "=================================" << std::endl;
}

// 设置市场数据回调函数（可用于外部实时监控）
void BacktestEngine::setMarketDataCallback(std::function<void(const MarketData&)> callback) {
    market_data_callback_ = std::move(callback);
}

// 启用交易记录功能
void BacktestEngine::enableTradeRecording(const std::string& output_file) {
    trade_output_file_ = output_file;
    recording_enabled_ = true;
    trade_records_.clear();
    next_trade_id_ = 1000000;  // 重置交易ID
}

// 记录单笔交易信息
void BacktestEngine::recordTrade(const Execution& ex, const std::string& datetime) {
    // 获取原始输入订单ID
    uint64_t buy_input_id = orderbook_->getOriginalOrderId(ex.buy_order_id);
    uint64_t sell_input_id = orderbook_->getOriginalOrderId(ex.sell_order_id);

    TradeRecord record(
        datetime,                           // 交易时间
        symbol_,                           // 合约代码
        ex.price / 10000.0,               // 成交价格（转换为元）
        static_cast<double>(ex.volume),    // 成交数量
        buy_input_id,                      // 买方原始订单ID
        sell_input_id,                     // 卖方原始订单ID
        next_trade_id_++,                  // 交易ID
        1,                                 // exectype=1（正常成交）
        " ",                               // tradebsflag（空格）
        2012,                              // channelno
        0                                  // bizindex
    );

    trade_records_.push_back(record);
}

// 记录撤单信息
void BacktestEngine::recordCancel(uint64_t order_id, const std::string& datetime) {
    if (!recording_enabled_) return;

    // 从订单簿中查找原始订单信息
    auto order = orderbook_->getOrder(order_id);
    if (!order) return;  // 找不到订单信息，不记录


    bool is_buy = order->direction == Direction::Buy;

    TradeRecord record(
        datetime,                           // 撤单时间
        symbol_,                           // 合约代码
        order->price / 10000.0,                               // 撤单价格为撤单订单价格
        static_cast<double>(order->remaining_volume()),  // 撤单数量为剩余未成交量
        is_buy ? order_id : 0,             // 买单撤单时填bid
        is_buy ? 0 : order_id,             // 卖单撤单时填ask
        next_trade_id_++,                  // 交易ID
        2,                                 // exectype=2（撤单）
        " ",                               // tradebsflag（空格）
        2012,                              // channelno
        0                                  // bizindex
    );

    trade_records_.push_back(record);
}

// 记录撤单信息，包含订单详细信息
void BacktestEngine::recordCancelWithOrderInfo(uint64_t original_id, const std::string& datetime, 
                                              std::shared_ptr<Order> order_info) {
    if (!recording_enabled_ || !order_info) return;
    
    bool is_buy = order_info->direction == Direction::Buy;
    
    TradeRecord record(
        datetime,                           // 撤单时间
        symbol_,                           // 合约代码
        0.0,                               // 撤单价格为0
        static_cast<double>(order_info->remaining_volume()),  // 撤单数量为剩余未成交量
        is_buy ? original_id : 0,         // 买单撤单时填bid
        is_buy ? 0 : original_id,         // 卖单撤单时填ask
        next_trade_id_++,                  // 交易ID
        2,                                 // exectype=2（撤单）
        " ",                               // tradebsflag（空格）
        2012,                              // channelno
        0                                  // bizindex
    );
    
    trade_records_.push_back(record);
}

// 将交易记录输出到CSV文件
void BacktestEngine::writeTradeRecords() const {
    if (trade_output_file_.empty() || trade_records_.empty()) {
        std::cout << "没有交易记录需要写入" << std::endl;
        return;
    }
    
    std::ofstream file(trade_output_file_);
    if (!file.is_open()) {
        std::cerr << "无法创建交易记录文件: " << trade_output_file_ << std::endl;
        return;
    }
    
    // 写入CSV标题行（与cstra文件格式一致）
    file << "datetime,sym,price,size,bidorderid,askorderid,tradeid,exectype,tradebsflag,channelno,bizindex\n";
    
    // 写入每笔交易记录
    size_t count = 0;
    for (const auto& record : trade_records_) {
        file << record.datetime << ","
             << record.sym << ","
             << std::fixed << std::setprecision(2) << record.price << ","  // 改为2位小数
             << std::fixed << std::setprecision(1) << record.size << ","   // 数量保持1位小数
             << record.bidorderid << ","
             << record.askorderid << ","
             << record.tradeid << ","
             << record.exectype << ","
             << record.tradebsflag << ","
             << record.channelno << ","
             << record.bizindex << "\n";
        count++;
    }
    
    file.close();
    std::cout << "成功写入 " << count << " 条交易记录到文件: " << trade_output_file_ << std::endl;
}

// 获取所有交易记录
const std::vector<TradeRecord>& BacktestEngine::getTradeRecords() const {
    return trade_records_;
}

// 处理用户事件（下单或撤单）
void BacktestEngine::processUserEvent(const UserEvent& user_event) {
    switch (user_event.type) {
        case UserEvent::ORDER:
            processUserOrder(user_event.order);
            break;
        case UserEvent::CANCEL:
            processUserCancel(user_event.cancel);
            break;
    }
}

// 处理用户撤单
void BacktestEngine::processUserCancel(const UserCancel& user_cancel) {
    auto it = user_order_mapping_.find(user_cancel.order_id);
    if (it != user_order_mapping_.end()) {
        uint64_t system_order_id = it->second;
        
        if (continuous_mode_) {
            con_engine_->cancel(system_order_id);
        } else {
            call_engine_->cancel(system_order_id);
        }
        
        std::cout << "[策略撤单] " << user_cancel.strategy_id 
                  << " 撤销订单: " << user_cancel.order_id 
                  << " (系统ID: " << system_order_id << ")" << std::endl;
    } else {
        std::cout << "[策略撤单失败] " << user_cancel.strategy_id 
                  << " 找不到订单: " << user_cancel.order_id << std::endl;
    }
}





// 通知策略订单成交
void BacktestEngine::notifyStrategiesOnExecution(const Execution& ex) {
    // 检查买方订单是否属于某个策略
    for (const auto& mapping : user_order_mapping_) {
        if (mapping.second == ex.buy_order_id) {
            // 找到对应的策略并通知
            for (auto& strategy : strategies_) {
                strategy->onOrderFilled(mapping.first, ex.price, ex.volume);
            }
            break;
        }
    }
    
    // 检查卖方订单是否属于某个策略
    for (const auto& mapping : user_order_mapping_) {
        if (mapping.second == ex.sell_order_id) {
            // 找到对应的策略并通知
            for (auto& strategy : strategies_) {
                strategy->onOrderFilled(mapping.first, ex.price, ex.volume);
            }
            break;
        }
    }
}



} // namespace wangcai 