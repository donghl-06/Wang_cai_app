/*
 * @Author: linzhuoyu
 * @Date  : 2025-07-07 03:11:27
 * @Update: 2025-07-08 14:35
 * @File  : /wangcai_orderbook_cpp/include/order.h
 * @brief : 撮合引擎订单 / 档位 / 成交 等核心实体
 */
#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <list>
#include <memory>
#include <chrono>

#include "types.h"   // Price / Quantity / generate_order_id()

namespace wangcai_orderbook_cpp {

//枚举类型

enum class OrderStatus : uint8_t {
    Submitted,      ///< 已提交
    PartFilled,     ///< 部分成交
    Filled,         ///< 全部成交
    Cancelled,      ///< 已撤销
    Rejected        ///< 拒绝
};

enum class OrderType : uint8_t {
    Limit,              ///< 限价
    Market,             ///< 市价
    BestCounterpart,    ///< 对手价
    BestOwn             ///< 本方价
};

enum class Direction : uint8_t { Buy, Sell };

//基础类型
using Timestamp = std::chrono::time_point<
                     std::chrono::system_clock,
                     std::chrono::nanoseconds>;

struct PriceLevel;     // 前置声明

//订单对象
struct Order {
    //业务字段
    std::string broker;             ///< 券商
    std::string account;            ///< 账户
    std::string exchange;           ///< 交易所
    std::string instrument;         ///< 代码
    std::string order_local_id;     ///< 本地编号
    uint64_t    order_id{};         ///< 系统 ID
    int64_t    bizindex{};         ///< 业务编号

    OrderType   order_type{OrderType::Limit}; // 订单类型
    Direction   direction{Direction::Buy}; // 订单方向
    Price       price{}; // 订单价格
    Quantity    volume{}; // 订单数量
    Quantity    traded_volume{}; // 已成交数量
    OrderStatus status{OrderStatus::Submitted}; // 订单状态
    Timestamp   timestamp{std::chrono::system_clock::now()}; // 订单时间

    std::string user_defined_string; // 用户定义字符串
    std::string algo_type; // 算法类型
    std::string algo_param; // 算法参数

    //链表 & 档位关联（内部使用）
    std::list<std::shared_ptr<Order>>::iterator level_iter; // 链表迭代器
    PriceLevel* price_level{nullptr}; // 价格档位

    //构造函数
    Order() = default;   // 供 object_pool 默认构造

    Order(std::string_view broker_,  // 券商
          std::string_view account_, // 账户
          std::string_view exchange_, // 交易所
          std::string_view instrument_, // 代码
          std::string_view order_local_id_, // 本地编号
          OrderType   type_, // 订单类型
          Direction   dir_, // 订单方向
          Price       px_, // 订单价格
          Quantity    vol_, // 订单数量
          uint64_t    bizindex_) // 业务编号
        : broker(broker_), account(account_), exchange(exchange_),
          instrument(instrument_), order_local_id(order_local_id_),
          order_id(generate_order_id()), 
          order_type(type_), direction(dir_), price(px_), volume(vol_), bizindex(bizindex_),
          traded_volume(0), status(OrderStatus::Submitted),
          timestamp(std::chrono::system_clock::now()) {}

    //方法
    [[nodiscard]] Quantity remaining_volume() const { return volume - traded_volume; } // 剩余数量
};

//价格档位
struct PriceLevel {
    Price price{}; // 价格
    Quantity total_size{}; // 总数量
    std::list<std::shared_ptr<Order>> orders; // 订单列表

    explicit PriceLevel(Price p) noexcept : price(p) {} // 构造函数

    void addOrder(const std::shared_ptr<Order>& od) {
        orders.push_back(od); // 添加订单
        od->level_iter = std::prev(orders.end()); // 更新订单的链表迭代器
        od->price_level = this; // 更新订单的价格档位
        total_size += od->volume; // 更新总数量
    }
    void removeOrder(const std::shared_ptr<Order>& od) {
        if (od->status != OrderStatus::Cancelled) { // 如果订单状态不是已撤销
            total_size -= od->remaining_volume(); // 更新总数量
            od->status = OrderStatus::Cancelled; // 更新订单状态
        }
        orders.erase(od->level_iter); // 从链表中移除订单
    }
    [[nodiscard]] bool isEmpty() const noexcept { return orders.empty(); } // 判断该价格档位是否为空
};

//成交记录
struct Execution {
    uint64_t  execution_id; // 成交ID
    uint64_t  buy_order_id; // 买方订单ID
    uint64_t  sell_order_id; // 卖方订单ID
    Price     price; // 成交价格
    Quantity  volume; // 成交数量
    Timestamp timestamp; // 成交时间

    Execution(uint64_t buy_id, uint64_t sell_id, Price px, Quantity vol) // 构造函数
        : execution_id(generate_execution_id()), // 生成成交ID
          buy_order_id(buy_id), // 买方订单ID
          sell_order_id(sell_id), // 卖方订单ID
          price(px), // 成交价格
          volume(vol), // 成交数量
          timestamp(std::chrono::system_clock::now()) {} // 当前时间
};

//价格统计（集合竞价用）
struct PriceStats {
    Price     price{}; // 价格
    Quantity  upper_buy_vol{}; // 高于此价格的买单总量
    Quantity  lower_sell_vol{}; // 低于此价格的卖单总量
    Quantity  same_price_buy_vol{}; // 此价格的买单量
    Quantity  same_price_sell_vol{}; // 此价格的卖单量
};

//五档快照  
struct Snapshot {
    uint64_t   timeid{}; // 时间ID
    Timestamp  datetime; // 时间
    std::string sym; // 代码
    Price      last_price{}; // 最新成交价

    std::array<Price, 5> bids{}; // 买方价格
    std::array<Quantity, 5> bid_sizes{}; // 买方数量
    std::array<Price, 5> asks{}; // 卖方价格
    std::array<Quantity, 5> ask_sizes{}; // 卖方数量
};

//撮合统计
struct Stats {
    uint64_t total_orders{}; // 总订单数
    uint64_t total_cancels{}; // 总撤销数
    uint64_t active_orders{}; // 活跃订单数
    Price    last_open_price{}; // 最新开盘价
    Price    last_open_volume{}; // 最新开盘量
};

} // namespace wangcai_orderbook_cpp