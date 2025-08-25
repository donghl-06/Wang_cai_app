/*
 * @Author: chenlisen
 * @Date: 2025-08-12 12:27:49
 * @LastEditTime: 2025-08-25 02:00:35
 * @FilePath: /wangcai_cpp/include/OrderLoader.h
 */
#pragma once
#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <execution>
#include <cmath>
#include <numeric>
namespace wangcai {

class InfoLoader {
public:
    InfoLoader() = default;
    ~InfoLoader() = default;
    void load_sh_info(const std::string& order_filename, std::string trade_filename, wangcai::OrderBook& order_book);
    void load_sz_info(const std::string& order_filename, std::string trade_filename, wangcai::OrderBook& order_book);
    void load_cstick_from_csv(const std::string& filename, wangcai::OrderBook& order_book);
    wangcai::Price loadPrevClosePrice(const std::string& filename);
    wangcai::Price loadOpenPrice(const std::string& filename);
private:
    void load_traders_from_csv(const std::string& filename, wangcai::OrderBook& order_book);
    void insert_event(const Event& event);
    void clear_events();
private:
    std::vector<Event> call_auction_orders_;
    std::vector<Event> continuous_orders_;
    std::vector<Event> continuous_trades_;
};


struct RecoverEvents{
    int64_t bizindex;
    int64_t orderid;
    int64_t size;
    bool is_trade;
    const Event* order_ptr;  

    RecoverEvents(int64_t biz, int64_t oid, int64_t sz, bool trade, const Event* ptr = nullptr)
        : bizindex(biz), orderid(oid), size(sz), is_trade(trade), order_ptr(ptr) {}
};

struct MissingOrderInfo {
    const Event* first_trade = nullptr;
    int64_t total_size = 0;
    int64_t bid_max_price = 0;
    int64_t ask_min_price = std::numeric_limits<int64_t>::max();
};

} // namespace wangcai
