#pragma once
#include "backtest_engine.hpp"
#include <iostream>
#include <iomanip>
#include <deque>

namespace wangcai {

// 全接口演示策略：展示所有回调接口的使用方法
class DemoStrategy : public Strategy {
public:
    DemoStrategy(const std::string& strategy_id = "DEMO_STRATEGY")
        : strategy_id_(strategy_id), phase_(0), order_counter_(0) {
        std::cout << "\n[" << strategy_id_ << "] 全接口演示策略已启动" << std::endl;
        std::cout << "将依次演示：下单回调 → 成交回调 → 撤单回调 → 持仓管理" << std::endl;
    }

    // 【接口1】订单事件处理
    std::vector<UserEvent> onOrderEvent(const Event& event) override {
        std::vector<UserEvent> events;
        
        // 只在连续竞价阶段进行演示
        if (event.datetime.substr(11, 8) < "09:30:00" || event.datetime.substr(11, 8) > "14:55:00") {
            return events;
        }
        
        // 根据阶段执行不同的演示
        switch (phase_) {
            case 0:
                events = demoOrderPlacement(event);
                break;
            case 1:
                events = demoPendingOrders(event);
                break;
            case 2:
                events = demoCancellation(event);
                break;
            default:
                // 演示完成，不再下单
                break;
        }
        
        return events;
    }

    // 【接口2】成交事件处理
    std::vector<UserEvent> onTradeEvent(const Execution& execution, const std::string& datetime) override {
        trade_event_count_++;
        if (trade_event_count_ <= 5) {  // 只打印前5个
            std::cout << "[" << strategy_id_ << "] 收到成交事件 #" << trade_event_count_ << ": "
                      << "价格=" << (execution.price / 10000.0)
                      << " 数量=" << execution.volume 
                      << " 时间=" << datetime << std::endl;
        }
        
        // 在这里可以根据市场成交情况做出策略决策
        return {};
    }

    // 【接口3】行情快照处理  
    int tick_count = 0;
    std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) override {
        if (tick_count < 5) {
        std::cout << "[" << strategy_id_ << "] 收到行情快照: "
                  << "最新价=" << (snapshot.last_price / 10000.0)
                  << " 买一=" << (snapshot.bids[0] / 10000.0)
                  << " 卖一=" << (snapshot.asks[0] / 10000.0)
                  << " 时间=" << snapshot.datetime << std::endl;
        }
        tick_count++;    
        // 在这里可以根据行情变化做出策略决策
        return {};
    }

    // 【接口4】统一交易回调（新接口）
    void onTradeCallback(const TradeCallback& callback) override {
        std::cout << "\n[" << strategy_id_ << "] ==== 统一交易回调 ====" << std::endl;
        std::cout << "   订单ID: " << callback.localid << std::endl;
        std::cout << "   回调类型: " << (callback.matchtype == 'T' ? "成交" : "撤单") << std::endl;
        std::cout << "   方向: " << (callback.direction == 'B' ? "买入" : "卖出") << std::endl;
        std::cout << "   数量: " << callback.volume << std::endl;
        std::cout << "   价格: " << std::fixed << std::setprecision(4) << (callback.price / 10000.0) << " 元" << std::endl;
        if (callback.matchtype == 'T') {
            std::cout << "   成交金额: " << std::fixed << std::setprecision(2) << callback.matchamount << " 元" << std::endl;
        }
        std::cout << "   当前总持仓: " << callback.deltapos << std::endl;
        std::cout << "   时间: " << callback.matchtime << std::endl;
        
        // 显示当前所有持仓
        auto positions = getAllPositions();
        if (!positions.empty()) {
            std::cout << "   持仓明细: ";
            for (const auto& pos : positions) {
                std::cout << pos.first << "=" << pos.second << " ";
            }
            std::cout << std::endl;
        }
        std::cout << "================================" << std::endl;
        
        // 统计
        if (callback.matchtype == 'T') {
            filled_count_++;
            total_filled_volume_ += callback.volume;
        } else {
            cancelled_count_++;
        }
    }

    // 【接口5】下单回调（新接口）
    void onOrderCallback(const OrderCallback& callback) override {
        std::cout << "\n[" << strategy_id_ << "] ==== 下单回调 ====" << std::endl;
        std::cout << "   订单ID: " << callback.orderlocalid << std::endl;
        std::cout << "   方向: " << (callback.direction == 1 ? "买入" : "卖出") << std::endl;
        std::cout << "   数量: " << callback.volume << std::endl;
        std::cout << "   价格: " << std::fixed << std::setprecision(4) << (callback.price / 10000.0) << " 元" << std::endl;
        std::cout << "   交易所: " << (callback.exchange == 0 ? "上海" : "深圳") << std::endl;
        std::cout << "   买一: " << std::fixed << std::setprecision(4) << (callback.bid1 / 10000.0) << " 元" << std::endl;
        std::cout << "   卖一: " << std::fixed << std::setprecision(4) << (callback.ask1 / 10000.0) << " 元" << std::endl;
        std::cout << "   当前总持仓: " << callback.deltapos << std::endl;
        std::cout << "   时间: " << callback.time << std::endl;
        std::cout << "==============================" << std::endl;
    }

