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
    
    // 从CSV字符串加载数据（Python DataFrame.to_csv() 生成的字符串）
    void load_sh_info(const std::string& order_csv_content, const std::string& trade_csv_content, wangcai::OrderBook& order_book);
    void load_sz_info(const std::string& order_csv_content, const std::string& trade_csv_content, wangcai::OrderBook& order_book);
    void load_cstick(const std::string& csv_content, wangcai::OrderBook& order_book);
    wangcai::Price loadPrevClosePrice(const std::string& csv_content);
    wangcai::Price loadOpenPrice(const std::string& csv_content);
    
    // 从csbar1d加载涨跌停限制（返回pair<上限,下限>，单位：厘，已包含0.1元冗余）
    std::pair<wangcai::Price, wangcai::Price> loadPriceLimits(const std::string& csbar1d_csv_content);
    
private:
    void load_traders(const std::string& csv_content, wangcai::OrderBook& order_book);
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
