#pragma once
#include "backtest_engine.hpp"
#include <iostream>
#include <string>

namespace wangcai_orderbook_cpp {

/**
 * 同步策略示例
 * 实现严格同步的事件处理，确保策略订单总是排在当前事件之后
 */
class SyncStrategy : public Strategy {
public:
    SyncStrategy(const std::string& strategy_id = "SYNC_STRATEGY", 
                 int64_t min_order_size = 1000,
                 int64_t strategy_volume = 100,
                 int64_t price_offset = 100)
        : strategy_id_(strategy_id), 
          min_order_size_(min_order_size),
          strategy_volume_(strategy_volume),
          price_offset_(price_offset),
          order_count_(0) {}

    // 接收原始事件（订单/撤单）
    StrategyResponse onRawEvent(const Event& event) override {
        StrategyResponse response;
        
        std::cout << "[策略] 收到事件: " 
                 << (event.source == "ord" ? "订单" : "撤单")
                 << " ID=" << (event.source == "ord" ? event.orderid : 
                               (event.bidorderid ? event.bidorderid : event.askorderid))
                 << " 价格=" << event.price / 10000.0
                 << " 数量=" << event.size 
                 << " 时间=" << event.datetime.substr(11, 8) << std::endl;
        
        // 决定是否跟单（只对大单进行跟单）
        if (event.source == "ord" && event.size >= min_order_size_) {
            UserOrder order;
            order.order_id = strategy_id_ + "_" + std::to_string(++order_count_);
            order.symbol = event.sym;
            // 反向下单策略：看到买单就下卖单，看到卖单就下买单
            order.direction = (event.side == 1) ? Direction::Sell : Direction::Buy;
            order.order_type = OrderType::Limit;
            // 价格稍微偏移，增加成交概率
            order.price = event.price + (order.direction == Direction::Buy ? -price_offset_ : price_offset_);
            order.volume = strategy_volume_;
            order.strategy_id = strategy_id_;
            
            response.type = StrategyResponse::PlaceOrder;
            response.orders.push_back(order);
            
            std::cout << "[策略] 决定跟单: " << order.order_id 
                     << " 方向=" << (order.direction == Direction::Buy ? "买" : "卖")
                     << " 价格=" << order.price / 10000.0
                     << " 数量=" << order.volume << std::endl;
        }
        
        return response;
    }
    
    // 接收成交事件（单笔）
    StrategyResponse onExecution(const Execution& ex, const std::string& datetime) override {
        StrategyResponse response;
        
        std::cout << "[策略] 收到成交: "
                 << " 买方=" << ex.buy_order_id
                 << " 卖方=" << ex.sell_order_id  
                 << " 价格=" << ex.price / 10000.0
                 << " 数量=" << ex.volume
                 << " 时间=" << datetime.substr(11, 8) << std::endl;
        
        // 记录成交信息
        total_executions_++;
        total_volume_ += ex.volume;
        
        // 简单的成交后策略：如果成交量很大，可以考虑追加订单
        if (ex.volume >= min_order_size_ * 2) {
            std::cout << "[策略] 发现大成交，可考虑追加订单" << std::endl;
            // 这里可以添加追加订单的逻辑
        }
        
        return response;
    }
    
    std::string getStrategyId() const override { return strategy_id_; }
    
    // 获取策略统计信息
    void printStatistics() const {
        std::cout << "\n========== 策略统计信息 ==========" << std::endl;
        std::cout << "策略ID: " << strategy_id_ << std::endl;
        std::cout << "下单次数: " << order_count_ << std::endl;
        std::cout << "观察到的成交笔数: " << total_executions_ << std::endl;
        std::cout << "观察到的总成交量: " << total_volume_ << std::endl;
        std::cout << "=================================" << std::endl;
    }
    
private:
    std::string strategy_id_;
    int64_t min_order_size_;      // 最小触发订单大小
    int64_t strategy_volume_;     // 策略下单量
    int64_t price_offset_;        // 价格偏移
    int order_count_;             // 下单计数
    int total_executions_ = 0;    // 观察到的成交笔数
    int64_t total_volume_ = 0;    // 观察到的总成交量
};

/**
 * 趋势跟踪策略示例
 * 根据价格变动趋势进行下单
 */
class TrendFollowingStrategy : public Strategy {
public:
    TrendFollowingStrategy(const std::string& strategy_id = "TREND_STRATEGY")
        : strategy_id_(strategy_id), last_price_(0), order_count_(0) {}

    StrategyResponse onRawEvent(const Event& event) override {
        StrategyResponse response;
        
        if (event.source == "ord" && last_price_ > 0) {
            // 计算价格变动
            double price_change_rate = (static_cast<double>(event.price) - last_price_) / last_price_;
            
            // 如果价格变动超过阈值，跟随趋势下单
            if (std::abs(price_change_rate) > 0.005) { // 0.5%的变动阈值
                UserOrder order;
                order.order_id = strategy_id_ + "_" + std::to_string(++order_count_);
                order.symbol = event.sym;
                // 趋势跟随：价格上涨就买入，价格下跌就卖出
                order.direction = (price_change_rate > 0) ? Direction::Buy : Direction::Sell;
                order.order_type = OrderType::Limit;
                order.price = event.price + (order.direction == Direction::Buy ? 50 : -50);
                order.volume = 50;
                order.strategy_id = strategy_id_;
                
                response.type = StrategyResponse::PlaceOrder;
                response.orders.push_back(order);
                
                std::cout << "[趋势策略] 价格变动 " << (price_change_rate * 100) << "%, 下单: " 
                         << order.order_id << " " << (order.direction == Direction::Buy ? "买" : "卖") << std::endl;
            }
        }
        
        if (event.source == "ord") {
            last_price_ = static_cast<double>(event.price);
        }
        
        return response;
    }
    
    StrategyResponse onExecution(const Execution& ex, const std::string& datetime) override {
        // 趋势策略对成交事件的简单处理
        return StrategyResponse(); // 继续
    }
    
    std::string getStrategyId() const override { return strategy_id_; }
    
private:
    std::string strategy_id_;
    double last_price_;
    int order_count_;
};

} // namespace wangcai_orderbook_cpp