    // 【接口6】成交回调（旧接口，保持兼容）
    void onOrderFilled(const std::string& order_id, Price price, Quantity volume) override {
        std::cout << "[" << strategy_id_ << "] 旧接口-成交回调: " << order_id 
                  << " 价格=" << (price / 10000.0) << " 数量=" << volume << std::endl;
    }

    // 【接口7】撤单回调（旧接口，保持兼容）
    void onOrderCancelled(const std::string& order_id, const std::string& reason) override {
        std::cout << "[" << strategy_id_ << "] 旧接口-撤单回调: " << order_id 
                  << " 原因=" << reason << std::endl;
    }

    std::string getStrategyId() const override { return strategy_id_; }

    void printStatistics() const {
        std::cout << "\n============ 演示策略统计报告 ============" << std::endl;
        std::cout << "策略ID: " << strategy_id_ << std::endl;
        std::cout << "演示阶段: " << (phase_ >= 3 ? "全部完成" : "进行中") << std::endl;
        
        std::cout << "\n=== 接口测试统计 ===" << std::endl;
        std::cout << "总下单数: " << order_counter_ << " (目标: 10个)" << std::endl;
        std::cout << "累计成交次数: " << filled_count_ << " (目标: 5个立即成交)" << std::endl;
        std::cout << "累计成交量: " << total_filled_volume_ << std::endl;
        std::cout << "累计撤单次数: " << cancelled_count_ << " (目标: 5个撤单)" << std::endl;
        std::cout << "收到成交或者撤单事件数: " << trade_event_count_ << " (显示前5个)" << std::endl;
        
        std::cout << "\n=== 演示阶段完成情况 ===" << std::endl;
        std::cout << "   阶段0-下单回调: " << (phase_ >= 1 ? "完成(5个订单)" : "进行中") << std::endl;
        std::cout << "   阶段1-挂单演示: " << (phase_ >= 2 ? "完成(5个挂单)" : (phase_ >= 1 ? "进行中" : "等待")) << std::endl;
        std::cout << "   阶段2-撤单回调: " << (phase_ >= 3 ? "完成(5个撤单)" : (phase_ >= 2 ? "进行中" : "等待")) << std::endl;
        
        // 持仓汇总
        auto positions = getAllPositions();
        if (!positions.empty()) {
            std::cout << "\n=== 持仓汇总 ===" << std::endl;
            int64_t total_net_position = 0;
            for (const auto& pos : positions) {
                std::cout << "   " << pos.first << ": " << pos.second << std::endl;
                total_net_position += pos.second;
            }
            std::cout << "   总净持仓: " << total_net_position << std::endl;
        } else {
            std::cout << "\n=== 持仓汇总 ===" << std::endl;
            std::cout << "   暂无持仓" << std::endl;
        }
        
        std::cout << "\n=== 接口演示总结 ===" << std::endl;
        std::cout << "   onOrderEvent() - 订单事件处理" << std::endl;
        std::cout << "   onTradeEvent() - 成交事件处理(显示前5个)" << std::endl;
        std::cout << "   onTickEvent() - 行情快照处理" << std::endl;
        std::cout << "   onTradeCallback() - 统一交易回调(新接口)" << std::endl;
        std::cout << "   onOrderCallback() - 下单回调(新接口)" << std::endl;
        std::cout << "   onOrderFilled() - 成交回调(旧接口兼容)" << std::endl;
        std::cout << "   onOrderCancelled() - 撤单回调(旧接口兼容)" << std::endl;
        std::cout << "========================================" << std::endl;
    }

private:
    // 阶段0：演示下单回调和立即成交（5个例子）
    std::vector<UserEvent> demoOrderPlacement(const Event& event) {
        static int demo_count = 0;
        static int event_count = 0;
        std::vector<UserEvent> events;
        
        event_count++;
        
        if (demo_count == 0 && event_count == 1) {
            std::cout << "\n=== 阶段0：演示下单回调和成交回调（5个订单）===" << std::endl;
        }
        
        if (demo_count < 5 && event_count % 3 == 1) {  // 每3个事件下一单
            demo_count++;
            
            // 交替下买单和卖单
            Direction direction = (demo_count % 2 == 1) ? Direction::Buy : Direction::Sell;
            std::string direction_str = (direction == Direction::Buy) ? "买" : "卖";
            std::string order_id = "DEMO_" + direction_str + "_" + std::to_string(demo_count);
            
            UserOrder order;
            order.order_id = order_id;
            order.symbol = event.sym;
            order.direction = direction;
            order.order_type = OrderType::Market;  // 市价单确保成交
            order.price = (direction == Direction::Buy) ? event.price + 1000 : event.price - 1000;
            order.volume = 100 + demo_count * 10;  // 数量递增
            order.strategy_id = strategy_id_;
            
            events.emplace_back(order);
            
            std::cout << "[" << demo_count << "/5] 提交" << direction_str << "单: " << order_id 
                      << " 数量=" << order.volume << " (预期立即成交)" << std::endl;
            
            if (demo_count == 5) {
                phase_ = 1; // 进入下一阶段
                std::cout << "阶段0完成：已演示5个下单回调和成交回调" << std::endl;
            }
        }
        
        return events;
    }

