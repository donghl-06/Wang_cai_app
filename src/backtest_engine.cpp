#include "../include/backtest_engine.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <fstream> // Added for file operations

namespace wangcai_orderbook_cpp {
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
            // 成交回调：生成市场数据并推送到队列
            MarketData data;
            // 连续竞价阶段使用最后一条BRK事件时间，集合竞价阶段使用当前事件时间
            data.datetime = continuous_mode_ ? last_brk_datetime_ : current_datetime_;
            data.symbol = symbol_;
            data.best_bid = orderbook_->bestBid();
            data.best_ask = orderbook_->bestAsk();
            data.last_price = ex.price;
            data.last_volume = ex.volume;
            data.event_type = "trade";
            
            market_data_queue_.push(data);
            
            // 更新最新成交价（供后续集合竞价使用）
            orderbook_->setLastTradePrice(ex.price);
            
            // 检查并通知策略订单成交
            notifyStrategiesOnExecution(ex);
            
            // 记录交易信息（如果启用了记录）
            if (recording_enabled_) {
                recordTrade(ex, continuous_mode_ ? last_brk_datetime_ : current_datetime_);
            }
            
            if (market_data_callback_) {
                market_data_callback_(data);
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
    
    // 5. 初始化连续竞价撮合引擎，注册撤单回调
    MarketType market_type = (orderbook_->getExchange() == "SH") ? MarketType::SH : MarketType::SZ;
    con_engine_ = std::make_unique<ConAuctionEngine>(*orderbook_, market_type,
        [this](uint64_t order_id, bool success, const std::string& reason, 
               std::shared_ptr<Order> order_info) {
            // 撤单回调，通知所有策略并记录撤单
            if (success && recording_enabled_ && order_info) {
                uint64_t original_id = orderbook_->getOriginalOrderId(order_id);
                recordCancelWithOrderInfo(original_id, continuous_mode_ ? last_brk_datetime_ : current_datetime_, order_info);
            }
            
            for (auto& strategy : strategies_) {
                strategy->onOrderCancelled(std::to_string(order_id), reason);
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
 
    // 7. 加载历史订单和成交数据，合并为事件流
    OrderBook::clearEvents(); // 清空事件
    std::string ord_file = data_path_ + "/csord_" + symbol_ + "_" + date_ + ".csv";
    std::string tra_file = data_path_ + "/cstra_" + symbol_ + "_" + date_ + ".csv";
    load_orders_from_csv(ord_file, *orderbook_);
    load_traders_from_csv(tra_file, *orderbook_);

    // 计算总事件数（仅用于日志输出）
    size_t total_events = 0;
    for (const auto& pair : OrderBook::whole_events) {
        total_events += pair.second.size();
    }
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
    
    static constexpr const char* Call_Open_Time = "09:25:00"; // 开盘集合竞价结束点
    static constexpr const char* Call_Close_Time = "14:57:00"; // 收盘集合竞价开始点
    
    // 主循环：按事件顺序处理
    for (const auto& pair : OrderBook::whole_events) {
        const auto& events_in_bucket = pair.second;
        
        for (size_t i = 0; i < events_in_bucket.size(); ++i) {
            const Event& ev = events_in_bucket[i];
            current_datetime_ = ev.datetime;
            
            // Step 1: 将事件转换为MarketData推送给策略
            MarketData market_data = eventToMarketData(ev);
            std::vector<UserOrder> strategy_orders;
            
            // 收集所有策略的订单响应
            for (auto& strategy : strategies_) {
                auto user_orders = strategy->onMarketData(market_data);
                for (const auto& order : user_orders) {
                    strategy_orders.push_back(order);
                }
            }
            
            // Step 2: 处理当前历史事件
            if (!continuous_mode_) {
                // 集合竞价阶段
                std::string tm_cur = ev.datetime.substr(11, 8);
                
                if (ev.source == "ord") {
                    Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                    auto ord = orderbook_->createOrder("BRK", "AC", orderbook_->getExchange(), 
                                                      ev.sym, std::to_string(ev.orderid),
                                                      toOrderType(ev), dir, ev.price, ev.size, ev.bizindex);
                    call_engine_->accept(ord);
                } else {
                    uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                    call_engine_->cancel_by_input_id(oid_raw);
                }
                
                // 发布市场数据快照
                publishMarketData("order", ev.datetime);
                
                // 检查是否结束集合竞价
                if (tm_cur >= Call_Open_Time) {
                    std::string auction_time = ev.datetime.substr(0, 11) + "09:25:00.000";
                    current_datetime_ = auction_time;
                    
                    call_engine_->settle();
                    continuous_mode_ = true;
                    publishMarketData("auction_settle", auction_time);
                    std::cout << "[09:25] 集合竞价完成，开盘价=" << call_engine_->getPredictPrice() / 10000.0 
                              << " 真实开盘价=" << actual_open_ / 10000.0
                              << " 开盘成交量=" << call_engine_->getPredictVolume() << std::endl;
                    std::cout << "连续竞价开始" << std::endl;
                    
                    current_datetime_ = ev.datetime;
                }
            } else {
                // 连续竞价阶段
                std::string tm_cur = ev.datetime.substr(11,8);
                if (!closing_mode_ && tm_cur >= Call_Close_Time) {
                    closing_mode_ = true;
                    close_engine_->bootstrap_from_orderbook();
                    std::cout << "[" << tm_cur << "] 进入收盘集合竞价阶段" << std::endl;
                }

                if (!closing_mode_) {
                    // 普通连续竞价处理
                    if (ev.source == "ord") {
                        Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                        auto ord = orderbook_->createOrder("BRK", "AC", orderbook_->getExchange(),
                                                          ev.sym, std::to_string(ev.orderid),
                                                          toOrderType(ev), dir, ev.price, ev.size, ev.bizindex);
                        con_engine_->accept(ord);
                        last_brk_datetime_ = ev.datetime;
                    } else {
                        uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                        con_engine_->cancel_by_input_id(oid_raw);
                    }
                } else {
                    // 收盘集合竞价期间：订单进入 close_engine_
                    if (ev.source == "ord") {
                        Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                        auto ord = orderbook_->createOrder("BRK", "AC", orderbook_->getExchange(),
                                                          ev.sym, std::to_string(ev.orderid),
                                                          toOrderType(ev), dir, ev.price, ev.size, ev.bizindex);
                        close_engine_->accept(ord);
                    } else {
                        uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                        close_engine_->cancel_by_input_id(oid_raw);
                    }
                }

                publishMarketData("order", ev.datetime);
            }
            
            // Step 3: 处理策略订单（在历史事件之后）
            for (const auto& user_order : strategy_orders) {
                if (continuous_mode_) {  // 只有连续竞价期间才允许策略下单
                    processUserOrder(user_order);
                }
            }
            
            // Step 4: 处理市场数据队列，包括成交推送
            while (!market_data_queue_.empty()) {
                MarketData data = market_data_queue_.front();
                market_data_queue_.pop();
                
                // 如果是成交数据，推送给策略并收集新的订单
                if (data.event_type == "trade") {
                    std::vector<UserOrder> trade_strategy_orders;
                    
                    for (auto& strategy : strategies_) {
                        // 先检查是否是该策略的订单成交
                        checkAndNotifyOrderFilled(data, strategy);
                        
                        // 推送成交数据给策略
                        auto user_orders = strategy->onMarketData(data);
                        for (const auto& order : user_orders) {
                            trade_strategy_orders.push_back(order);
                        }
                    }
                    
                    // 处理成交后的策略订单
                    for (const auto& user_order : trade_strategy_orders) {
                        if (continuous_mode_) {
                            processUserOrder(user_order);
                        }
                    }
                }
                
                if (market_data_callback_) {
                    market_data_callback_(data);
                }
            }
        }
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

// 发布市场数据到队列，并触发回调
void BacktestEngine::publishMarketData(const std::string& event_type, const std::string& datetime) {
    MarketData data;
    data.datetime = datetime;
    data.symbol = symbol_;
    data.best_bid = orderbook_->bestBid();
    data.best_ask = orderbook_->bestAsk();
    data.event_type = event_type;
    
    market_data_queue_.push(data);
    if (market_data_callback_) {
        market_data_callback_(data);
    }
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
        con_engine_->accept(order);
        
        // 如果订单立即成交，更新持仓并通知策略
        ////// 未完成！！！！！ /////
        if (order->traded_volume > 0) {
            updatePosition(user_order.strategy_id, user_order.symbol,
                          user_order.direction, order->traded_volume, 
                          order->traded_volume > 0 ? order->price : 0); // 简化：使用订单价格
            
            // 通知对应策略订单成交
            for (auto& strategy : strategies_) {
                if (strategy->getStrategyId() == user_order.strategy_id) {
                    strategy->onOrderFilled(user_order.order_id, order->price, order->traded_volume);
                }
            }
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
    TradeRecord record(
        datetime,                           // 交易时间
        symbol_,                           // 合约代码
        ex.price / 10000.0,               // 成交价格（转换为元）
        static_cast<double>(ex.volume),    // 成交数量
        ex.buy_order_id,                   // 买方订单ID
        ex.sell_order_id,                  // 卖方订单ID
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
        0.0,                               // 撤单价格为0
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



// 事件到MarketData的转换
MarketData BacktestEngine::eventToMarketData(const Event& ev) {
    MarketData data;
    data.datetime = ev.datetime;
    data.symbol = symbol_;
    data.best_bid = orderbook_->bestBid();
    data.best_ask = orderbook_->bestAsk();
    data.event_type = (ev.source == "ord") ? "order" : "cancel";
    data.last_price = 0;  // 将在后续成交时更新
    data.last_volume = 0;
    return data;
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



} // namespace wangcai_orderbook_cpp 