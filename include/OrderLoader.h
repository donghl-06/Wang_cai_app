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
    
    // 加载前收盘价和开盘价（is_etf: true=ETF三位小数, false=股票两位小数）
    wangcai::Price loadPrevClosePrice(const std::string& csv_content, bool is_etf = false);
    wangcai::Price loadOpenPrice(const std::string& csv_content, bool is_etf = false);
    
    // 从csbar1d加载涨跌停限制（返回pair<上限,下限>，单位：厘，已包含0.1元冗余）
    // is_etf: true=ETF（tick=10厘=0.001元）, false=股票（tick=100厘=0.01元）
    // prev_close_yuan/px_lo_yuan/px_hi_yuan: 仅当 csbar1d 涨跌停为 0（新股上市
    // 前 5 日无涨跌幅）时用于构造簿边界——取全天委托/成交价格范围并含前收。
    // trade_lo_yuan/trade_hi_yuan: 全天成交价范围（撤单行 price=0 已被扫描端
    // 过滤）。无涨跌幅日申报可含恶作剧天价（301408.SZ 2023-03-01 上市首日
    // 26033921 元卖单），簿边界以成交范围×kOutOfBookFactor 封顶，界外历史
    // 委托由各 accept 段吸收进簿外价表（永不成交，语义论证见实现注释）。
    static constexpr double kOutOfBookFactor = 1000.0;
    std::pair<wangcai::Price, wangcai::Price> loadPriceLimits(
        const std::string& csbar1d_csv_content, bool is_etf = false,
        double prev_close_yuan = 0.0, double px_lo_yuan = 0.0, double px_hi_yuan = 0.0,
        double trade_lo_yuan = 0.0, double trade_hi_yuan = 0.0);

    // 扫描委托/成交 CSV 第 4 列(price,元)取 [min,max];无有效价格返回 {0,0}
    static std::pair<double, double> scanPriceRange(
        const std::string& csv_a, const std::string& csv_b);
    
private:
    void load_traders(const std::string& csv_content, wangcai::OrderBook& order_book);
    void insert_event(const Event& event);
    
private:
    std::vector<Event> call_auction_orders_;
    std::vector<Event> continuous_orders_;
    std::vector<Event> continuous_trades_;
    // 深市裸市价单(ordertype=1 且 price=0,≥2023-04-10)集合:load_sz_info 收集,
    // load_traders 据此放行其成交记录进事件流(事件驱动执行的执行依据)
    std::unordered_set<int64_t> naked_market_ids_;
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
