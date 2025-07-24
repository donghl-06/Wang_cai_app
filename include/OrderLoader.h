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

void print_event_statistics();
void clear_events();

// 集合竞价验证函数
std::vector<Event> loadOrderData(const std::string& filename);
std::vector<Event> loadCancelData(const std::string& filename);
wangcai_orderbook_cpp::Price loadPrevClosePrice(const std::string& filename);
wangcai_orderbook_cpp::Price loadOpenPrice(const std::string& filename);
void validateCallAuction(const std::string& stock_code, const std::string& date);

} // namespace wangcai_orderbook_cpp
