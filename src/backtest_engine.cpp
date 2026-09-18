#include "../include/backtest_engine.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <fstream> // Added for file operations
#include <ranges>
#include <thread>  // 用于 sleep_for
#include <chrono>  // 用于 microseconds
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

// 构造函数，初始化回测引擎，从CSV字符串加载数据
// is_etf: true=ETF（三位小数，tick=10）, false=股票（两位小数，tick=100）
BacktestEngine::BacktestEngine(const std::string& symbol, 
                               const std::string& cstick_csv,
                               const std::string& order_csv, 
                               const std::string& trade_csv,
                               const std::string& csbar1d_csv,
                               bool is_etf)
    : symbol_(symbol), cstick_csv_(cstick_csv), order_csv_(order_csv), trade_csv_(trade_csv), csbar1d_csv_(csbar1d_csv),
      continuous_mode_(false), next_order_id_(1), next_trade_id_(1), recording_enabled_(false), is_etf_(is_etf)
{
    initialize();
}

// 初始化函数，完成回测环境的所有准备工作
void BacktestEngine::initialize() {
    // 1. 从CSV字符串读取前收盘价和真实开盘价（传入 is_etf_ 以正确对齐 tick）
    InfoLoader loader;
    prev_close_ = loader.loadPrevClosePrice(cstick_csv_, is_etf_);
    actual_open_ = loader.loadOpenPrice(cstick_csv_, is_etf_);
    const bool is_sz = symbol_.ends_with(".SZ");

    // 1.5 价格笼子规则判定：从 cstick 首个数据行解析交易日，按 板块×日期 查规则矩阵
    {
        // cstick 列序：date,time,sym,prevclose,...（首个数据行的第一个字段）
        size_t pos = cstick_csv_.find('\n');
        std::string first_data = cstick_csv_.substr(pos + 1, cstick_csv_.find('\n', pos + 1) - pos - 1);
        const size_t comma = first_data.find(',');
        trading_date_ = (comma != std::string::npos) ? first_data.substr(0, comma) : "";
        cage_rule_ = priceCageRuleFor(boardOf(symbol_), trading_date_);
        cage_inference_active_ = needsHistoricalCageInference(symbol_, trading_date_);
        if (cage_rule_.enabled) {
            const char* action = cage_rule_.action == CageAction::Reject ? "废单" : "暂存";
            std::cout << "[价格笼子] " << symbol_ << " " << trading_date_
                      << " 规则: ±" << cage_rule_.pct_num / 100.0 << "%"
                      << (cage_rule_.min_abs > 0 ? " 与 0.1 元孰高" : "")
                      << "，超范围=" << action << std::endl;
            if (cage_inference_active_) {
                std::cout << "[价格笼子] 深市创业板暂存窗口：历史单启用消息流反推状态机"
                          << "（真实成交事件进流用于判定）" << std::endl;
            }
        }
    }

    if (prev_close_ == 0) {
        // 如果前收盘价读取失败，抛出异常
        throw std::runtime_error("无法读取前收盘价");
    }
    
    // 2. 从csbar1d文件读取涨跌停限制（含冗余，ETF=0.01元，股票=0.1元）；
    //    涨跌停为 0（新股前 5 日无涨跌幅）时按全天申报/成交价格范围构造簿边界
    auto [px_lo, px_hi] = InfoLoader::scanPriceRange(order_csv_, trade_csv_);
    auto [upper_limit, lower_limit] = loader.loadPriceLimits(
        csbar1d_csv_, is_etf_, prev_close_ / 10000.0, px_lo, px_hi);
    upper_limit_ = upper_limit;
    lower_limit_ = lower_limit;

    // 3. 初始化订单簿，注册成交回调（传入 is_etf_ 设置 tick）
    orderbook_ = std::make_unique<OrderBook>(upper_limit_, lower_limit_, is_etf_,
        [this](const Execution& ex) {
            std::string trade_datetime = current_datetime_;
            
            // 更新最新成交价（供后续集合竞价使用）
            orderbook_->setLastTradePrice(ex.price);
            
            // Execution 中只允许内部 system-id；输出市场 ID 在本回调统一解析。
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
            
            const bool user_execution = ex.origin != ExecutionOrigin::HistoricalHistorical;
            if (user_execution) {
                if (buy_is_virtual == sell_is_virtual) {
                    throw MarketIdentityError("用户成交必须且只能包含一侧用户 system-id");
                }
                if (buy_is_virtual) {
                    updatePosition(virtual_strategy_id, symbol_, Direction::Buy, ex.volume, ex.price);
                } else if (sell_is_virtual) {
                    updatePosition(virtual_strategy_id, symbol_, Direction::Sell, ex.volume, ex.price);
                }
                // 虚拟成交：只通知对应策略，不记录到CSV
                for (auto& strategy : strategies_) {
                    if (strategy->getStrategyId() == virtual_strategy_id) {
                        // 确定虚拟订单的方向
                        Direction virtual_direction;
                        uint64_t virtual_sys_id = buy_is_virtual ? ex.buy_order_id : ex.sell_order_id;
                        auto dir_it = user_order_direction_.find(virtual_sys_id);
                        if (dir_it != user_order_direction_.end()) {
                            virtual_direction = dir_it->second;
                        } else {
                            // 如果找不到记录，根据买卖方ID推断
                            virtual_direction = buy_is_virtual ? Direction::Buy : Direction::Sell;
                        }
                        
                        // 调用新的统一成交回调接口
                        auto callback = createTradeCallback(virtual_strategy_id, virtual_order_id,
                                                          virtual_direction, ex.volume, ex.price,
                                                          trade_datetime, 'T');
                        notifyStrategyTradeCallback(virtual_strategy_id, callback);
                        
                        std::cout << "[虚拟成交] 策略 " << virtual_strategy_id 
                                  << " 订单 " << virtual_order_id 
                                  << " 成交 " << ex.volume << "@" << ex.price / 10000.0 << std::endl;
                        break;
                    }
                }
                
                // 成交后清理虚拟订单映射
                // RT模式下被动单可能多次部分成交，需保留映射到终态（Filled/Cancelled/Rejected）
                auto should_cleanup_virtual_mapping = [this](uint64_t virtual_sys_id) {
                    if (!real_trade_match_mode_) return true;
                    auto ord = orderbook_->getOrder(virtual_sys_id);
                    if (!ord) return true;
                    if (ord->status == OrderStatus::Filled ||
                        ord->status == OrderStatus::Cancelled ||
                        ord->status == OrderStatus::Rejected) {
                        return true;
                    }
                    return ord->remaining_volume() == 0;
                };

                if (buy_is_virtual && should_cleanup_virtual_mapping(ex.buy_order_id)) {
                    virtual_order_strategy_.erase(ex.buy_order_id);
                    virtual_order_local_id_.erase(ex.buy_order_id);
                    user_order_direction_.erase(ex.buy_order_id);  // 清理方向映射
                }
                if (sell_is_virtual && should_cleanup_virtual_mapping(ex.sell_order_id)) {
                    virtual_order_strategy_.erase(ex.sell_order_id);
                    virtual_order_local_id_.erase(ex.sell_order_id);
                    user_order_direction_.erase(ex.sell_order_id);  // 清理方向映射
                }
                
                // 注意：虚拟成交不记录到CSV，不调用recordTrade()
            } else {
                if (buy_is_virtual || sell_is_virtual) {
                    throw MarketIdentityError("历史成交中出现了用户 system-id");
                }
                // 历史订单成交：正常处理并记录到CSV

                // 实时合成 tick 的日累计器：历史成交在此唯一入口计入
                accumulateInternalTrade(ex.price, ex.volume);

                // 1. 成交先计入批量缓冲,待本市场事件全部撮合完成后
                // 经 onTradeEventsBatch 一次性推送策略(批量跨 Python 边界)
                // 将Execution转换为TradeDetail
                pending_trade_batch_.push_back(normalizeHistoricalExecution(ex, trade_datetime));
                
                // 2. 记录交易信息（如果启用了记录）- 只记录历史订单成交
                if (recording_enabled_) {
                    recordTrade(pending_trade_batch_.back(), trade_datetime);
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
                                                               uint64_t original_id = orderbook_->requireMarketIdentity(order_id).market_order_id;
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
                if (order_info->is_historical) {
                    // 历史订单撤单：只记录撤单到CSV，不通知策略
                    if (recording_enabled_) {
                        uint64_t original_id = orderbook_->requireMarketIdentity(order_id).market_order_id;
                        recordCancelWithOrderInfo(original_id, current_datetime_, order_info);
                    }
                    // 注意：历史订单撤单不通知策略，因为策略不应该关心历史订单的撤单
                } else {
                    // 虚拟订单撤单：只通知对应策略，不记录到CSV
                    for (auto& strategy : strategies_) {
                        if (strategy->getStrategyId() == order_info->account) {
                            // 调用原有撤单接口（兼容性）
                            // strategy->onOrderCancelled(order_info->order_local_id, reason);
                            
                            // 调用新的统一撤单回调接口
                            std::string cancel_datetime = current_datetime_;
                            auto callback = createTradeCallback(strategy->getStrategyId(), order_info->order_local_id,
                                                              order_info->direction, 0, order_info->price,
                                                              cancel_datetime, 'D');
                            strategy->onTradeCallback(callback);
                            
                            std::cout << "[虚拟撤单] 策略 " << order_info->account 
                                      << " 订单 " << order_info->order_local_id 
                                      << " 已撤单，原因: " << reason << std::endl;
                            break;
                        }
                    }
                    
                    // 撤单后清理虚拟订单映射，防止影响后续订单识别
                    virtual_order_strategy_.erase(order_id);
                    virtual_order_local_id_.erase(order_id);  
                    user_order_direction_.erase(order_id);
                }
            }
        });

    // 6. 初始化收盘集合竞价撮合引擎（逻辑基本同开盘集合竞价）
    close_engine_ = std::make_unique<CloseAuctionEngine>(*orderbook_, prev_close_,
                                                        orderbook_->getExchange(),
                                                        nullptr, 
                                                        [this](uint64_t order_id, bool success, const std::string& reason,
                                                               std::shared_ptr<Order> order_info) {
                                                            if (success && recording_enabled_ && order_info) {
                                                                uint64_t original_id = orderbook_->requireMarketIdentity(order_id).market_order_id;
                                                                recordCancelWithOrderInfo(original_id, current_datetime_, order_info);
                                                            }
                                                        });


    data_manager_ = std::make_unique<DataManager>(orderbook_.get(), call_engine_.get(), con_engine_.get(), close_engine_.get());

    // 7. 从CSV字符串加载历史订单和成交数据，合并为事件流
    orderbook_->clearEvents(); // 清空事件
    orderbook_->clearTicks(); // 清空tick事件

    if (is_sz) {
        loader.load_sz_info(order_csv_, trade_csv_, *orderbook_);
        // 比较器必须按 const 引用:原先按值传参,每次比较复制两个 ~700B Event,
        // 数十万事件排序仅此一项就是数亿字节的堆分配
        std::sort(std::execution::par_unseq, orderbook_->whole_events.begin(), orderbook_->whole_events.end(), [&](const auto& a, const auto& b) {
            return a.orderid < b.orderid; // SZ
        });
    } else {
        loader.load_sh_info(order_csv_, trade_csv_, *orderbook_);
        std::sort(std::execution::par_unseq, orderbook_->whole_events.begin(), orderbook_->whole_events.end(), [&](const auto& a, const auto& b) {
            return a.bizindex < b.bizindex; // SH
        });
    }
    
    loader.load_cstick(cstick_csv_, *orderbook_);
}

// 注册策略，支持多策略回测
void BacktestEngine::registerStrategy(std::shared_ptr<Strategy> strategy) {
    strategies_.push_back(strategy);
    // 为每个策略初始化持仓映射
    positions_[strategy->getStrategyId()] = std::map<std::string, Position>();
    // 下单延迟与进簿位置快照（注册之后再 set 不生效）
    const int lat = strategy->getOrderLatencyMs();
    latency_map_[strategy->getStrategyId()] = lat;
    latency_head_map_[strategy->getStrategyId()] =
        strategy->getLatencyEntryPosition() == LatencyEntryPosition::Head;
    if (lat > 0) any_latency_ = true;
}

// 尝试立即成交USER订单
bool BacktestEngine::tryFillImmediately(std::shared_ptr<Order> user_order) {
    // ==================== 严格主动单模式：禁用“秒成捷径” ====================
    if (con_engine_ && con_engine_->isStrictActiveOrderMode()) {
        return false;
    }

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
                // 调用原有成交接口（兼容性）
                // strategy->onOrderFilled(user_order->order_local_id, fill_price, fill_qty);
                
                // 调用新的统一成交回调接口
                std::string trade_datetime = current_datetime_;
                auto callback = createTradeCallback(strategy->getStrategyId(), user_order->order_local_id,
                                                  user_order->direction, fill_qty, fill_price,
                                                  trade_datetime, 'T');
                notifyStrategyTradeCallback(strategy->getStrategyId(), callback);
                break;
            }
        }
        
        // 立即成交后必须清理虚拟订单映射，防止影响后续订单识别
        virtual_order_strategy_.erase(user_order->order_id);
        virtual_order_local_id_.erase(user_order->order_id);
        user_order_direction_.erase(user_order->order_id);
        
        std::cout << "[虚拟成交] USER订单 " << user_order->order_local_id 
                  << " 立即成交 " << fill_qty << "@" << fill_price / 10000.0 << std::endl;
        return true;
    }
    
    return false;
}


