#pragma once
#include "backtest_engine.hpp"
#include <iostream>
#include <string>
#include <deque>

namespace wangcai {

// 同步跟单策略示例 - 演示严格同步的事件处理
class SyncFollowStrategy : public Strategy {
public:
    SyncFollowStrategy(const std::string& strategy_id = "SYNC_FOLLOW", 
                       int64_t min_trigger_size = 1000,
                       int64_t follow_volume = 100,
                       int64_t price_offset = 100)
        : strategy_id_(strategy_id), 
          min_trigger_size_(min_trigger_size),
          follow_volume_(follow_volume),
          price_offset_(price_offset),
          order_count_(0) {}

    std::vector<UserEvent> onOrderEvent(const Event& event) override {
        std::vector<UserEvent> events;
        
        // 只对新订单事件进行跟单
        if (event.source == "ord" && event.datetime.substr(11, 8) >= "09:30:00") {
            // 简单的跟单逻辑：每N个事件尝试下单一次
            if (order_count_ < 30 && ++event_count_ % 50 == 0) { // 最多下30单
                UserOrder order;
                order.order_id = strategy_id_ + "_FOLLOW_" + std::to_string(++order_count_);
                order.symbol = event.sym;
                order.direction = (order_count_ % 2 == 1) ? Direction::Buy : Direction::Sell;
                order.order_type = OrderType::Limit;
                
                if (order.direction == Direction::Buy) {
                    order.price = event.price - price_offset_; // 买单价格稍低
                } else {
                    order.price = event.price + price_offset_; // 卖单价格稍高
                }
                
                order.volume = follow_volume_;
                order.strategy_id = strategy_id_;
                
                events.push_back(UserEvent(order));
                
                std::cout << "[" << strategy_id_ << "] 跟单: " << order.order_id 
                          << " 方向: " << (order.direction == Direction::Buy ? "买" : "卖")
                          << " 价格: " << order.price / 10000.0 
                          << " 数量: " << order.volume << std::endl;
            }
        }
        
        return events;
    }

    std::vector<UserEvent> onTradeEvent(const Execution& execution, const std::string& datetime) override {
        std::vector<UserEvent> events;
        
        // 可以根据成交情况决定是否跟单
        if (execution.volume >= 500 && order_count_ < 30) {  // 大单成交时跟单，最多下30单
            UserOrder order;
            order.order_id = strategy_id_ + "_FOLLOW_TRADE_" + std::to_string(++order_count_);
            order.symbol = "000001SZ";  // 简化处理，使用固定symbol
            order.direction = Direction::Buy;  // 简单跟买
            order.order_type = OrderType::Limit;
            order.price = execution.price - price_offset_;  // 稍低价买入
            order.volume = 50;  // 小量跟单
            order.strategy_id = strategy_id_;
            
            events.push_back(UserEvent(order));
            
            std::cout << "[" << strategy_id_ << "] 大单跟单: " << order.order_id 
                      << " 价格: " << order.price / 10000.0 << std::endl;
        }
        
        return events;
    }

    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        std::cout << "========== 策略成交回报 ==========" << std::endl;
        std::cout << "策略ID: " << strategy_id_ << std::endl;
        std::cout << "订单ID: " << order_id << std::endl;
        std::cout << "成交价格: " << price / 10000.0 << " 元" << std::endl;
        std::cout << "成交数量: " << volume << std::endl;
        std::cout << "累计成交量: " << total_filled_volume_ + volume << std::endl;
        std::cout << "=================================" << std::endl;
        total_filled_volume_ += volume;
    }

    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        std::cout << "========== 策略撤单回报 ==========" << std::endl;
        std::cout << "策略ID: " << strategy_id_ << std::endl;
        std::cout << "订单ID: " << order_id << std::endl;
        std::cout << "撤单原因: " << reason << std::endl;
        std::cout << "=================================" << std::endl;
    }

    std::string getStrategyId() const override { return strategy_id_; }

    void printStatistics() const {
        std::cout << "\n=== " << strategy_id_ << " 统计信息 ===" << std::endl;
        std::cout << "总下单数: " << order_count_ << std::endl;
        std::cout << "总成交量: " << total_filled_volume_ << std::endl;
        std::cout << "处理事件数: " << event_count_ << std::endl;
    }

private:
    std::string strategy_id_;
    int64_t min_trigger_size_;
    int64_t follow_volume_;
    int64_t price_offset_;
    int order_count_;
    int event_count_ = 0;
    int64_t total_filled_volume_ = 0;
};

// 简单的均值回归策略示例
class MeanReversionStrategy : public Strategy {
public:
    MeanReversionStrategy(const std::string& strategy_id, double threshold = 0.02)
        : strategy_id_(strategy_id), threshold_(threshold), last_price_(0), order_count_(0) {}
    
