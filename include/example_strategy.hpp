#pragma once

#include "backtest_engine.hpp"
#include <iostream>
#include <string>

namespace wangcai_orderbook_cpp {

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