    // 阶段1：演示挂单回调（5个挂单）
    std::vector<UserEvent> demoPendingOrders(const Event& event) {
        static int demo_count = 0;
        static int event_count = 0;
        std::vector<UserEvent> events;
        
        event_count++;
        
        if (demo_count == 0 && event_count == 1) {
            std::cout << "\n=== 阶段1：演示挂单（5个待撤单）===" << std::endl;
        }
        
        if (demo_count < 5 && event_count % 2 == 1) {  // 每2个事件下一单
            demo_count++;
            
            // 交替下买单和卖单，但远离市价不会成交
            Direction direction = (demo_count % 2 == 1) ? Direction::Buy : Direction::Sell;
            std::string direction_str = (direction == Direction::Buy) ? "买" : "卖";
            std::string order_id = "PENDING_" + direction_str + "_" + std::to_string(demo_count);
            
            UserOrder order;
            order.order_id = order_id;
            order.symbol = event.sym;
            order.direction = direction;
            order.order_type = OrderType::Limit;
            // 故意设置远离市价，不会成交
            order.price = (direction == Direction::Buy) ? event.price - 1000 : event.price + 1000;
            order.volume = 200 + demo_count * 20;
            order.strategy_id = strategy_id_;
            
            events.emplace_back(order);
            pending_orders_.push_back(order_id);  // 记录待撤单
            
            std::cout << "[" << demo_count << "/5] 提交限价" << direction_str << "单: " << order_id 
                      << " 数量=" << order.volume << " (挂单等待)" << std::endl;
            
            if (demo_count == 5) {
                phase_ = 2; // 进入撤单阶段
                std::cout << "阶段1完成：已演示5个挂单，准备撤单演示" << std::endl;
            }
        }
        
        return events;
    }

    // 阶段2：演示撤单回调（5个撤单）
    std::vector<UserEvent> demoCancellation(const Event& event) {
        static int demo_count = 0;
        static int event_count = 0;
        std::vector<UserEvent> events;
        
        event_count++;
        
        if (demo_count == 0 && event_count == 1) {
            std::cout << "\n=== 阶段2：演示撤单回调（5个撤单）===" << std::endl;
        }
        
        if (demo_count < 5 && !pending_orders_.empty() && event_count % 2 == 1) {
            demo_count++;
            
            // 撤销之前的挂单
            std::string cancel_id = pending_orders_.front();
            pending_orders_.pop_front();
            
            UserCancel cancel(cancel_id, strategy_id_);
            events.emplace_back(cancel);
            
            std::cout << "[" << demo_count << "/5] 提交撤单: " << cancel_id << " (演示撤单回调)" << std::endl;
            
            if (demo_count == 5) {
                phase_ = 3; // 演示完成
                std::cout << "阶段2完成：已演示5个撤单回调" << std::endl;
                std::cout << "\n=== 所有接口演示完成！===" << std::endl;
            }
        }
        
        return events;
    }

    std::string strategy_id_;
    int phase_;                           // 演示阶段
    int order_counter_;                   // 订单计数器
    int filled_count_ = 0;               // 成交次数
    int cancelled_count_ = 0;            // 撤单次数  
    int64_t total_filled_volume_ = 0;    // 总成交量
    int trade_event_count_ = 0;          // 成交事件计数器
    std::deque<std::string> pending_orders_; // 待撤销订单
};

/*
// 以下策略已注释掉，仅保留演示策略

// 同步跟单策略示例 - 演示严格同步的事件处理
class SyncFollowStrategy : public Strategy {
    // ... 原有代码注释掉 ...
};

// 均值回归策略示例 - 演示基于价格偏离的交易逻辑  
class MeanReversionStrategy : public Strategy {
    // ... 原有代码注释掉 ...
};

// 测试回调策略示例 - 专门测试各种回调场景
class TestCallbackStrategy : public Strategy {
    // ... 原有代码注释掉 ...
};
*/

} // namespace wangcai
