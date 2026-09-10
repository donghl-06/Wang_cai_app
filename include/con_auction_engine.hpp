// === include/con_auction_engine.hpp ===
/*
 * @brief : 09:30 连续竞价引擎
 */
#pragma once
#include "orderbook.h"
#include "types.h"
#include "price_cage.h"
#include <functional>
#include <unordered_map>
#include <unordered_set>
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

// === 真实成交替代模式：带队列位置的虚拟订单 ===
struct RtVirtualOrder {
    std::shared_ptr<Order> order;       // 订单本体
    uint64_t queue_position;            // 下单时同侧同价位历史订单总量（排在前面的量）
    uint64_t pool_at_entry{0};          // 下单时真实成交池的累积值（只算下单之后的增量）
    uint64_t frontier_hist_order_id{0}; // 下单瞬间"队首边界"历史单ID（仅<=该ID的撤单可推进队列）
    Quantity filled_volume{0};          // 已成交量（支持部分成交）
    
    RtVirtualOrder(std::shared_ptr<Order> od, uint64_t qpos, uint64_t pool_entry, uint64_t frontier_id)
        : order(od), queue_position(qpos), pool_at_entry(pool_entry), frontier_hist_order_id(frontier_id) {}
};

// 真实成交替代模式：虚拟订单位置（用于O(1)撤单）
struct RtVirtualOrderLoc {
    Price price;
    bool is_buy;
    std::list<RtVirtualOrder>::iterator it;
};

class ConAuctionEngine {
public:
        using CancelCallback = std::function<void(uint64_t order_id, bool success, const std::string& reason, 
                                            std::shared_ptr<Order> order_info)>;
    
    explicit ConAuctionEngine(OrderBook& ob, MarketType market_type, CancelCallback cancel_cb = nullptr)
        : ob_(ob), market_type_(market_type), on_cancel_(cancel_cb), price_cage_enabled_(false) {}
    
    void accept(std::shared_ptr<Order>);
    bool cancel(uint64_t oid);
    bool cancel_by_input_id(uint64_t input_id, int channel_no = -1);  // 通过市场复合键撤单
    
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
    
    // === 严格主动单模式（欠债限制功能）===
    // 开启后，虚拟主动单吃掉的历史订单需要被真实市场消耗后才能下新的主动单
    void setStrictActiveOrderMode(bool enabled) { strict_active_order_mode_ = enabled; }
    bool isStrictActiveOrderMode() const { return strict_active_order_mode_; }
    // 检查是否有未还清的欠债（虚拟消耗量尚未被真实市场补回）
    bool hasDebt() const { return debt_volume_ > 0; }
    // 获取欠债量（用于调试）
    uint64_t getDebtVolume() const { return debt_volume_; }
    
    // === 真实成交替代模式 ===
    // 开启后：主动单复用严格模式，被动单使用"队列位置+真实成交池"匹配
    void setRealTradeMatchMode(bool enabled) { 
        real_trade_match_mode_ = enabled;
        if (enabled) strict_active_order_mode_ = true;  // 自动启用严格主动单模式
    }
    bool isRealTradeMatchMode() const { return real_trade_match_mode_; }
    // 喂入真实成交事件（由 BacktestEngine 在处理 cstra ExecType='1' 时调用）
    // buy_side_passive: true=买方被动被消耗，false=卖方被动被消耗
    void feedRealTrade(Price price, Quantity volume, bool buy_side_passive);
    // === 用户下单队列信息（可选）===
    struct QueueInfo {
        int64_t ahead_count{-1};         // 前方历史订单数量
        int64_t ahead_volume{-1};        // 前方历史订单总量
        std::vector<uint64_t> prev_order_ids; // 最近3个前序历史订单ID（从近到远）
    };
    void setQueueInfoEnabled(bool enabled) { queue_info_enabled_ = enabled; }
    bool isQueueInfoEnabled() const { return queue_info_enabled_; }
    QueueInfo getQueueInfoForUserOrder(Price price, bool is_buy) const;
    // 获取RT虚拟订单数量（用于调试）
    size_t getRtVirtualBuyCount() const {
        size_t count = 0;
        for (const auto& [price, vlist] : rt_virtual_buy_) count += vlist.size();
        return count;
    }
    size_t getRtVirtualSellCount() const {
        size_t count = 0;
        for (const auto& [price, vlist] : rt_virtual_sell_) count += vlist.size();
        return count;
    }