// 处理用户策略下发的订单
// 连续竞价阶段：立即撮合；集合竞价阶段（开盘/收盘）：暂存为影子单，settle 时判定
void BacktestEngine::processUserOrder(const UserOrder& user_order) {
    try {
        // 创建订单对象（broker="USER"，is_historical=false，天然影子）
        auto order = orderbook_->createOrder(
            "USER", user_order.strategy_id, orderbook_->getExchange(),
            user_order.symbol, user_order.order_id,
            user_order.order_type, user_order.direction,
            user_order.price, user_order.volume, next_order_id_++
        );

        // 记录映射（不论阶段）
        user_order_mapping_[user_order.order_id] = order->order_id;
        user_order_direction_[order->order_id] = user_order.direction;
        virtual_order_strategy_[order->order_id] = user_order.strategy_id;
        virtual_order_local_id_[order->order_id] = user_order.order_id;

        // 创建并发送下单回调
        auto order_callback = createOrderCallback(user_order.strategy_id, user_order.order_id,
                                                user_order.direction, user_order.volume, user_order.price,
                                                current_datetime_);
        // 可选：队列信息（仅在连续竞价阶段有意义）
        if (queue_info_enabled_ && con_engine_ && continuous_mode_ && !closing_mode_) {
            auto qi = con_engine_->getQueueInfoForUserOrder(
                order->price, order->direction == Direction::Buy);
            order_callback.queue_ahead_count = qi.ahead_count;
            order_callback.queue_ahead_volume = qi.ahead_volume;
            order_callback.prev_order_ids = qi.prev_order_ids;
        }
        notifyStrategyOrderCallback(user_order.strategy_id, order_callback);

        if (continuous_mode_ && !closing_mode_) {
            // 连续竞价阶段：策略限价单依次通过 涨跌停 → 价格笼子 校验
            bool rejected = false;
            if (order->order_type == OrderType::Limit &&
                (order->price > orderbook_->getUpperLimit() ||
                 order->price < orderbook_->getLowerLimit())) {
                // 涨跌停前置校验（所有时代、所有板块；当前引擎此前缺失此校验）
                con_engine_->rejectUserOrder(order, "涨跌停校验：申报价格超出涨跌停限制，废单");
                rejected = true;
            } else if (user_cage_enabled_ && cage_rule_.enabled
                       && order->order_type == OrderType::Limit) {
                // 价格笼子数值判定（策略单没有后续消息可反推，按规则数值模拟）
                const auto bounds = ConAuctionEngine::userCageBounds(
                    order->direction == Direction::Buy, cage_rule_, *orderbook_);
                if (bounds.lo > 0 &&
                    (order->price > bounds.hi || order->price < bounds.lo)) {
                    if (cage_rule_.action == CageAction::Reject) {
                        con_engine_->rejectUserOrder(order,
                            "价格笼子：申报价格超出有效申报价格范围，废单");
                        rejected = true;
                    } else {  // CageAction::Dormant（创业板暂存窗口）
                        con_engine_->suspendUserOrder(order);
                        return;  // 入笼：不进簿、不可见、可撤单、待出笼
                    }
                }
            }
            if (rejected) return;

            // 连续竞价：原逻辑 —— 尝试立即成交，失败则送入撮合引擎
            if (!tryFillImmediately(order)) {
                con_engine_->accept(order);
            }
        } else {
            // 集合竞价阶段（开盘 或 收盘）：暂存为影子单，不参与真实集合竞价的价格发现
            pending_auction_orders_.push_back({order, /*is_close_auction=*/closing_mode_});
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
                double current_price = orderbook_->bestBid() / 10000.0; 
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

// === 严格主动单模式（欠债限制功能）===
void BacktestEngine::setStrictActiveOrderMode(bool enabled) {
    if (con_engine_) {
        con_engine_->setStrictActiveOrderMode(enabled);
    }
}

bool BacktestEngine::isStrictActiveOrderMode() const {
    if (con_engine_) {
        return con_engine_->isStrictActiveOrderMode();
    }
    return false;
}

bool BacktestEngine::hasDebt() const {
    if (con_engine_) {
        return con_engine_->hasDebt();
    }
    return false;
}

// === 真实成交替代模式 ===
void BacktestEngine::setRealTradeMatchMode(bool enabled) {
    real_trade_match_mode_ = enabled;
    if (con_engine_) {
        con_engine_->setRealTradeMatchMode(enabled);
    }
}

bool BacktestEngine::isRealTradeMatchMode() const {
    return real_trade_match_mode_;
}

void BacktestEngine::setQueueInfoEnabled(bool enabled) {
    queue_info_enabled_ = enabled;
    if (con_engine_) {
        con_engine_->setQueueInfoEnabled(enabled);
    }
}

bool BacktestEngine::isQueueInfoEnabled() const {
    return queue_info_enabled_;
}

void BacktestEngine::setRealTimeTickInterval(int interval_ms) {
    realtime_tick_interval_ms_ = interval_ms > 0 ? interval_ms : 0;
}

int BacktestEngine::getRealTimeTickInterval() const {
    return realtime_tick_interval_ms_;
}

void BacktestEngine::setEventSnapshotEnabled(bool enabled) {
    event_snapshot_enabled_ = enabled;
}

bool BacktestEngine::isEventSnapshotEnabled() const {
    return event_snapshot_enabled_;
}

size_t BacktestEngine::getSuspendedHistoricalCount() const {
    return con_engine_ ? con_engine_->getSuspendedHistoricalCount() : 0;
}

size_t BacktestEngine::getUserCageCount() const {
    return con_engine_ ? con_engine_->getUserCageCount() : 0;
}

namespace {
// "YYYY-MM-DD HH:MM:SS.mmm" -> 当日毫秒数；格式不足时返回 -1
inline int64_t msOfDayFromDatetime(const std::string& dt) {
    if (dt.size() < 23) return -1;
    auto d2 = [&dt](std::size_t pos) {
        return (dt[pos] - '0') * 10 + (dt[pos + 1] - '0');
    };
    const int64_t sec = static_cast<int64_t>(d2(11)) * 3600 +
                        static_cast<int64_t>(d2(14)) * 60 +
                        static_cast<int64_t>(d2(17));
    const int64_t ms = (dt[20] - '0') * 100 + (dt[21] - '0') * 10 + (dt[22] - '0');
    return sec * 1000 + ms;
}

// 当日毫秒数 -> HHMMSSsss 整数（与真实 tick 的 Snapshot.Time 同格式）
inline int timeRawFromMsOfDay(int64_t ms_of_day) {
    const int h = static_cast<int>(ms_of_day / 3600000);
    const int m = static_cast<int>((ms_of_day / 60000) % 60);
    const int s = static_cast<int>((ms_of_day / 1000) % 60);
    const int ms = static_cast<int>(ms_of_day % 1000);
    return h * 10000000 + m * 100000 + s * 1000 + ms;
}

// 真实 tick 从 09:15:00 起才有效（insertTick 同款过滤），合成 tick 保持一致
constexpr int64_t kRealTimeTickStartMs = (9 * 3600 + 15 * 60) * 1000LL;
} // namespace

// 历史撮合成交递增日累计器（exec 回调的 HistoricalHistorical 分支调用）。
// 覆盖连续竞价内部撮合与开/收盘集合竞价 settle；用户虚拟成交不计入，
// 因为官方行情的累计量只包含真实市场成交。
void BacktestEngine::accumulateInternalTrade(Price price, Quantity volume) {
    if (price <= 0 || volume <= 0) return;
    rt_volume_ += volume;
    rt_turnover_ += static_cast<double>(price) / 10000.0 * static_cast<double>(volume);
    rt_num_trades_ += 1;
    if (rt_high_ == 0 || price > rt_high_) rt_high_ = price;
    if (rt_low_ == 0 || price < rt_low_) rt_low_ = price;
}

// 从内部订单簿合成十档快照，字段结构与真实 tick 的 Snapshot 一致：
// - 十档/最新价/涨跌停/委托总量：订单簿实时状态
// - Volume/Turnover/NumTrades/High/Low：日累计器（引擎重建口径；实测与官方
//   cstick 的 volume/tradecount 一致——见 tests/event_snapshot 对齐测试）
// - Open/PreClose/Close/Iopv 等无法从订单簿推导的字段：承接上一个真实 tick
// - 集合竞价阶段额外填 AuctionPrice/AuctionQty（预测价/预测量）
Snapshot BacktestEngine::buildRealTimeSnapshot(const Event& ev) const {
    Snapshot s{};  // 值初始化兜底：首个真实 tick 之前 DataManager 的快照字段未定义
    if (has_real_tick_) {
        s = data_manager_->getSnapshot();
    }

    s.Exchange = ev.exchange != -1 ? ev.exchange : (ev.isSZ() ? 1 : 0);
    s.Instrument = symbol_;
    if (ev.trading_day > 0) s.TradingDay = ev.trading_day;
    if (ev.action_day > 0) s.ActionDay = ev.action_day;
    else if (ev.trading_day > 0) s.ActionDay = ev.trading_day;

    const int64_t ms_of_day = msOfDayFromDatetime(ev.datetime);
    s.Time = ms_of_day >= 0 ? timeRawFromMsOfDay(ms_of_day)
                            : (ev.time_raw != -1 ? ev.time_raw : 0);
    s.datetime = ev.datetime;

    // 盘口：只含历史订单的纯市场十档
    orderbook_->fillDepth(s.bids, s.bid_sizes, /*is_buy=*/true);
    orderbook_->fillDepth(s.asks, s.ask_sizes, /*is_buy=*/false);
    s.last_price = orderbook_->getLastTradePrice();
    s.UpperLimit = orderbook_->getUpperLimit();
    s.LowerLimit = orderbook_->getLowerLimit();
    if (orderbook_->getPrevClose() > 0) s.PreClose = orderbook_->getPrevClose();
    s.TotalBidVol = orderbook_->getTotalBidVol();
    s.TotalAskVol = orderbook_->getTotalAskVol();

    // 累计字段
    s.Volume = rt_volume_;
    s.Turnover = static_cast<uint64_t>(rt_turnover_);
    s.NumTrades = rt_num_trades_;
    if (rt_high_ > 0) s.High = rt_high_;
    if (rt_low_ > 0) s.Low = rt_low_;

    // 集合竞价阶段盘口未交叉，十档语义与连续竞价不同；补充预测价/量供策略参考
    if (!continuous_mode_) {
        s.AuctionPrice = call_engine_->getPredictPrice();
        s.AuctionQty = call_engine_->getPredictVolume();
    } else if (closing_mode_) {
        s.AuctionPrice = close_engine_->getPredictPrice();
        s.AuctionQty = close_engine_->getPredictVolume();
    }
    return s;
}

// 跨过间隔网格边界时推送合成 tick。
// 网格对齐（当日毫秒 / 间隔）保证推送落点确定、可复现；
// 同一网格内的后续事件不再推送，即"每个市场事件时间戳最多推一次"。
// 在当前事件撮合完成后调用，快照反映事件处理后的盘口。
void BacktestEngine::maybeEmitRealTimeTick(const Event& ev) {
    if (realtime_tick_interval_ms_ <= 0 || strategies_.empty()) return;
    const int64_t now_ms = msOfDayFromDatetime(ev.datetime);
    if (now_ms < kRealTimeTickStartMs) return;
    const int64_t bucket = now_ms / realtime_tick_interval_ms_;
    if (bucket <= realtime_tick_last_bucket_) return;
    realtime_tick_last_bucket_ = bucket;

    const Snapshot snapshot = buildRealTimeSnapshot(ev);
    for (auto& strategy : strategies_) {
        auto user_events = strategy->onRealTimeTickEvent(snapshot);
        for (const auto& ue : user_events) {
            pending_trade_events_.push_back(ue);
        }
    }
}

// 穿价放行改走自主撮合后，回放机制已移除：
// 反推放行单在引擎簿上正常占位与排队，撮合对手/量由引擎自主重建（与真实一致），
// 避免回放单不占簿导致的排队量缺失、后续撮合对手错位。
bool BacktestEngine::determineBuyPassive(const Event& ev) const {
    // RT模式强约束：
    // - 必须同时有 bidorderid / askorderid
    // - 直接按 orderid 大小判定主动/被动：小ID被动，大ID主动
    if (ev.bidorderid <= 0 || ev.askorderid <= 0) {
        throw std::runtime_error(
            "RT模式要求tra成交事件包含有效的bidorderid和askorderid: sym=" + ev.sym +
            ", datetime=" + ev.datetime +
            ", bidorderid=" + std::to_string(ev.bidorderid) +
            ", askorderid=" + std::to_string(ev.askorderid)
        );
    }

    if (ev.bidorderid == ev.askorderid) {
        throw std::runtime_error(
            "RT模式要求bidorderid与askorderid不能相同: sym=" + ev.sym +
            ", datetime=" + ev.datetime +
            ", orderid=" + std::to_string(ev.bidorderid)
        );
    }

    // bidorderid 更小 => 买方被动；否则卖方被动
    return static_cast<uint64_t>(ev.bidorderid) < static_cast<uint64_t>(ev.askorderid);
}

// === 用户自定义事件支持 ===
void BacktestEngine::submitUserEvent(const UserEvent& user_event) {
    // 走下单延迟入口（跨标的/自定义事件同样受策略延迟约束）
    enqueueOrDispatch(user_event);
}

bool BacktestEngine::hasUserOrder(const std::string& order_id) const {
    // 延迟队列中的订单也算本引擎持有（撤单路由判定用）
    return user_order_mapping_.find(order_id) != user_order_mapping_.end()
        || delayed_order_ids_.find(order_id) != delayed_order_ids_.end();
}

void BacktestEngine::setCurrentDatetimeForCustomEvent(const std::string& datetime) {
    current_datetime_ = datetime;
    current_ms_ = parseDatetimeMs(datetime);  // 下单延迟的释放基准（跨标的/自定义事件路径）
}

std::vector<UserEvent> BacktestEngine::drainCrossSymbolEvents() {
    std::vector<UserEvent> out;
    out.swap(cross_symbol_events_);
    return out;
}

TradeDetail BacktestEngine::normalizeHistoricalExecution(const Execution& ex,
                                                         const std::string& datetime) const {
    if (ex.origin != ExecutionOrigin::HistoricalHistorical ||
        ex.buy_order_id == 0 || ex.sell_order_id == 0) {
        throw MarketIdentityError("历史成交缺少买卖双方内部订单 ID");
    }

    const auto& buy = orderbook_->requireMarketIdentity(ex.buy_order_id);
    const auto& sell = orderbook_->requireMarketIdentity(ex.sell_order_id);
    if (buy.direction != Direction::Buy || sell.direction != Direction::Sell) {
        throw MarketIdentityError("历史成交的订单方向与 BuyNo/SellNo 不一致");
    }
    if (buy.instrument != symbol_ || sell.instrument != symbol_) {
        throw MarketIdentityError("历史成交双方不属于当前标的 " + symbol_);
    }
    if (buy.channel_no != sell.channel_no) {
        throw MarketIdentityError("历史成交双方 ChannelNo 不一致，现有 TradeDetail 无法无歧义表达");
    }
    if (buy.trading_day != sell.trading_day) {
        throw MarketIdentityError("历史成交双方 TradingDay 不一致");
    }

    int time_raw = 0;
    if (datetime.length() >= 19) {
        // 手工扫描 HHMMSSmmm,不再 substr + erase-remove + stoi(每笔成交省 3 次堆分配)
        auto d2 = [&](std::size_t p) { return (datetime[p] - '0') * 10 + (datetime[p + 1] - '0'); };
        int ms = 0;
        if (datetime.length() > 22 && datetime[19] == '.')
            ms = (datetime[20] - '0') * 100 + (datetime[21] - '0') * 10 + (datetime[22] - '0');
        time_raw = d2(11) * 10000000 + d2(14) * 100000 + d2(17) * 1000 + ms;
    }

    TradeDetail trade{};
    trade.Exchange = symbol_.ends_with(".SZ") ? 1 : 0;
    trade.Instrument = symbol_;
    trade.ChannelNo = buy.channel_no;
    trade.TradeIndex = static_cast<long long>(ex.execution_id);
    trade.Time = time_raw;
    trade.Price = ex.price;
    trade.Volume = ex.volume;
    trade.ExecType = '1';
    trade.BuyNo = buy.market_order_id;
    trade.SellNo = sell.market_order_id;
    trade.TradeBSFlag = 'N';
    trade.BizIndex = 0;
    return trade;
}

// 记录单笔交易信息；输入已经完成市场 ID 还原。
void BacktestEngine::recordTrade(const TradeDetail& trade, const std::string& datetime) {

    TradeRecord record(
        datetime,                           // 交易时间
        symbol_,                           // 合约代码
        trade.Price / 10000.0,
        static_cast<double>(trade.Volume),
        static_cast<uint64_t>(trade.BuyNo),
        static_cast<uint64_t>(trade.SellNo),
        next_trade_id_++,                  // 交易ID
        1,                                 // exectype=1（正常成交）
        " ",                               // tradebsflag（空格）
        trade.ChannelNo,
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
    uint64_t output_order_id = order_id;
    int output_channel = order->market_channel_no;
    if (order->is_historical) {
        const auto& identity = orderbook_->requireMarketIdentity(order_id);
        output_order_id = identity.market_order_id;
        output_channel = identity.channel_no;
    }

    TradeRecord record(
        datetime,                           // 撤单时间
        symbol_,                           // 合约代码
        order->price / 10000.0,                               // 撤单价格为撤单订单价格
        static_cast<double>(order->remaining_volume()),  // 撤单数量为剩余未成交量
        is_buy ? output_order_id : 0,
        is_buy ? 0 : output_order_id,
        next_trade_id_++,                  // 交易ID
        2,                                 // exectype=2（撤单）
        " ",                               // tradebsflag（空格）
        output_channel,
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
        order_info->market_channel_no,
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

// 下单延迟入口：开了延迟的策略事件入延迟队列（release = 当前时钟 + 延迟），
// 否则同步处理——默认 latency=0 时与旧版行为完全一致。
// 进簿位置（头/尾）决定入哪个队列：头部单抢在同时间订单之前进簿，
// 尾部单等同时间订单处理完再进簿
void BacktestEngine::enqueueOrDispatch(const UserEvent& user_event) {
    if (any_latency_ && current_ms_ >= 0) {
        const std::string& sid = user_event.type == UserEvent::ORDER
                                     ? user_event.order.strategy_id
                                     : user_event.cancel.strategy_id;
        auto it = latency_map_.find(sid);
        if (it != latency_map_.end() && it->second > 0) {
            if (user_event.type == UserEvent::ORDER)
                delayed_order_ids_.insert(user_event.order.order_id);
            auto hit = latency_head_map_.find(sid);
            const bool at_head = hit == latency_head_map_.end() || hit->second;
            auto& q = at_head ? delayed_head_ : delayed_tail_;
            q.push(DelayedUserEvent{current_ms_ + it->second, delayed_seq_++, user_event});
            return;
        }
    }
    processUserEvent(user_event);
}

// 释放到点的延迟事件，走原有校验/进簿/撮合路径。
// 头部单：release <= 当前事件时间即释放（先于该时间所有事件进簿）;
// 尾部单：release < 当前事件时间才释放（该时间的事件全部处理完后再进簿）;
// 同队列内先下单的先释放（release 相同按 seq），撤单不会比订单先到达
void BacktestEngine::drainDelayedEvents(int64_t now_ms) {
    auto drain = [&](DelayedQueue& q, bool at_head) {
        while (!q.empty()) {
            const bool due = at_head ? q.top().release_ms <= now_ms
                                     : q.top().release_ms < now_ms;
            if (!due) break;
            DelayedUserEvent ev = std::move(const_cast<DelayedUserEvent&>(q.top()));
            q.pop();
            if (ev.event.type == UserEvent::ORDER)
                delayed_order_ids_.erase(ev.event.order.order_id);
            processUserEvent(ev.event);
        }
    };
    drain(delayed_head_, true);
    drain(delayed_tail_, false);
}

// 处理用户撤单
void BacktestEngine::processUserCancel(const UserCancel& user_cancel) {
    auto it = user_order_mapping_.find(user_cancel.order_id);
    if (it == user_order_mapping_.end()) {
        std::cout << "[策略撤单失败] " << user_cancel.strategy_id
                  << " 找不到订单: " << user_cancel.order_id << std::endl;
        return;
    }
    uint64_t system_order_id = it->second;

    // 先查集合竞价影子单队列（集合竞价期间暂存、尚未参与 settle 的订单）
    auto pit = std::find_if(pending_auction_orders_.begin(), pending_auction_orders_.end(),
        [system_order_id](const PendingAuctionOrder& po) {
            return po.order->order_id == system_order_id;
        });
    if (pit != pending_auction_orders_.end()) {
        auto ord = pit->order;
        pending_auction_orders_.erase(pit);

        // 发策略撤单回调（与 con_engine 撤单回调格式一致：match_type='D'）
        for (auto& strategy : strategies_) {
            if (strategy->getStrategyId() == ord->account) {
                auto callback = createTradeCallback(strategy->getStrategyId(), ord->order_local_id,
                                                    ord->direction, 0, ord->price,
                                                    current_datetime_, 'D');
                strategy->onTradeCallback(callback);
                break;
            }
        }

        // 清理映射
        virtual_order_strategy_.erase(system_order_id);
        virtual_order_local_id_.erase(system_order_id);
        user_order_direction_.erase(system_order_id);

        std::cout << "[策略撤单] " << user_cancel.strategy_id
                  << " 撤销集合竞价影子单: " << user_cancel.order_id
                  << " (系统ID: " << system_order_id << ")" << std::endl;
        return;
    }

    // 其次按阶段走对应撮合引擎的撤单
    if (continuous_mode_ && !closing_mode_) {
        con_engine_->cancel(system_order_id);
    } else if (!continuous_mode_) {
        call_engine_->cancel(system_order_id);
    } else {
        // 进入收盘集合竞价后，用户单只会在 pending_auction_orders_ 里（上面已查）；
        // 这里能到说明是连续竞价阶段残留在 con_engine_ 的单
        con_engine_->cancel(system_order_id);
    }

    std::cout << "[策略撤单] " << user_cancel.strategy_id
              << " 撤销订单: " << user_cancel.order_id
              << " (系统ID: " << system_order_id << ")" << std::endl;
}

// 集合竞价 settle 时对暂存的影子订单做成交判定
// is_close=false 表示开盘集合竞价（call_engine_），true 表示收盘集合竞价（close_engine_）
// 成交判定规则：
//   - 市价单 → 全量成交 @ auction_px
//   - 限价买 且 price >= auction_px → 全量成交 @ auction_px
//   - 限价卖 且 price <= auction_px → 全量成交 @ auction_px
// 未成交处理：
//   - 开盘：结转到连续竞价 con_engine_（仍然是 USER 影子单，不进真实 orderbook 历史订单流）
//   - 收盘：发 'D' 撤单回调，丢弃
void BacktestEngine::settleAuctionUserOrders(bool is_close) {
    if (pending_auction_orders_.empty()) return;

    Price auction_px = is_close ? close_engine_->getPredictPrice()
                                : call_engine_->getPredictPrice();

    std::vector<PendingAuctionOrder> remaining;
    remaining.reserve(pending_auction_orders_.size());

    for (auto& po : pending_auction_orders_) {
        // 非当前阶段的影子单保留
        if (po.is_close_auction != is_close) {
            remaining.push_back(po);
            continue;
        }

        auto ord = po.order;
        bool can_fill = false;
        if (auction_px > 0) {
            if (ord->order_type == OrderType::Market) {
                can_fill = true;
            } else if (ord->direction == Direction::Buy && ord->price >= auction_px) {
                can_fill = true;
            } else if (ord->direction == Direction::Sell && ord->price <= auction_px) {
                can_fill = true;
            }
        }

        auto it_s = virtual_order_strategy_.find(ord->order_id);
        auto it_l = virtual_order_local_id_.find(ord->order_id);

        if (can_fill) {
            // 影子成交：全量 @ auction_px
            if (it_s != virtual_order_strategy_.end() && it_l != virtual_order_local_id_.end()) {
                const std::string& strategy_id = it_s->second;
                const std::string& local_id = it_l->second;

                updatePosition(strategy_id, symbol_, ord->direction, ord->volume, auction_px);

                auto callback = createTradeCallback(strategy_id, local_id,
                                                    ord->direction, ord->volume, auction_px,
                                                    current_datetime_, 'T');
                notifyStrategyTradeCallback(strategy_id, callback);

                std::cout << "[集合竞价影子成交] " << (is_close ? "收盘" : "开盘")
                          << " 策略 " << strategy_id
                          << " 订单 " << local_id
                          << " 成交 " << ord->volume << "@" << auction_px / 10000.0 << std::endl;
            }
            // 成交后清理映射
            virtual_order_strategy_.erase(ord->order_id);
            virtual_order_local_id_.erase(ord->order_id);
            user_order_direction_.erase(ord->order_id);

        } else if (!is_close) {
            // 开盘未成交：结转到连续竞价（con_engine_->accept 本身就是 USER 影子单路径）
            con_engine_->accept(ord);
            std::cout << "[集合竞价未成交结转] 订单 " << ord->order_local_id
                      << " 结转到连续竞价 (" << (ord->direction == Direction::Buy ? "买" : "卖")
                      << " " << ord->volume << "@" << ord->price / 10000.0 << ")" << std::endl;

        } else {
            // 收盘未成交：发 'D' 撤单回调
            if (it_s != virtual_order_strategy_.end() && it_l != virtual_order_local_id_.end()) {
                const std::string& strategy_id = it_s->second;
                const std::string& local_id = it_l->second;

                auto callback = createTradeCallback(strategy_id, local_id,
                                                    ord->direction, 0, ord->price,
                                                    current_datetime_, 'D');
                for (auto& strategy : strategies_) {
                    if (strategy->getStrategyId() == strategy_id) {
                        strategy->onTradeCallback(callback);
                        break;
                    }
                }
                std::cout << "[收盘集合竞价未成交撤单] 策略 " << strategy_id
                          << " 订单 " << local_id << std::endl;
            }
            virtual_order_strategy_.erase(ord->order_id);
            virtual_order_local_id_.erase(ord->order_id);
            user_order_direction_.erase(ord->order_id);
        }
    }

    pending_auction_orders_ = std::move(remaining);
}

// 通知策略订单成交
void BacktestEngine::notifyStrategiesOnExecution(const Execution& ex) {
    std::string trade_datetime = current_datetime_;
    
    // 检查买方订单是否属于某个策略
    for (const auto& mapping : user_order_mapping_) {
        if (mapping.second == ex.buy_order_id) {
            // 找到对应的策略
            for (auto& strategy : strategies_) {
                if (strategy->getStrategyId() == virtual_order_strategy_[ex.buy_order_id]) {
                    // 调用原有接口（兼容性）
                    // strategy->onOrderFilled(mapping.first, ex.price, ex.volume);
                    
                    // 调用新的统一接口
                    auto callback = createTradeCallback(strategy->getStrategyId(), mapping.first,
                                                      Direction::Buy, ex.volume, ex.price, 
                                                      trade_datetime, 'T');
                    notifyStrategyTradeCallback(strategy->getStrategyId(), callback);
                    break;
                }
            }
            break;
        }
    }
    
    // 检查卖方订单是否属于某个策略
    for (const auto& mapping : user_order_mapping_) {
        if (mapping.second == ex.sell_order_id) {
            // 找到对应的策略
            for (auto& strategy : strategies_) {
                if (strategy->getStrategyId() == virtual_order_strategy_[ex.sell_order_id]) {
                    
                    // 调用新的统一接口
                    auto callback = createTradeCallback(strategy->getStrategyId(), mapping.first,
                                                      Direction::Sell, ex.volume, ex.price, 
                                                      trade_datetime, 'T');
                    notifyStrategyTradeCallback(strategy->getStrategyId(), callback);
                    break;
                }
            }
            break;
        }
    }
}

// 创建交易回调对象
TradeCallback BacktestEngine::createTradeCallback(const std::string& strategy_id, const std::string& order_id, 
                                                 Direction direction, Quantity volume, Price price, 
                                                 const std::string& datetime, char match_type) {
    char dir_char = (direction == Direction::Buy) ? 'B' : 'S';
    double match_amount = (price / 10000.0) * volume;  // 转换为元
    
    // 先更新持仓，再获取总持仓量
    int64_t total_position = 0;
    for (auto& strategy : strategies_) {
        if (strategy->getStrategyId() == strategy_id) {
            // 如果是成交，更新持仓
            if (match_type == 'T') {
                int64_t position_change = (direction == Direction::Buy) ? 
                    static_cast<int64_t>(volume) : -static_cast<int64_t>(volume);
                strategy->updateStrategyPosition(symbol_, position_change);
            }
            // 获取当前总持仓量
            total_position = strategy->getPosition(symbol_);
            break;
        }
    }
    
    return TradeCallback(order_id, dir_char, volume, price, match_amount, total_position, datetime, match_type);
}

// 通知策略统一交易回调
void BacktestEngine::notifyStrategyTradeCallback(const std::string& strategy_id, const TradeCallback& callback) {
    for (auto& strategy : strategies_) {
        if (strategy->getStrategyId() == strategy_id) {
            // 直接调用统一回调接口
            strategy->onTradeCallback(callback);
            break;
        }
    }
}

// 创建下单回调对象
OrderCallback BacktestEngine::createOrderCallback(const std::string& strategy_id, const std::string& order_id,
                                                Direction direction, Quantity volume, Price price, 
                                                const std::string& datetime) {
    // 获取交易所代码（0=上海, 1=深圳）
    int exchange = (symbol_.find("SH") != std::string::npos) ? 0 : 1;
    
    // 获取当前买一卖一价格
    Price ask1 = orderbook_->bestAsk();
    Price bid1 = orderbook_->bestBid();
    
    // 获取当前总持仓量
    int64_t total_position = 0;
    for (auto& strategy : strategies_) {
        if (strategy->getStrategyId() == strategy_id) {
            total_position = strategy->getPosition(symbol_);
            break;
        }
    }
    
    // 转换方向（1=买入, 2=卖出）
    int dir = (direction == Direction::Buy) ? 1 : 2;
    
    return OrderCallback(datetime, exchange, ask1, bid1, total_position, price, volume, dir, order_id);
}

// 通知策略下单回调
void BacktestEngine::notifyStrategyOrderCallback(const std::string& strategy_id, const OrderCallback& callback) {
    for (auto& strategy : strategies_) {
        if (strategy->getStrategyId() == strategy_id) {
            strategy->onOrderCallback(callback);
            break;
        }
    }
}



/*
 * processEvent
 * 允许外部按时间顺序逐个推送事件给引擎，实现多合约的统一驱动。
 * 调用该方法会根据事件类型和当前回测阶段(集合竞价/连续竞价/收盘集合竞价)
 * 执行相应的撮合、快照更新和策略回调。
 */
void BacktestEngine::processEvent(const Event& ev, const std::unordered_set<int64_t>& /*will_trade_ids*/) {
    // 时间阈值(当日毫秒):09:25:00 / 14:57:00 / 15:00:00
    // 用事件预解析的 int64 毫秒键比较,不再每事件 substr(11,8) 堆分配
    static constexpr int64_t Call_Open_Ms  = (9 * 3600 + 25 * 60) * 1000LL;
    static constexpr int64_t Call_Close_Ms = (14 * 3600 + 57 * 60) * 1000LL;
    static constexpr int64_t End_Sec       = 15 * 3600;

    // 更新当前时间
    current_datetime_ = ev.datetime;
    current_ms_ = ev.datetime_ms;
    // 下单延迟：先释放"到达时刻 <= 当前事件时刻"的策略单/撤单（先于本事件进簿）
    if (any_latency_) drainDelayedEvents(ev.datetime_ms);
    const int64_t ev_ms_of_day = ev.datetime_ms % 86400000LL;
    if (ev_ms_of_day / 1000 > End_Sec) {  // 同原 "HH:MM:SS" > "15:00:00"(15:00:00.xxx 仍处理)
        return;
    }

    // 深市裸市价单(≥2023-04-10)事件驱动执行：消息序=orderid 序，本单的
    // 成交/撤单记录紧随委托连续到达。本条消息是它的成交 → 按记录吃掉簿内
    // 对手（executeMarketRealTrade）；是它的撤单 → 进场即撤登记（后续撤单
    // 处理由 entry_cancelled 表自动吸收）；都不是 → 事件段终结：剩余量以
    // 最后成交价（=对方最优一档）挂簿，后续被动成交交自主撮合。
    if (pending_market_hist_) {
        auto od = pending_market_hist_;
        const uint64_t pid = od->input_id;
        const bool is_my_tra = (ev.source == "tra" && !ev.exectype.empty()
            && (static_cast<uint64_t>(ev.bidorderid) == pid
                || static_cast<uint64_t>(ev.askorderid) == pid));
        if (is_my_tra && ev.exectype[0] == '1') {
            const bool od_on_bid = (static_cast<uint64_t>(ev.bidorderid) == pid);
            const uint64_t counter = od_on_bid
                ? static_cast<uint64_t>(ev.askorderid)
                : static_cast<uint64_t>(ev.bidorderid);
            con_engine_->executeMarketRealTrade(od, ev.price, ev.size,
                                                counter, ev.channelno);
            pending_market_last_px_ = ev.price;
            // 不清 pending：消息段内可能还有下一条本单成交
        } else if (is_my_tra && ev.exectype[0] == '2') {
            // 撤单回执（含零成交即撤与数据缺损场景）；有成交的剩余撤销
            // 实证不存在（挂簿后撤单走正常路径），统一按进场即撤吸收
            orderbook_->markEntryCancelled(od->order_id);
            pending_market_hist_ = nullptr;
        } else {
            // 非本单消息 → 事件段终结
            pending_market_hist_ = nullptr;
            if (od->remaining_volume() > 0) {
                if (od->traded_volume > 0) {
                    con_engine_->placeMarketRemainderOnBook(od, pending_market_last_px_);
                } else {
                    // 零成交且撤单回执未紧随（消息序被跳过）：按进场即撤处理
                    orderbook_->markEntryCancelled(od->order_id);
                }
            }
        }
    }

    // 出笼回放段（事件驱动出笼兜底，见 replaying_caged_ 声明处注释）：
    // 本条消息是某回放中订单的成交 → 按记录吃掉簿内对手；对每个回放中
    // 订单，非本单引用的消息 → 该单终结，剩余量按原限价挂簿交自主撮合。
    if (!replaying_caged_.empty() && con_engine_) {
        std::vector<uint64_t> finished;
        for (auto& [pid, od] : replaying_caged_) {
            const bool is_my_tra = (ev.source == "tra" && !ev.exectype.empty()
                && (static_cast<uint64_t>(ev.bidorderid) == pid
                    || static_cast<uint64_t>(ev.askorderid) == pid));
            if (is_my_tra && ev.exectype[0] == '1') {
                const bool od_on_bid = (static_cast<uint64_t>(ev.bidorderid) == pid);
                const uint64_t counter = od_on_bid
                    ? static_cast<uint64_t>(ev.askorderid)
                    : static_cast<uint64_t>(ev.bidorderid);
                try {
                    con_engine_->executeMarketRealTrade(od, ev.price, ev.size,
                                                        counter, ev.channelno);
                } catch (const MarketIdentityError& e) {
                    // 校验失败（簿偏离致对手/量对不上，簿未被改动）：
                    // 降级回笼等待收盘竞价恢复，差异由对账暴露
                    std::cerr << "⚠️ [出笼回放失败] " << e.what() << std::endl;
                    con_engine_->suspendHistoricalOrder(od);
                    finished.push_back(pid);
                    continue;
                }
                // 不终结：吃穿串可能还有下一条本单成交
            } else {
                finished.push_back(pid);
            }
        }
        for (uint64_t pid : finished) {
            auto it = replaying_caged_.find(pid);
            auto od = it->second;
            replaying_caged_.erase(it);
            if (od->remaining_volume() > 0) {
                // 出笼撮合的剩余量回笼（创业板暂存语义，见
                // activateEligibleSuspendedHistorical 同款注释：300568.SZ
                // 28.71 买出笼成交 100 后剩 1900 回笼，挂簿会被同毫秒
                // 深穿价卖单多吃）
                con_engine_->suspendHistoricalOrder(od);
            }
        }
    }

    // 价格笼子反推确认：上一条挂起的穿价历史单，看紧挨的本条消息——
    // 是它的成交（tra, exectype='1', 引用其市场 orderid）→ 放行：正常 accept
    //   自主撮合。消息序=交易所处理序：真实里穿价单若立即成交，紧邻消息必是
    //   它的成交；挂起仅延迟一拍（竞争单必在其后），FIFO 排队位置不变，
    //   撮合对手与量由引擎簿自主重建（与真实一致）。
    // 不是 → 入笼（不进簿、十档不可见，等待出笼条件或撤单或收盘竞价恢复）。
    const bool is_real_trade_event = (ev.source == "tra" && !ev.exectype.empty()
                                      && ev.exectype[0] == '1');
    if (pending_crossing_hist_ && con_engine_) {
        auto od = pending_crossing_hist_;
        pending_crossing_hist_ = nullptr;
        // 同毫秒确认判据：正常穿价进簿的撮合成交与委托同毫秒（主机即时
        // 处理）；入笼后出笼的吃穿成交必然跨毫秒（价格落回需市场事件驱动，
        // 300745.SZ 2022-06-27 实证：53.73 卖 820ms 后出笼吃穿被误确认
        // 放行，自主撮合从引擎簿偏差价位吃穿，全天错配级联）。跨毫秒的
        // 本单成交 → 入笼，出笼交由数值判据扫描/事件驱动回放。
        const bool next_is_my_trade = (ev.source == "tra" && !ev.exectype.empty()
            && ev.exectype[0] == '1'
            && ev.datetime_ms == pending_crossing_ms_
            && (static_cast<uint64_t>(ev.bidorderid) == od->input_id
                || static_cast<uint64_t>(ev.askorderid) == od->input_id));
        // 簿顶价一致性校验：立即撮合的首笔成交必然打在簿顶价（被动方价格）。
        // 同毫秒成交但价≠簿顶 → 真实并非"进场即撮合"，而是同毫秒内的
        // 状态变更（撤单打掉簿顶→笼子边界移动→出笼）后的成交——
        // 300568.SZ 2022-06-29 实证：卖 28.13 数值上出笼（买一 28.71×
        // 0.98=28.1358→28.14）被暂存，同毫秒撤单撤掉买一 17697508 剩
        // 1900 后边界落到 28.10 出笼，打在 28.67 的 16121963；adata 压平
        // 记录里该成交排在撤单记录前，若见此成交即 accept 会错打 28.71 档
        // 多吃 400 股。价不符 → 入笼：本笔真实成交由出笼回放兜底按记录
        // 精确回放（同事件后续 tra 处理段命中 releaseCagedForReplay）。
        const bool book_top_match = [&] {
            const bool buy = od->direction == Direction::Buy;
            const Price top = buy ? orderbook_->bestAsk() : orderbook_->bestBid();
            return top > 0 && ev.price == top;
        }();
        if (next_is_my_trade && book_top_match) {
            con_engine_->accept(od);
        } else {
            con_engine_->suspendHistoricalOrder(od);
        }
    }
    
    // ========== 价格笼子功能已禁用 ==========
    // // 首次事件时自动检测是否启用价格笼子（2023年3月1日前启用）
    // if (!price_cage_checked_ && con_engine_) {
    //     price_cage_checked_ = true;
    //     con_engine_->checkAndEnablePriceCage(current_datetime_);
    //     if (con_engine_->isPriceCageEnabled()) {
    //         std::cout << "[价格笼子] 检测到日期 " << current_datetime_.substr(0, 10) 
    //                   << " < 2023-03-01，已启用价格笼子规则" << std::endl;
    //     }
    // }
    
    // 收集策略产生的用户事件
    std::vector<UserEvent> strategy_events;

    // 进场即撤单的真实撤单记录:身份校验后吸收为 no-op,不做簿操作
    // (订单在入场时因定价基准侧空簿被交易所当场自动撤销,见 accept_sz)
    bool entry_cancel_absorbed = false;

    // 根据事件类型调用不同的策略回调
    if (ev.source == "ord") {
        // 委托事件：将Event转换为OrderDetail
        OrderDetail order;
        order.Exchange = ev.exchange != -1 ? ev.exchange : (ev.sym.find(".SZ") != std::string::npos ? 1 : 0);
        order.Instrument = ev.sym;
        order.Time = ev.time_raw != -1 ? ev.time_raw : 0;
        order.ChannelNo = ev.channelno;
        order.OrderNo = ev.orderid;
        order.Price = ev.price;
        order.Volume = ev.size;
        order.Side = ev.side == 1 ? "1" : "2"; // 1=买, 2=卖  
        order.OrderKind = ev.order_kind != '\0' ? ev.order_kind : (ev.ordertype == 1 ? '1' : '2');
        order.SeqNo = ev.seqno;
        order.BizIndex = ev.bizindex;
        
        // 原始委托必须在它可能触发的计算成交之前、按原始数量发布。
        for (auto& strategy : strategies_) {
            auto user_events = strategy->onOrderEvent(order);
            for (const auto& ue : user_events) {
                strategy_events.push_back(ue);
            }
        }
    } else if (ev.source == "tra") {
        // 成交/撤单事件：将Event转换为TradeDetail。
        // 注意：exectype='1' 的真实成交事件仅在价格笼子反推法激活时进流
        //（OrderLoader 条件放行），仅用于 pending 穿价单确认（见函数开头），
        // 不透传给策略（成交回报保持引擎重建口径），也不做任何簿操作。
        TradeDetail trade;
        trade.Exchange = ev.exchange != -1 ? ev.exchange : (ev.sym.find(".SZ") != std::string::npos ? 1 : 0);
        trade.Instrument = ev.sym;
        trade.ChannelNo = ev.channelno;
        trade.TradeIndex = ev.trade_index != -1 ? ev.trade_index : ev.tradeid;
        trade.Time = ev.time_raw != -1 ? ev.time_raw : 0;
        trade.Price = ev.price;
        trade.Volume = ev.size;
        trade.ExecType = ev.exectype.empty() ? '1' : ev.exectype[0]; // '1'=成交, '2'=撤销
        if (ev.bidorderid < 0 || ev.askorderid < 0) {
            throw MarketIdentityError("成交/撤单订单 ID 不能为负数");
        }
        trade.BuyNo = static_cast<uint64_t>(ev.bidorderid);
        trade.SellNo = static_cast<uint64_t>(ev.askorderid);
        trade.TradeBSFlag = ev.tradebsflag.empty() ? 'N' : ev.tradebsflag[0];
        trade.BizIndex = ev.bizindex;

        if (is_real_trade_event) {
            // 真实成交事件：已完成 pending 确认（函数开头），其余 no-op。
            // 事件驱动出笼兜底：成交引用的订单仍在笼中 → 数值判据漏判了
            // 出笼（引擎簿偏差），强制取出进入回放段，本笔即回放首笔。
            if (cage_inference_active_ && con_engine_ && !entry_cancel_absorbed) {
                auto od = con_engine_->releaseCagedForReplay(
                    static_cast<uint64_t>(trade.BuyNo),
                    static_cast<int>(trade.ChannelNo));
                if (!od) {
                    od = con_engine_->releaseCagedForReplay(
                        static_cast<uint64_t>(trade.SellNo),
                        static_cast<int>(trade.ChannelNo));
                }
                if (od) {
                    const bool od_on_bid =
                        (static_cast<uint64_t>(trade.BuyNo) == od->input_id);
                    const uint64_t counter = od_on_bid
                        ? static_cast<uint64_t>(trade.SellNo)
                        : static_cast<uint64_t>(trade.BuyNo);
                    try {
                        con_engine_->executeMarketRealTrade(
                            od, ev.price, ev.size, counter, ev.channelno);
                    } catch (const MarketIdentityError& e) {
                        // 校验失败（簿偏离致对手/量对不上，簿未被改动）：
                        // 降级回笼等待收盘竞价恢复，差异由对账暴露
                        std::cerr << "⚠️ [出笼回放失败] " << e.what() << std::endl;
                        con_engine_->suspendHistoricalOrder(od);
                        od = nullptr;
                    }
                    if (od && od->remaining_volume() > 0) {
                        replaying_caged_[od->input_id] = od;  // 连续成交串继续回放
                    }
                }
            }
        } else if (trade.ExecType == '2') {
            if ((trade.BuyNo != 0) == (trade.SellNo != 0)) {
                throw MarketIdentityError("撤单事件必须且只能有一个非零 BuyNo/SellNo");
            }

            const uint64_t market_order_id = trade.BuyNo != 0
                ? static_cast<uint64_t>(trade.BuyNo)
                : static_cast<uint64_t>(trade.SellNo);
            const Direction expected_direction = trade.BuyNo != 0
                ? Direction::Buy : Direction::Sell;
            const auto system_id = orderbook_->findSystemOrderId(
                market_order_id, static_cast<int>(trade.ChannelNo));
            if (!system_id.has_value()) {
                // 活跃表未命中 → 查进场即撤表:该订单入场时因定价基准侧空簿
                // 被交易所当场自动撤销,这条撤单记录即自动撤销的回执,
                // 身份校验后吸收为 no-op。
                const auto ec_id = orderbook_->findEntryCancelledSystemId(
                    market_order_id, static_cast<int>(trade.ChannelNo));
                if (ec_id.has_value()) {
                    const auto& ec_identity = orderbook_->requireMarketIdentity(*ec_id);
                    if (ec_identity.direction != expected_direction || ec_identity.instrument != ev.sym ||
                        (ev.trading_day > 0 && ec_identity.trading_day > 0 &&
                         ec_identity.trading_day != ev.trading_day)) {
                        throw MarketIdentityError(
                            "撤单市场身份与 BuyNo/SellNo 方向、标的或交易日不一致(进场即撤): symbol=" + ev.sym +
                            ", datetime=" + ev.datetime +
                            ", channel=" + std::to_string(trade.ChannelNo) +
                            ", order_id=" + std::to_string(market_order_id));
                    }
                    entry_cancel_absorbed = true;
                } else {
                    // 两表均未命中：撤单引用的委托在引擎侧已完结（被撮合吃完
                    // 或已撤）。源数据侧"撤单引用未注册委托"已被预检拦截
                    // （data_incomplete 不会进引擎），走到这里必然是引擎簿与
                    // 真实的偏差或同毫秒消息序歧义（真实先撤后吃、引擎先吃
                    // 后撤，301089.SZ 2022-06-06 实证）。吸收为 no-op 并告警，
                    // 让差异在对账中显性量化，而非整批 engine_fail 连坐。
                    std::cerr << "⚠️ [撤单吸收] 引用引擎侧已完结订单(簿偏差或"
                                 "同毫秒序歧义): symbol=" << ev.sym
                              << " datetime=" << ev.datetime
                              << " channel=" << trade.ChannelNo
                              << " order_id=" << market_order_id << std::endl;
                    entry_cancel_absorbed = true;
                }
            } else {

            const auto& identity = orderbook_->requireMarketIdentity(*system_id);
            if (identity.direction != expected_direction || identity.instrument != ev.sym ||
                (ev.trading_day > 0 && identity.trading_day > 0 &&
                 identity.trading_day != ev.trading_day)) {
                throw MarketIdentityError(
                    "撤单市场身份与 BuyNo/SellNo 方向、标的或交易日不一致: symbol=" + ev.sym +
                    ", datetime=" + ev.datetime +
                    ", channel=" + std::to_string(trade.ChannelNo) +
                    ", order_id=" + std::to_string(market_order_id));
            }
            }
        }
        
        if (!is_real_trade_event) {
            // 仅撤单事件透传给策略（现状口径）；真实成交事件不透传
            for (auto& strategy : strategies_) {
                auto user_events = strategy->onTradeEvent(trade);
                for (const auto& ue : user_events) {
                    strategy_events.push_back(ue);
                }
            }
        }
    }
    // 注意：onTickEvent的调用保持不变，在后面的分支中处理

    // 根据是否进入连续竞价阶段分别处理
    if (!continuous_mode_) {
        // 集合竞价阶段
        if (ev.source == "ord") {
            // 历史委托推送
            Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
            auto ord = orderbook_->createHistoricalOrder(
                static_cast<uint64_t>(ev.orderid), static_cast<int>(ev.channelno), ev.trading_day,
                "BRK", "AC", orderbook_->getExchange(), ev.sym, std::to_string(ev.orderid),
                toOrderType(ev), dir, ev.price, ev.size, ev.bizindex);
            
            call_engine_->accept(ord);
            data_manager_->updateOrderDetail(ev, 'A');
        } else if (ev.source == "tra" && !is_real_trade_event) {
            // 历史撤单推送（真实成交事件不进集合竞价段，防御性排除）
            uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;

            if (!entry_cancel_absorbed)
                call_engine_->cancel_by_input_id(oid_raw, static_cast<int>(ev.channelno));
        } else {
            // tick 推送
            data_manager_->updateSnapshot(ev);
            has_real_tick_ = true;
            auto snapshot = data_manager_->getSnapshot();
            for (auto& strategy : strategies_) {
                auto user_events = strategy->onTickEvent(snapshot);
                for (const auto& ue : user_events) {
                    pending_trade_events_.push_back(ue);
                }
            }
        }

        // 判断是否结束集合竞价阶段
        if (ev_ms_of_day >= Call_Open_Ms) {
            // 使用 09:25:00.000 统一时间戳
            std::string auction_time = ev.datetime.substr(0, 11) + "09:25:00.000";
            current_datetime_ = auction_time;
            call_engine_->settle();
            continuous_mode_ = true;
            // 集合竞价 settle 之后，对暂存的用户影子单做成交/结转判定
            // 必须在 continuous_mode_ = true 之后、连续竞价正式开始之前调用
            // 未成交订单会通过 con_engine_->accept 结转到连续竞价
            settleAuctionUserOrders(/*is_close=*/false);
            std::cout << "[09:25] 集合竞价完成，开盘价=" << call_engine_->getPredictPrice() / 10000.0
                      << " 真实开盘价=" << actual_open_ / 10000.0
                      << " 开盘成交量=" << call_engine_->getPredictVolume() << std::endl;
            std::cout << "连续竞价开始" << std::endl;
            // 恢复当前时间为事件时间
            current_datetime_ = ev.datetime;
        }
    } else {
        // 连续竞价阶段
        if (!closing_mode_ && ev_ms_of_day >= Call_Close_Ms) {
            closing_mode_ = true;
            close_engine_->bootstrap_from_orderbook();
            // 价格笼子：14:57 收盘集合竞价开始，笼中订单恢复参与竞价撮合
            //（历史笼单作为收盘竞价委托；策略笼单转影子单走收盘竞价判定）
            if (con_engine_) {
                for (auto& od : con_engine_->takeAllSuspendedHistorical()) {
                    close_engine_->accept(od);
                }
                for (auto& od : con_engine_->takeAllUserCageOrders()) {
                    od->status = OrderStatus::Pending;
                    pending_auction_orders_.push_back({od, /*is_close_auction=*/true});
                }
            }
            std::cout << "[" << ev.datetime.substr(11, 8) << "] 进入收盘集合竞价阶段" << std::endl;
        }

        if (!closing_mode_) {
            // 普通连续竞价
            if (ev.source == "ord") {
                Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                auto ord = orderbook_->createHistoricalOrder(
                    static_cast<uint64_t>(ev.orderid), static_cast<int>(ev.channelno), ev.trading_day,
                    "BRK", "AC", orderbook_->getExchange(), ev.sym, std::to_string(ev.orderid),
                    toOrderType(ev), dir, ev.price, ev.size, ev.bizindex);

                // 价格笼子反推（仅深市创业板暂存窗口激活）：
                // 穿价的限价历史单先挂起，等紧挨的下一条消息确认
                //（是它的成交→放行；不是→入笼不可见，见函数开头的确认逻辑）。
                // 市价单不适用笼子，直接进撮合（转换价来自盘口，天然合规）。
                bool crossing_hist = false;
                if (cage_inference_active_ && ord->order_type == OrderType::Limit) {
                    const bool buy = ord->direction == Direction::Buy;
                    const Price opp = buy ? orderbook_->bestAsk() : orderbook_->bestBid();
                    crossing_hist = (opp > 0 && (buy ? ord->price >= opp : ord->price <= opp));
                }

                // 深市裸市价单(≥2023-04-10)：子类被 adata 压平，accept 阶段
                // 无法正确转换（见 accept_sz 注释），挂起走事件驱动执行
                bool naked_sz_market = (ord->order_type == OrderType::Market
                    && ord->price == 0
                    && ev.sym.find(".SZ") != std::string::npos
                    && ord->market_trading_day >= 20230410);
                if (naked_sz_market) {
                    pending_market_hist_ = ord;
                    pending_market_last_px_ = 0;
                } else if (crossing_hist) {
                    pending_crossing_hist_ = ord;  // 不进簿：十档暂不可见
                    pending_crossing_ms_ = ev.datetime_ms;
                } else {
                    con_engine_->accept(ord);
                }
                data_manager_->updateOrderDetail(ev, 'A');
                last_brk_datetime_ = ev.datetime;

            } else if (ev.source == "tra" && is_real_trade_event) {
                // 真实成交事件（仅反推法激活时进流）：pending 确认已在函数开头完成。
                // 历史成交重建由 ord 事件驱动的撮合完成（深市已验证逐笔精确一致），
                // 此处不做任何簿操作、不推策略，避免双重处理。
            } else if (ev.source == "tra") {
                uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                if (!entry_cancel_absorbed)
                    con_engine_->cancel_by_input_id(oid_raw, static_cast<int>(ev.channelno));
            } else {
                data_manager_->updateSnapshot(ev);
                has_real_tick_ = true;
                auto snapshot = data_manager_->getSnapshot();
                for (auto& strategy : strategies_) {
                    auto user_events = strategy->onTickEvent(snapshot);
                    for (const auto& ue : user_events) {
                        pending_trade_events_.push_back(ue);
                    }
                }
            }
        } else {
            // 收盘集合竞价阶段(>15:00:00 已在函数开头统一拦截,此处无需重复判断)
            if (ev.source == "ord") {
                Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                auto ord = orderbook_->createHistoricalOrder(
                    static_cast<uint64_t>(ev.orderid), static_cast<int>(ev.channelno), ev.trading_day,
                    "BRK", "AC", orderbook_->getExchange(), ev.sym, std::to_string(ev.orderid),
                    toOrderType(ev), dir, ev.price, ev.size, ev.bizindex);
                close_engine_->accept(ord);
                data_manager_->updateOrderDetail(ev, 'A');
            } else if (ev.source == "tra") {
                uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                if (!entry_cancel_absorbed)
                    close_engine_->cancel_by_input_id(oid_raw, static_cast<int>(ev.channelno));
            } else {
                data_manager_->updateSnapshot(ev);
                has_real_tick_ = true;
                auto snapshot = data_manager_->getSnapshot();
                for (auto& strategy : strategies_) {
                    auto user_events = strategy->onTickEvent(snapshot);
                    for (const auto& ue : user_events) {
                        pending_trade_events_.push_back(ue);
                    }
                }
            }
        }
    }

    // 实时合成 tick：当前事件撮合完成后，若跨过间隔网格边界则推送
    // 返回的用户事件进 pending_trade_events_，与 onTickEvent 走同一条 dispatch 链路
    maybeEmitRealTimeTick(ev);

    // 统一分发：按 symbol 过滤，跨标的订单暂存到 cross_symbol_events_
    // 由上层 MultiBacktestEngine 在 Taskflow join 之后串行路由到正确的子引擎
    // 注意：集合竞价阶段也允许订单进入 processUserEvent，由 processUserOrder 内部决定
    // 是走连续竞价撮合还是暂存到 pending_auction_orders_（影子单）
    auto dispatch = [this](const UserEvent& ue) {
        if (ue.type == UserEvent::ORDER) {
            if (ue.order.symbol != symbol_) {
                cross_symbol_events_.push_back(ue);
                return;
            }
        } else { // CANCEL
            if (user_order_mapping_.find(ue.cancel.order_id) == user_order_mapping_.end()
                && delayed_order_ids_.find(ue.cancel.order_id) == delayed_order_ids_.end()) {
                cross_symbol_events_.push_back(ue);
                return;
            }
        }
        enqueueOrDispatch(ue);  // 下单延迟：开了延迟的策略入队，否则同步处理
    };

    // 批量成交推送:本市场事件撮合产生的全部历史成交一次跨边界。
    // 策略返回的用户事件进 pending_trade_events_,与原先逐笔推送同序分发
    if (!pending_trade_batch_.empty()) {
        for (auto& strategy : strategies_) {
            auto user_events = strategy->onTradeEventsBatch(pending_trade_batch_);
            for (const auto& event : user_events) {
                pending_trade_events_.push_back(event);
            }
        }
        pending_trade_batch_.clear();
    }

    for (const auto& ue : strategy_events)       dispatch(ue);
    for (const auto& ue : pending_trade_events_) dispatch(ue);
    pending_trade_events_.clear();

    // 价格笼子出笼扫描（连续竞价段，每条逐笔消息处理后）：
    // 历史笼单与策略笼单均按数值规则重查（共用 userCageBounds，判据=价格落回
    // 有效申报范围；非"不再穿价"——出笼时可仍穿价，300026 边界单实测锁定）。
    // 数值判据的基准来自引擎簿，边界单可能漏放（由事件驱动回放兜底，
    // 见 replaying_caged_）或早放（罕见，对账暴露）。放在事件快照推送之前，
    // 出笼效果体现在本次快照中。
    if (continuous_mode_ && !closing_mode_ && con_engine_) {
        if (cage_inference_active_) {
            con_engine_->activateEligibleSuspendedHistorical(cage_rule_);
        }
        if (user_cage_enabled_ && cage_rule_.enabled
            && cage_rule_.action == CageAction::Dormant) {
            con_engine_->activateEligibleUserCageOrders(cage_rule_);
        }
    }

    // 事件驱动快照：每个市场事件（ord/tra）的全部处理（含上面 dispatch 的
    // 策略响应下单/撤单）结束后，从当前订单簿合成快照推送一次。
    // 策略在回调里返回的新事件立即 dispatch（撮合发生在本事件内，
    // 其对盘口的影响反映在下一次事件快照中）。tick 事件不触发
    // （真实 tick 已有 onTickEvent 官方口径推送）。
    if (event_snapshot_enabled_ && (ev.source == "ord" || ev.source == "tra")
        && !strategies_.empty()) {
        const Snapshot snapshot = buildRealTimeSnapshot(ev);
        for (auto& strategy : strategies_) {
            auto user_events = strategy->onEventSnapshot(snapshot);
            for (const auto& ue : user_events) dispatch(ue);
        }
    }
}

/*
 * finish
 * 在调用 processEvent() 完成所有事件后，调用此方法执行收盘结算并输出结果。
 * 会先等待所有策略完成处理，确保没有遗漏的事件。
 */
void BacktestEngine::finish() {
    // 等待所有策略完成处理
    waitForStrategiesCompletion();

    // 下单延迟收尾：收盘时刻(15:00:00.000 含)前"到达"的订单照常进场
    // （收市竞价时段到达的进 pending_auction_orders_，随下面 settle 判定）；
    // 收盘后才到达的订单，真实中交易所已拒收，丢弃并提示
    // （尾部单按规则须严格早于 15:00 才释放，恰在 15:00 到达的尾部单一并丢弃）
    if (any_latency_) {
        drainDelayedEvents(15LL * 3600 * 1000);
        for (auto* q : {&delayed_head_, &delayed_tail_}) {
            while (!q->empty()) {
                const auto& d = q->top();
                std::cout << "[下单延迟] 到达时刻已过 15:00，未进场即丢弃: "
                          << (d.event.type == UserEvent::ORDER ? d.event.order.order_id
                                                               : d.event.cancel.order_id)
                          << std::endl;
                if (d.event.type == UserEvent::ORDER)
                    delayed_order_ids_.erase(d.event.order.order_id);
                q->pop();
            }
        }
    }

    // 如果已进入收盘集合竞价阶段，则结算
    if (closing_mode_) {
        close_engine_->settle();
        // 收盘集合竞价 settle 之后，对暂存的用户影子单做成交判定；未成交直接撤单
        settleAuctionUserOrders(/*is_close=*/true);
        // 批量成交推送 flush:finish() 在 processEvent 之外,收盘竞价成交在此
        // 一次性推送策略(返回的用户事件维持原语义:进 pending 队列,不补 dispatch)
        if (!pending_trade_batch_.empty()) {
            for (auto& strategy : strategies_) {
                auto user_events = strategy->onTradeEventsBatch(pending_trade_batch_);
                for (const auto& event : user_events) {
                    pending_trade_events_.push_back(event);
                }
            }
            pending_trade_batch_.clear();
        }
        std::cout << "[收盘集合竞价] 成交价=" << close_engine_->getPredictPrice() / 10000.0
                  << " 成交量=" << close_engine_->getPredictVolume() << std::endl;
    }
    std::cout << "同步交互式回测完成" << std::endl;

    // (DIAG-8 RT MODE SUMMARY 诊断日志已移除)

    // 输出交易记录
    if (recording_enabled_) {
        writeTradeRecords();
        std::cout << "交易记录已输出到: " << trade_output_file_ << std::endl;
        std::cout << "总交易笔数: " << trade_records_.size() << std::endl;
    }
    // printResults();
}

// 检查所有策略是否已完成处理
bool BacktestEngine::areAllStrategiesComplete() const {
    // 检查所有策略是否完成处理
    for (const auto& strategy : strategies_) {
        if (!strategy->isProcessingComplete()) {
            return false;
        }
    }
    
    // 检查是否还有待处理的事件
    if (!pending_trade_events_.empty()) {
        return false;
    }
    
    return true;
}

// 等待所有策略完成处理
void BacktestEngine::waitForStrategiesCompletion() {
    const int max_wait_iterations = 1000; // 最大等待轮次
    int wait_count = 0;
    
    std::cout << "等待策略完成处理中..." << std::endl;
    
    while (!areAllStrategiesComplete() && wait_count < max_wait_iterations) {
        wait_count++;
        
        // 输出等待状态信息
        if (wait_count % 100 == 0) {
            std::cout << "等待第 " << wait_count << " 轮: ";
            
            // 显示未完成的策略
            for (const auto& strategy : strategies_) {
                if (!strategy->isProcessingComplete()) {
                    std::cout << strategy->getStrategyId() << " ";
                }
            }
            
            // 显示待处理事件数量
            if (!pending_trade_events_.empty()) {
                std::cout << "(待处理事件: " << pending_trade_events_.size() << ") ";
            }
            
            std::cout << std::endl;
        }
        
        // 短暂休眠，给策略时间完成处理（如果是异步策略）
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    if (wait_count >= max_wait_iterations) {
        std::cout << "⚠️  等待超时！部分策略可能未完成处理:" << std::endl;
        
        for (const auto& strategy : strategies_) {
            if (!strategy->isProcessingComplete()) {
                std::cout << "   - " << strategy->getStrategyId() << " 未完成" << std::endl;
            }
        }
        
        if (!pending_trade_events_.empty()) {
            std::cout << "   - 剩余待处理事件: " << pending_trade_events_.size() << " 个" << std::endl;
        }
    } else {
        std::cout << "✅ 所有策略处理完成 (等待轮次: " << wait_count << ")" << std::endl;
    }
}


} // namespace wangcai
