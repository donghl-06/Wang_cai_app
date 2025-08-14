/*
 * @Author: chenlisen
 * @Date: 2025-08-12 12:27:49
 * @LastEditTime: 2025-08-13 10:09:08
 * @FilePath: /wangcai_cpp/include/OrderLoader.h
 */
#pragma once
#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include <string>
#include <vector>

namespace wangcai_orderbook_cpp {

// 基础数据加载函数
void load_orders_from_csv(const std::string& filename, wangcai_orderbook_cpp::OrderBook& order_book);
void load_traders_from_csv(const std::string& filename, wangcai_orderbook_cpp::OrderBook& order_book);
void load_cstick_from_csv(const std::string& filename, wangcai_orderbook_cpp::OrderBook& order_book);

// 有序事件管理函数
void insert_event(const Event& event);

void clear_events();

wangcai_orderbook_cpp::Price loadPrevClosePrice(const std::string& filename);
wangcai_orderbook_cpp::Price loadOpenPrice(const std::string& filename);
void validateCallAuction(const std::string& stock_code, const std::string& date);

} // namespace wangcai_orderbook_cpp