    std::vector<UserEvent> onOrderEvent(const Event& event) override {
        // 均值回归策略不需要对订单事件做出响应
        return {};
    }

    std::vector<UserEvent> onTradeEvent(const Execution& execution, const std::string& datetime) override {
        std::vector<UserEvent> events;
        
        // 基于成交价格进行均值回归判断
        double trade_price = execution.price / 10000.0;
        double prev_close = 5.0; // 简化：假设前收盘价为5元
        
        if (last_price_ > 0) {
            double price_change = (trade_price - last_price_) / last_price_;
            
            // 如果价格下跌超过阈值，买入
            if (price_change < -threshold_ && order_count_ < 30) {
                UserOrder order;
                order.order_id = strategy_id_ + "_BUY_" + std::to_string(++order_count_);
                order.symbol = "000001SZ";  // 简化处理
                order.direction = Direction::Buy;
                order.order_type = OrderType::Limit;
                order.price = static_cast<Price>(trade_price * 0.999 * 10000); // 稍低于当前价
                order.volume = 100;
                order.strategy_id = strategy_id_;
                
                events.push_back(UserEvent(order));
                
                std::cout << "[" << strategy_id_ << "] 价格下跌 " << (price_change * 100) 
                          << "%，买入订单: " << order.order_id 
                          << " 价格: " << order.price / 10000.0 << std::endl;
            }
            // 如果价格上涨超过阈值，卖出
            else if (price_change > threshold_ && order_count_ < 30) {
                UserOrder order;
                order.order_id = strategy_id_ + "_SELL_" + std::to_string(++order_count_);
                order.symbol = "000001SZ";  // 简化处理
                order.direction = Direction::Sell;
                order.order_type = OrderType::Limit;
                order.price = static_cast<Price>(trade_price * 1.001 * 10000); // 稍高于当前价
                order.volume = 100;
                order.strategy_id = strategy_id_;
                
                events.push_back(UserEvent(order));
                
                std::cout << "[" << strategy_id_ << "] 价格上涨 " << (price_change * 100) 
                          << "%，卖出订单: " << order.order_id 
                          << " 价格: " << order.price / 10000.0 << std::endl;
            }
        }
        
        last_price_ = trade_price;
        return events;
    }
    
    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        std::cout << "========== 策略成交回报 ==========" << std::endl;
        std::cout << "策略ID: " << strategy_id_ << std::endl;
        std::cout << "订单ID: " << order_id << std::endl;
        std::cout << "成交价格: " << price / 10000.0 << " 元" << std::endl;
        std::cout << "成交数量: " << volume << std::endl;
        std::cout << "=================================" << std::endl;
    }
    
    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        std::cout << "========== 策略撤单回报 ==========" << std::endl;
        std::cout << "策略ID: " << strategy_id_ << std::endl;
        std::cout << "订单ID: " << order_id << std::endl;
        std::cout << "撤单原因: " << reason << std::endl;
        std::cout << "=================================" << std::endl;
    }
    
    std::string getStrategyId() const override {
        return strategy_id_;
    }
    
private:
    std::string strategy_id_;
    double threshold_;
    double last_price_;
    int order_count_;
};

// 专门测试成交回报和撤单回报的策略
class TestCallbackStrategy : public Strategy {
public:
    TestCallbackStrategy(const std::string& strategy_id = "TEST_CALLBACK")
        : strategy_id_(strategy_id), test_phase_(0), order_counter_(0) {}
    
    std::vector<UserEvent> onOrderEvent(const Event& event) override {
        std::vector<UserEvent> events;
        
        // 只在连续竞价阶段进行测试
        if (event.source == "ord" && event.datetime.substr(11, 8) >= "09:30:00" && test_phase_ < 6) {
            
            // 每处理100个事件执行一次测试
            if (++event_count_ % 100 == 0) {
                switch (test_phase_) {
                    case 0: // 测试立即成交
                        events = testImmediateFill(event);
                        break;
                    case 1: // 测试撮合引擎成交
                        events = testMatchingFill(event);
                        break;
                    case 2: // 测试主动撤单
                        events = testActiveCancel(event);
                        break;
                    case 3: // 测试批量下单成交
                        events = testBatchOrders(event);
                        break;
                    case 4: // 测试不同价格成交
                        events = testDifferentPrices(event);
                        break;
                    case 5: // 测试大单成交
                        events = testLargeOrders(event);
                        break;
                }
                test_phase_++;
                
                std::cout << "\n[测试策略] 开始第 " << test_phase_ << " 阶段测试" << std::endl;
            }
        }
        
        return events;
    }
    
