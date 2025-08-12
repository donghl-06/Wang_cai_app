#pragma once

#include "backtest_engine.hpp"
#include <iostream>
#include <string>

namespace wangcai_orderbook_cpp {

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

    std::vector<UserOrder> onMarketData(const MarketData& data) override {
        std::vector<UserOrder> orders;
        
        // 对成交数据的响应
        if (data.event_type == "trade") {
            std::cout << "[" << strategy_id_ << "] 看到成交: " 
                      << " 价格=" << data.last_price / 10000.0 
                      << " 数量=" << data.last_volume 
                      << " 时间=" << data.datetime << std::endl;
            
            // 可以根据成交情况决定是否跟单
            if (data.last_volume >= 500) {  // 大单成交时跟单
                UserOrder order;
                order.order_id = strategy_id_ + "_FOLLOW_TRADE_" + std::to_string(++order_count_);
                order.symbol = data.symbol;
                order.direction = Direction::Buy;  // 简单跟买
                order.order_type = OrderType::Limit;
                order.price = data.last_price - price_offset_;  // 稍低价买入
                order.volume = 50;  // 小量跟单
                order.strategy_id = strategy_id_;
                
                orders.push_back(order);
                
                std::cout << "[" << strategy_id_ << "] 大单跟单: " << order.order_id 
                          << " 价格: " << order.price / 10000.0 << std::endl;
            }
        }
        // 只对订单事件进行跟单（演示同步处理）
        else if (data.event_type == "order") {
            // 检查最佳买卖盘的变化，假设有大单进入
            if (data.best_bid > 0 && data.best_ask > 0) {
                // 简单的跟单逻辑：每N个事件尝试下单一次
                if (++event_count_ % 50 == 0) { // 每50个事件跟单一次
                    UserOrder order;
                    order.order_id = strategy_id_ + "_FOLLOW_" + std::to_string(++order_count_);
                    order.symbol = data.symbol;
                    order.direction = (order_count_ % 2 == 1) ? Direction::Buy : Direction::Sell;
                    order.order_type = OrderType::Limit;
                    
                    if (order.direction == Direction::Buy) {
                        order.price = data.best_bid - price_offset_; // 买单价格稍低
                    } else {
                        order.price = data.best_ask + price_offset_; // 卖单价格稍高
                    }
                    
                    order.volume = follow_volume_;
                    order.strategy_id = strategy_id_;
                    
                    orders.push_back(order);
                    
                    std::cout << "[" << strategy_id_ << "] 跟单: " << order.order_id 
                              << " 方向: " << (order.direction == Direction::Buy ? "买" : "卖")
                              << " 价格: " << order.price / 10000.0 
                              << " 数量: " << order.volume << std::endl;
                }
            }
        }
        
        return orders;
    }

    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        std::cout << "[" << strategy_id_ << "] 订单成交: " << order_id 
                  << " 价格: " << price / 10000.0 
                  << " 数量: " << volume << std::endl;
        total_filled_volume_ += volume;
        
