#ifndef ORDERLOADER_H
#define ORDERLOADER_H

#include "orderbook.h"
#include <string>

// 加载订单数据从CSV文件
void load_orders_from_csv(const std::string& filename, wangcai_orderbook_cpp::OrderBook& order_book);
// 加载交易者数据从CSV文件
void load_traders_from_csv(const std::string& filename, wangcai_orderbook_cpp::OrderBook& order_book);
// 加载prevclose
void load_cstick_from_csv(const std::string& filename, wangcai_orderbook_cpp::OrderBook& order_book);

#endif // ORDERLOADER_H