    std::vector<UserEvent> onTradeEvent(const Execution& execution, const std::string& datetime) override {
        // 不响应成交事件，专注于测试回调
        return {};
    }
    
    std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) override {
        // // 不响应Tick事件，专注于测试回调
        // std::cout << "Tick is Coming!" << std::endl;
        // auto bid = snapshot.bids;
        // auto ask = snapshot.asks;
        // std::cout << "Tick Time is " << snapshot.datetime << '\n';
        // std::cout << "Bid: \n";
        // for (auto x: bid) {
        //     std::cout << "价格: " << x / 10000.0 << " 元" << std::endl;
        // }

        // std::cout << "Ask: \n";
        // for (auto x: ask) {
        //     std::cout << "价格: " << x / 10000.0 << " 元"<< std::endl;
        // }
        return {};
    }
    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        filled_count_++;
        total_filled_volume_ += volume;
        
        std::cout << "\n🎉========== 测试策略成交回报 ==========" << std::endl;
        std::cout << "✅ 策略ID: " << strategy_id_ << std::endl;
        std::cout << "✅ 订单ID: " << order_id << std::endl;
        std::cout << "✅ 成交价格: " << price / 10000.0 << " 元" << std::endl;
        std::cout << "✅ 成交数量: " << volume << std::endl;
        std::cout << "✅ 累计成交次数: " << filled_count_ << std::endl;
        std::cout << "✅ 累计成交量: " << total_filled_volume_ << std::endl;
        std::cout << "🎉========================================" << std::endl;
        
        // 如果是需要撤单的测试订单，记录下来准备撤单
        if (order_id.find("CANCEL_TEST") != std::string::npos) {
            orders_to_cancel_.push_back(order_id);
        }
    }
    
    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        cancelled_count_++;
        
        std::cout << "\n❌========== 测试策略撤单回报 ==========" << std::endl;
        std::cout << "❌ 策略ID: " << strategy_id_ << std::endl;
        std::cout << "❌ 订单ID: " << order_id << std::endl;
        std::cout << "❌ 撤单原因: " << reason << std::endl;
        std::cout << "❌ 累计撤单次数: " << cancelled_count_ << std::endl;
        std::cout << "❌========================================" << std::endl;
    }
    
    std::string getStrategyId() const override { return strategy_id_; }
    
    void printStatistics() const {
        std::cout << "\n🔍======= 测试策略统计报告 =======" << std::endl;
        std::cout << "📊 策略ID: " << strategy_id_ << std::endl;
        std::cout << "📊 总下单数: " << order_counter_ << std::endl;
        std::cout << "📊 成交回报次数: " << filled_count_ << std::endl;
        std::cout << "📊 撤单回报次数: " << cancelled_count_ << std::endl;
        std::cout << "📊 总成交量: " << total_filled_volume_ << std::endl;
        std::cout << "📊 处理事件数: " << event_count_ << std::endl;
        std::cout << "📊 测试阶段: " << test_phase_ << "/6" << std::endl;
        std::cout << "🔍===============================" << std::endl;
    }