        // 成交后可以追加订单
        // 这里可以根据成交情况决定是否需要下新单
    }
    
    std::vector<UserOrder> onMarketData(const MarketData& data) override {
        std::vector<UserOrder> orders;
        
        // 对成交数据的响应
        if (data.event_type == "trade") {
            std::cout << "[" << strategy_id_ << "] 看到成交: " 
                      << " 价格=" << data.last_price / 10000.0 
                      << " 数量=" << data.last_volume 
                      << " 时间=" << data.datetime << std::endl;
            
            // 可以根据成交情况决定是否跟单
            if (data.last_volume >= 500) {  // 大单成交时跟单
                UserOrder order;
                order.order_id = strategy_id_ + "_FOLLOW_TRADE_" + std::to_string(++order_count_);
                order.symbol = data.symbol;
                order.direction = Direction::Buy;  // 简单跟买
                order.order_type = OrderType::Limit;
                order.price = data.last_price - price_offset_;  // 稍低价买入
                order.volume = 50;  // 小量跟单
                order.strategy_id = strategy_id_;
                
                orders.push_back(order);
                
                std::cout << "[" << strategy_id_ << "] 大单跟单: " << order.order_id 
                          << " 价格: " << order.price / 10000.0 << std::endl;
            }
        }
        // 只对订单事件进行跟单（演示同步处理）
        else if (data.event_type == "order") {
            // 检查最佳买卖盘的变化，假设有大单进入
            if (data.best_bid > 0 && data.best_ask > 0) {
                // 简单的跟单逻辑：每N个事件尝试下单一次
                if (++event_count_ % 50 == 0) { // 每50个事件跟单一次
                    UserOrder order;
                    order.order_id = strategy_id_ + "_FOLLOW_" + std::to_string(++order_count_);
                    order.symbol = data.symbol;
                    order.direction = (order_count_ % 2 == 1) ? Direction::Buy : Direction::Sell;
                    order.order_type = OrderType::Limit;
                    
                    if (order.direction == Direction::Buy) {
                        order.price = data.best_bid - price_offset_; // 买单价格稍低
                    } else {
                        order.price = data.best_ask + price_offset_; // 卖单价格稍高
                    }
                    
                    order.volume = follow_volume_;
                    order.strategy_id = strategy_id_;
                    
                    orders.push_back(order);
                    
                    std::cout << "[" << strategy_id_ << "] 跟单: " << order.order_id 
                              << " 方向: " << (order.direction == Direction::Buy ? "买" : "卖")
                              << " 价格: " << order.price / 10000.0 
                              << " 数量: " << order.volume << std::endl;
                }
            }
        }
        
        return orders;
    }

    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        std::cout << "[" << strategy_id_ << "] 订单撤销: " << order_id 
                  << " 原因: " << reason << std::endl;
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
    
    std::vector<UserOrder> onMarketData(const MarketData& data) override {
        std::vector<UserOrder> orders;
        
        // 简单策略：如果价格偏离前收盘价超过阈值，则反向下单
        if (data.best_bid > 0 && data.best_ask > 0) {
            double mid_price = (data.best_bid + data.best_ask) / 2.0 / 10000.0;
            double prev_close = 5.0; // 简化：假设前收盘价为5元
            
            if (mid_price > prev_close * (1 + threshold_)) {
                // 价格过高，卖出
                UserOrder order;
                order.order_id = strategy_id_ + "_" + std::to_string(++order_count_);
                order.symbol = data.symbol;
                order.direction = Direction::Sell;
                order.order_type = OrderType::Limit;
                order.price = data.best_bid; // 以买一价卖出
                order.volume = 100;
                order.strategy_id = strategy_id_;
                orders.push_back(order);
                
                std::cout << "[策略] " << strategy_id_ << " 卖出信号，价格=" 
                         << mid_price << " > " << prev_close * (1 + threshold_) << std::endl;
            } else if (mid_price < prev_close * (1 - threshold_)) {
                // 价格过低，买入
                UserOrder order;
                order.order_id = strategy_id_ + "_" + std::to_string(++order_count_);
                order.symbol = data.symbol;
                order.direction = Direction::Buy;
                order.order_type = OrderType::Limit;
                order.price = data.best_ask; // 以卖一价买入
                order.volume = 100;
                order.strategy_id = strategy_id_;
                orders.push_back(order);
                
                std::cout << "[策略] " << strategy_id_ << " 买入信号，价格=" 
                         << mid_price << " < " << prev_close * (1 - threshold_) << std::endl;
            }
        }
        
        return orders;
    }
    
    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        std::cout << "[策略] " << strategy_id_ << " 订单成交: " << order_id 
                 << " 价格=" << price / 10000.0 << " 数量=" << volume << std::endl;
    }
    
    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        std::cout << "[策略] " << strategy_id_ << " 订单取消: " << order_id 
                 << " 原因=" << reason << std::endl;
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

} // namespace wangcai_orderbook_cpp