    // === 价格笼子：历史单暂存（消息流反推法，行情可见性重建） ===
    // 仅"深市创业板暂存窗口(2020.8-2023.4)"由 BacktestEngine 激活。
    // 笼中单不进主订单簿（fillDepth 十档天然不可见），可被撤单，可出笼恢复。
    void suspendHistoricalOrder(std::shared_ptr<Order> od);       // 入笼
    // 出笼扫描（数值判据：价格落回有效申报范围，按入笼序恢复）
    void activateEligibleSuspendedHistorical(const PriceCageRule& rule);
    std::vector<std::shared_ptr<Order>> takeAllSuspendedHistorical(); // 14:57 收盘竞价开始时全部恢复
    size_t getSuspendedHistoricalCount() const { return suspended_hist_.size(); }

    // === 价格笼子：策略单数值判定 ===
    struct CageBounds { Price lo; Price hi; };                     // 有效申报范围（含边界）
    // 计算策略单的有效申报价格范围：基准价链（对手一档→本方一档→最新成交→昨收）
    // + max(基准×幅度%, 兜底额)，上限向上/下限向下取整到 tick，并夹在涨跌停内
    static CageBounds userCageBounds(bool is_buy, const PriceCageRule& rule,
                                     const OrderBook& ob);
    void suspendUserOrder(std::shared_ptr<Order> od);             // 暂存时代：策略单入笼
    void activateEligibleUserCageOrders(const PriceCageRule& rule); // 出笼扫描（数值范围重查）
    std::vector<std::shared_ptr<Order>> takeAllUserCageOrders();  // 收盘竞价恢复
    size_t getUserCageCount() const { return user_cage_.size(); }
    // 策略单废单（涨跌停/价格笼子拒单）：Rejected + on_cancel_ 回调链路
    void rejectUserOrder(std::shared_ptr<Order> od, const std::string& reason);

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
    
    // === 严格主动单模式（欠债限制）===
    bool strict_active_order_mode_ = false;  // 开关，默认关闭
    // 虚拟主动单消耗的总量中，尚未被真实市场补回的部分
    uint64_t debt_volume_ = 0;
    
    // === 真实成交替代模式 ===
    bool real_trade_match_mode_ = false;  // 开关，默认关闭
    // RT模式虚拟订单入队（被动单专用）
    void accept_rt_virtual_order(std::shared_ptr<Order> od);
    // RT模式尝试成交虚拟订单（真实成交事件触发）
    void try_fill_rt_virtual_orders(Price price, bool is_buy_side);
    // RT模式虚拟订单成交（支持部分成交）
    void rt_virtual_fill(std::shared_ptr<Order>& od, Price fill_price, Quantity fill_qty);
    // RT模式撤销虚拟订单
    bool cancel_rt_virtual_order(uint64_t order_id);
    // RT模式：历史订单撤单时调整队列位置
    void on_historical_order_cancel_rt(int bucket_idx, bool is_buy, uint64_t cancel_hist_order_id, Quantity cancel_vol);
    
    // RT模式虚拟订单簿（按价格分组，FIFO）
    std::map<Price, std::list<RtVirtualOrder>> rt_virtual_buy_;
    std::map<Price, std::list<RtVirtualOrder>> rt_virtual_sell_;
    // RT虚拟订单位置映射（O(1)撤单）
    std::unordered_map<uint64_t, RtVirtualOrderLoc> rt_virtual_loc_;
    // 真实成交池：每个价位每个方向的累积真实成交量
    std::unordered_map<Price, uint64_t> rt_pool_buy_;   // 买侧被消耗的累积量
    std::unordered_map<Price, uint64_t> rt_pool_sell_;  // 卖侧被消耗的累积量
    // 用户下单回调队列信息开关（默认关闭）
    bool queue_info_enabled_ = false;

    // === 价格笼子容器 ===
    // 历史/策略笼单：system_id -> order。system_id 创建序 = 事件流序（=市场委托序），
    // map 有序迭代即"按原始 orderid 升序恢复"（aqsnapshots 语义）
    std::map<uint64_t, std::shared_ptr<Order>> suspended_hist_;
    std::map<uint64_t, std::shared_ptr<Order>> user_cage_;

public:
    // === 诊断计数器（仅用于日志，不影响业务逻辑）===
    struct DiagCounters {
        uint64_t passive_order_count = 0;
        uint64_t active_order_count = 0;
        uint64_t passive_fill_count = 0;
        uint64_t active_fill_count = 0;
        uint64_t active_fill_volume = 0;
        uint64_t debt_reject_count = 0;
        uint64_t debt_create_total = 0;
        uint64_t debt_clear_total = 0;
        uint64_t rt_feed_count = 0;
        uint64_t rt_feed_volume = 0;
        uint64_t rt_cancel_count = 0;
    } diag_;
};

} // namespace wangcai
