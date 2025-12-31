// === include/con_auction_engine.hpp ===
/*
 * @brief : 09:30 连续竞价引擎
 */
#pragma once
#include "orderbook.h"
#include <functional>
#include <unordered_map>
#include <list>
#include <map>
#include <vector>
#include <algorithm>
#include "OrderLoader.h"

namespace wangcai {

// 市场类型枚举
enum class MarketType {
    SH,  // 上海市场
    SZ   // 深圳市场
};

// 虚拟订单结构
struct VirtualOrder {
    std::shared_ptr<Order> order;       // 订单本体
    uint64_t trigger_order_id;          // 标签：等这个历史订单被移除后我才可成交
    bool is_tradeable;                  // 是否可成交
    
    VirtualOrder(std::shared_ptr<Order> od, uint64_t trigger_id, bool tradeable)
        : order(od), trigger_order_id(trigger_id), is_tradeable(tradeable) {}
};

// 虚拟订单位置（用于O(1)撤单）
struct VirtualOrderLoc {
    Price price;
    bool is_buy;
    std::list<VirtualOrder>::iterator it;
};

class ConAuctionEngine {
public:
        using CancelCallback = std::function<void(uint64_t order_id, bool success, const std::string& reason, 
                                            std::shared_ptr<Order> order_info)>;
    
    explicit ConAuctionEngine(OrderBook& ob, MarketType market_type, CancelCallback cancel_cb = nullptr)
        : ob_(ob), market_type_(market_type), on_cancel_(cancel_cb), price_cage_enabled_(false) {}
    
    void accept(std::shared_ptr<Order>);
    bool cancel(uint64_t oid);
    bool cancel_by_input_id(uint64_t input_id);  // 通过输入订单ID撤单
    
    // === 价格笼子功能（2023年3月前规则）===
    void enablePriceCage(bool enable) { price_cage_enabled_ = enable; }
    bool isPriceCageEnabled() const { return price_cage_enabled_; }
    // 根据订单时间自动检测并启用价格笼子（2023年3月1日前自动启用）
    void checkAndEnablePriceCage(const std::string& datetime);
    
    // 获取休眠订单数量（用于调试）
    size_t getDormantBuyCount() const { return dormant_buy_orders_.size(); }
    size_t getDormantSellCount() const { return dormant_sell_orders_.size(); }
    
    // 获取虚拟订单数量（用于调试）
    size_t getVirtualBuyCount() const { 
        size_t count = 0;
        for (const auto& [price, vlist] : virtual_buy_) count += vlist.size();
        return count;
    }
    size_t getVirtualSellCount() const {
        size_t count = 0;
        for (const auto& [price, vlist] : virtual_sell_) count += vlist.size();
        return count;
    }

private:
    void match(std::shared_ptr<Order>&);
    void match_sh(std::shared_ptr<Order>&);  // 上海市场撮合逻辑（只处理历史订单）
    void match_sz(std::shared_ptr<Order>&);  // 深圳市场撮合逻辑（只处理历史订单）
    void accept_sh(std::shared_ptr<Order> od);  // 上海市场订单处理
    void accept_sz(std::shared_ptr<Order> od);  // 深圳市场订单处理
    
    // === 虚拟订单处理函数 ===
    // 虚拟订单入队
    void accept_virtual_order(std::shared_ptr<Order> od);
    // 虚拟订单成交（counterparty_id是对手方历史订单ID）
    void virtual_fill(std::shared_ptr<Order>& od, Price fill_price, uint64_t counterparty_id);
    // 历史订单被移除时（成交或撤单），更新虚拟订单状态
    void on_historical_order_removing(std::shared_ptr<Order>& removing_order, int bucket_idx, bool is_buy, bool is_filled);
    // 历史订单处理完后，检查虚拟订单成交
    void try_match_virtual_orders(std::shared_ptr<Order>& inc);
    // 撤销虚拟订单（O(1)复杂度）
    bool cancel_virtual_order(uint64_t order_id);
    
    // === 价格笼子相关函数 ===
    // 获取买单基准价：即时最低卖一价 → 最近成交价 → 昨收价
    Price getBuyBenchmark() const;
    // 获取卖单基准价：即时最高买一价 → 最近成交价 → 昨收价
    Price getSellBenchmark() const;
    // 检查订单是否在价格笼子范围内
    bool isInPriceCage(const std::shared_ptr<Order>& od) const;
    // 计算价格笼子上限（买单）：基准价 + max(基准价×2%, 0.1元)，向上取整到tick
    Price calcCageCeiling(Price benchmark) const;
    // 计算价格笼子下限（卖单）：基准价 - max(基准价×2%, 0.1元)，向下取整到tick
    Price calcCageFloor(Price benchmark) const;
    // 添加到休眠队列
    void addToDormant(std::shared_ptr<Order> od);
    // 尝试激活休眠订单
    // void tryActivateDormantOrders();  // 价格笼子功能已禁用
    
    OrderBook& ob_;
    MarketType market_type_;  // 市场类型
    CancelCallback on_cancel_;
    
    // === 价格笼子数据 ===
    bool price_cage_enabled_;  // 是否启用价格笼子（2023年3月前为true）
    std::list<std::shared_ptr<Order>> dormant_buy_orders_;   // 休眠买单队列（时间顺序）
    std::list<std::shared_ptr<Order>> dormant_sell_orders_;  // 休眠卖单队列（时间顺序）
    
    // === 虚拟订单簿数据 ===
    std::map<Price, std::list<VirtualOrder>> virtual_buy_;   // 虚拟买单（按价格分组）
    std::map<Price, std::list<VirtualOrder>> virtual_sell_;  // 虚拟卖单（按价格分组）
    // 标签反向映射：trigger_order_id -> 虚拟订单指针列表（性能优化）
    std::unordered_map<uint64_t, std::vector<VirtualOrder*>> trigger_map_;
    // 虚拟订单位置映射：order_id -> 位置信息（O(1)撤单）
    std::unordered_map<uint64_t, VirtualOrderLoc> virtual_loc_;
};

} // namespace wangcai 