private:
    // 测试立即成交（市价单或者价格很优的限价单）
    std::vector<UserEvent> testImmediateFill(const Event& event) {
        std::vector<UserEvent> events;
        
        std::cout << "[测试阶段1] 测试立即成交订单" << std::endl;
        
        UserOrder order;
        order.order_id = strategy_id_ + "_IMMEDIATE_" + std::to_string(++order_counter_);
        order.symbol = event.sym;
        order.direction = Direction::Buy;
        order.order_type = OrderType::Limit;
        order.price = event.price + 1000; // 高价买入，确保立即成交
        order.volume = 100;
        order.strategy_id = strategy_id_;
        
        events.push_back(UserEvent(order));
        
        std::cout << "📝 下单: " << order.order_id 
                  << " 高价买入 " << order.price / 10000.0 
                  << " 元，预期立即成交" << std::endl;
                  
        return events;
    }
    
    // 测试撮合引擎成交
    std::vector<UserEvent> testMatchingFill(const Event& event) {
        std::vector<UserEvent> events;
        
        std::cout << "[测试阶段2] 测试撮合引擎成交" << std::endl;
        
        UserOrder order;
        order.order_id = strategy_id_ + "_MATCHING_" + std::to_string(++order_counter_);
        order.symbol = event.sym;
        order.direction = Direction::Sell;
        order.order_type = OrderType::Limit;
        order.price = event.price - 200; // 低价卖出，等待撮合
        order.volume = 50;
        order.strategy_id = strategy_id_;
        
        events.push_back(UserEvent(order));
        
        std::cout << "📝 下单: " << order.order_id 
                  << " 低价卖出 " << order.price / 10000.0 
                  << " 元，等待撮合成交" << std::endl;
                  
        return events;
    }
    
    // 测试主动撤单
    std::vector<UserEvent> testActiveCancel(const Event& event) {
        std::vector<UserEvent> events;
        
        std::cout << "[测试阶段3] 测试主动撤单" << std::endl;
        
        // 先下一个不容易成交的订单
        UserOrder order;
        order.order_id = strategy_id_ + "_CANCEL_TEST_" + std::to_string(++order_counter_);
        order.symbol = event.sym;
        order.direction = Direction::Buy;
        order.order_type = OrderType::Limit;
        order.price = event.price - 1000; // 很低的价格，不容易成交
        order.volume = 200;
        order.strategy_id = strategy_id_;
        
        events.push_back(UserEvent(order));
        
        // 记录订单ID，稍后撤单
        pending_cancel_orders_.push_back(order.order_id);
        
        std::cout << "📝 下单: " << order.order_id 
                  << " 低价买入 " << order.price / 10000.0 
                  << " 元，准备稍后撤单" << std::endl;
                  
        // 如果有之前的订单需要撤单，现在撤掉
        if (!pending_cancel_orders_.empty() && pending_cancel_orders_.size() > 1) {
            std::string cancel_id = pending_cancel_orders_.front();
            pending_cancel_orders_.pop_front();
            
            UserCancel cancel(cancel_id, strategy_id_);
            events.push_back(UserEvent(cancel));
            
            std::cout << "❌ 撤单: " << cancel_id << std::endl;
        }
                  
        return events;
    }
    
    // 测试批量订单
    std::vector<UserEvent> testBatchOrders(const Event& event) {
        std::vector<UserEvent> events;
        
        std::cout << "[测试阶段4] 测试批量订单成交" << std::endl;
        
        // 下多个小单
        for (int i = 0; i < 3; i++) {
            UserOrder order;
            order.order_id = strategy_id_ + "_BATCH_" + std::to_string(++order_counter_);
            order.symbol = event.sym;
            order.direction = (i % 2 == 0) ? Direction::Buy : Direction::Sell;
            order.order_type = OrderType::Limit;
            order.price = event.price + (i % 2 == 0 ? 500 : -500); // 买高卖低
            order.volume = 30;
            order.strategy_id = strategy_id_;
            
            events.push_back(UserEvent(order));
            
            std::cout << "📝 批量下单 " << (i+1) << "/3: " << order.order_id 
                      << " " << (order.direction == Direction::Buy ? "买入" : "卖出")
                      << " " << order.price / 10000.0 << " 元" << std::endl;
        }
                  
        return events;
    }
    
    // 测试不同价格成交
    std::vector<UserEvent> testDifferentPrices(const Event& event) {
        std::vector<UserEvent> events;
        
        std::cout << "[测试阶段5] 测试不同价格成交" << std::endl;
        
        UserOrder order;
        order.order_id = strategy_id_ + "_PRICE_" + std::to_string(++order_counter_);
        order.symbol = event.sym;
        order.direction = Direction::Buy;
        order.order_type = OrderType::Limit;
        order.price = event.price + 300; // 稍高价格
        order.volume = 150;
        order.strategy_id = strategy_id_;
        
        events.push_back(UserEvent(order));
        
        std::cout << "📝 下单: " << order.order_id 
                  << " 买入 " << order.price / 10000.0 
                  << " 元，测试不同价格成交" << std::endl;
                  
        return events;
    }
    
    // 测试大单成交
    std::vector<UserEvent> testLargeOrders(const Event& event) {
        std::vector<UserEvent> events;
        
        std::cout << "[测试阶段6] 测试大单成交" << std::endl;
        
        UserOrder order;
        order.order_id = strategy_id_ + "_LARGE_" + std::to_string(++order_counter_);
        order.symbol = event.sym;
        order.direction = Direction::Sell;
        order.order_type = OrderType::Limit;
        order.price = event.price - 100; // 稍低价格
        order.volume = 500; // 大单
        order.strategy_id = strategy_id_;
        
        events.push_back(UserEvent(order));
        
        std::cout << "📝 下大单: " << order.order_id 
                  << " 卖出 " << order.price / 10000.0 
                  << " 元，数量 " << order.volume << std::endl;
                  
        return events;
    }

private:
    std::string strategy_id_;
    int test_phase_;
    int order_counter_;
    int event_count_ = 0;
    
    // 统计数据
    int filled_count_ = 0;
    int cancelled_count_ = 0;
    int64_t total_filled_volume_ = 0;
    
    // 测试用的数据结构
    std::vector<std::string> orders_to_cancel_;
    std::deque<std::string> pending_cancel_orders_;
};

} // namespace wangcai