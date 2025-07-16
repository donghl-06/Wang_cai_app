#include "OrderLoader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <cmath> 
#include <chrono>
#include <iomanip>
#include <unordered_map>
#include <map>

// 事件结构
struct Event {
    std::string datetime;
    std::string sym;
    int64_t price;
    int64_t size;
    int64_t side;
    int64_t ordertype;
    int64_t orderid;
    int64_t channelno;
    int64_t seqno;
    int64_t bizindex;
    int64_t bidorderid;
    int64_t askorderid;
    int64_t tradeid;
    std::string exectype;
    std::string tradebsflag;
    std::string source; // "ord" 或 "tra"
    uint64_t sort_key; // 排序键：SZ用orderid，SH用bizindex
    
    Event(const std::string& dt, const std::string& symbol, int64_t p, int64_t sz, int64_t sd, 
          int64_t ot, int64_t oid, int64_t ch, int64_t seq, int64_t biz, int64_t bid, int64_t ask, 
          int64_t tid, const std::string& et, const std::string& tbf, const std::string& src) 
        : datetime(dt), sym(symbol), price(p), size(sz), side(sd), ordertype(ot), orderid(oid),
          channelno(ch), seqno(seq), bizindex(biz), bidorderid(bid), askorderid(ask), tradeid(tid),
          exectype(et), tradebsflag(tbf), source(src) {
        // 根据交易所设置排序键
        if (symbol.substr(symbol.size() - 2) == "SZ") {
            sort_key = static_cast<uint64_t>(orderid);
        } else {
            sort_key = static_cast<uint64_t>(bizindex);
        }
    }
};

// 全局有序列表
static std::map<uint64_t, std::vector<Event>> sh_events; // 沪市事件，按bizindex排序
static std::map<uint64_t, std::vector<Event>> sz_events; // 深市事件，按orderid排序

// 插入事件到有序列表
void insert_event(const Event& event) {
    // 时间过滤：只处理集合竞价时间段 09:15:00 到 09:25:00
    std::string time_part = event.datetime.substr(11); // 提取时间部分 HH:MM:SS
    if (time_part < "09:15:00" || time_part >= "09:25:00") {
         return; // 跳过不在集合竞价时间段的事件
    }
    
    if (event.sym.substr(event.sym.size() - 2) == "SZ") {
        // 深市：按orderid排序
        sz_events[event.sort_key].push_back(event);
    } else {
        // 沪市：按bizindex排序  
        sh_events[event.sort_key].push_back(event);
    }
}

// 工具函数：将价格字符串转为int64_t，*10000并四舍五入到100
inline int64_t parse_price(const std::string& price_str) {
    if (price_str.empty()) return 0;
    double price_raw = std::stod(price_str);
    double price_multiplied = price_raw * 10000;
    double price_rounded = std::round(price_multiplied / 100.0) * 100.0;
    return static_cast<int64_t>(price_rounded);
}

// 加载订单数据
void load_orders_from_csv(const std::string& csv_file, wangcai_orderbook_cpp::OrderBook& order_book) {
    std::ifstream file(csv_file);  
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    while (std::getline(file, line)) {
        try {
            std::istringstream ss(line);
            std::string datetime, sym, ordertype_str, channelno_str, seqno_str, bizindex_str, price_str, size_str, side_str, orderid_str;

            std::getline(ss, datetime, ','); 
            std::getline(ss, sym, ',');
            std::getline(ss, price_str, ',');
            std::getline(ss, size_str, ',');
            std::getline(ss, side_str, ',');
            std::getline(ss, ordertype_str, ',');
            std::getline(ss, orderid_str, ',');
            std::getline(ss, channelno_str, ',');
            std::getline(ss, seqno_str, ',');
            std::getline(ss, bizindex_str, ',');

            // 解析数据
            int64_t price = parse_price(price_str);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t side = side_str.empty() ? 0 : std::stoi(side_str);
            int64_t ordertype = ordertype_str.empty() ? 0 : std::stoi(ordertype_str);
            int64_t orderid = orderid_str.empty() ? 0 : std::stoi(orderid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t seqno = (seqno_str == "-9223372036854775808") ? 0 : std::stoi(seqno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoi(bizindex_str);
            
            // 创建并插入订单事件
            Event order_event(datetime, sym, price, size, side, ordertype, orderid, channelno, 
                            seqno, bizindex, -1, -1, -1, "", "", "ord");
            insert_event(order_event);

            // 创建订单对象用于引擎处理
            auto order = order_book.createOrder(
                "EXCHANGE",  // broker
                "EXCHANGE", // account
                sym.substr(sym.size() - 2), // exchange (取代码后两位作为交易所)
                sym,              // instrument
                orderid_str,      // order_local_id (使用原始字符串)
                // 根据委托价格类型和交易所判断OrderType
                (sym.substr(sym.size() - 2) == "SZ"
                    ? (ordertype == 1 ? wangcai_orderbook_cpp::OrderType::Market
                        : (ordertype == 2 ? wangcai_orderbook_cpp::OrderType::Limit
                            : (ordertype == 3 ? wangcai_orderbook_cpp::OrderType::BestOwn
                                : wangcai_orderbook_cpp::OrderType::Limit)))
                    : wangcai_orderbook_cpp::OrderType::Limit), // 沪市全部为限价
                // 根据方向判断Direction
                side == 1 ? wangcai_orderbook_cpp::Direction::Buy : wangcai_orderbook_cpp::Direction::Sell,
                price,           // price
                size,            // volume
                bizindex
            );

        } catch (const std::exception& e) {
            std::cerr << "处理订单时发生错误: " << e.what() << std::endl;
        }
    }
}

// 加载撤单数据
void load_traders_from_csv(const std::string& csv_file, wangcai_orderbook_cpp::OrderBook& order_book) {
    std::istringstream file(csv_file);
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    while (std::getline(file, line)) {
        try {
            std::istringstream ss(line);
            std::string datetime, sym, exectype, tradebsflag, channelno_str, bizindex_str, price_str, size_str, bidorderid_str, askorderid_str, tradeid_str;

            // 解析CSV行
            std::getline(ss, datetime, ',');
            std::getline(ss, sym, ',');
            std::getline(ss, price_str, ',');
            std::getline(ss, size_str, ',');
            std::getline(ss, bidorderid_str, ',');
            std::getline(ss, askorderid_str, ',');
            std::getline(ss, tradeid_str, ',');
            std::getline(ss, exectype, ',');
            std::getline(ss, tradebsflag, ',');
            std::getline(ss, channelno_str, ',');
            std::getline(ss, bizindex_str, ',');
            
            // 去除空格
            tradebsflag.erase(std::remove(tradebsflag.begin(), tradebsflag.end(), ' '), tradebsflag.end());

            int64_t price = parse_price(price_str);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t bidorderid = bidorderid_str.empty() ? 0 : std::stoi(bidorderid_str);
            int64_t askorderid = askorderid_str.empty() ? 0 : std::stoi(askorderid_str);
            int64_t tradeid = tradeid_str.empty() ? 0 : std::stoi(tradeid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoi(bizindex_str);

            // 创建并插入撤单事件
            Event cancel_event(datetime, sym, price, size, -1, -1, tradeid, channelno, -1, bizindex, 
                             bidorderid, askorderid, tradeid, exectype, tradebsflag, "tra");
            insert_event(cancel_event);

        } catch (const std::exception& e) {
            std::cerr << "处理撤单时发生错误: " << e.what() << std::endl;
        }
    }
}

// 获取合并后的有序事件列表用于处理
std::vector<Event> get_merged_events() {
    std::vector<Event> merged_events;
    
    // 合并沪市事件
    for (const auto& pair : sh_events) {
        for (const auto& event : pair.second) {
            merged_events.push_back(event);
        }
    }
    
    // 合并深市事件
    for (const auto& pair : sz_events) {
        for (const auto& event : pair.second) {
            merged_events.push_back(event);
        }
    }
    
    // 按时间排序最终结果
    std::sort(merged_events.begin(), merged_events.end(), 
              [](const Event& a, const Event& b) {
                  return a.datetime < b.datetime;
              });
    
    return merged_events;
}

// 打印事件统计信息
void print_event_statistics() {
    std::cout << "=== 事件统计信息 ===" << std::endl;
    std::cout << "沪市事件数量: " << std::endl;
    size_t sh_total = 0;
    for (const auto& pair : sh_events) {
        size_t count = pair.second.size();
        sh_total += count;
        if (count > 1) {
            std::cout << "  bizindex " << pair.first << ": " << count << " 个事件" << std::endl;
        }
    }
    std::cout << "  总计: " << sh_total << " 个事件" << std::endl;
    
    std::cout << "深市事件数量: " << std::endl;
    size_t sz_total = 0;
    for (const auto& pair : sz_events) {
        size_t count = pair.second.size();
        sz_total += count;
        if (count > 1) {
            std::cout << "  orderid " << pair.first << ": " << count << " 个事件" << std::endl;
        }
    }
    std::cout << "  总计: " << sz_total << " 个事件" << std::endl;
    std::cout << "总事件数: " << (sh_total + sz_total) << std::endl;
}

// 加载cstick用来获取开盘价
void load_cstick_from_csv(const std::string& csv_file, wangcai_orderbook_cpp::OrderBook& order_book) {
    std::istringstream file(csv_file);
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    std::string last_line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            last_line = line;
        }
    }

    if (!last_line.empty()) {
        try {
            std::istringstream ss(last_line);
            std::string datetime, sym, prevclose_str, open_str;
            
            std::getline(ss, datetime, ',');
            std::getline(ss, sym, ',');
            std::getline(ss, prevclose_str, ',');
            std::getline(ss, open_str, ',');
            
            wangcai_orderbook_cpp::Price prev_close = parse_price(prevclose_str);
            wangcai_orderbook_cpp::Price open_price = parse_price(open_str);
            
            order_book.setPrevClosePrice(prev_close);
            
            std::cout << "前收盘价: " << prev_close / 10000.0 << " 元" << std::endl;
            std::cout << "开盘价: " << open_price / 10000.0 << " 元" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "处理价格数据时发生错误: " << e.what() << std::endl;
        }
    }
}

// 清空事件列表
void clear_events() {
    sh_events.clear();
    sz_events.clear();
}

// 从文件读取数据的辅助函数
std::vector<Event> loadOrderData(const std::string& filename) {
    std::vector<Event> orders;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cout << "错误：无法打开订单文件 " << filename << std::endl;
        return orders;
    }
    
    std::string line;
    std::getline(file, line); // 跳过标题行
    // std::cout << "订单文件标题: " << line << std::endl;
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        
        std::istringstream ss(line);
        std::string field;
        
        std::string datetime, sym, price_str, size_str, side_str, ordertype_str, orderid_str, channelno_str, seqno_str, bizindex_str;
        
        // datetime,sym,price,size,side,ordertype,orderid,channelno,seqno,bizindex
        std::getline(ss, datetime, ',');
        std::getline(ss, sym, ',');
        std::getline(ss, price_str, ',');
        std::getline(ss, size_str, ',');
        std::getline(ss, side_str, ',');
        std::getline(ss, ordertype_str, ',');
        std::getline(ss, orderid_str, ',');
        std::getline(ss, channelno_str, ',');
        std::getline(ss, seqno_str, ',');
        std::getline(ss, bizindex_str, ',');
        
        int64_t price = parse_price(price_str);
        int64_t size = static_cast<int64_t>(std::stod(size_str));
        int64_t side = std::stoi(side_str);
        int64_t ordertype = std::stoi(ordertype_str);
        int64_t orderid = std::stoull(orderid_str);
        int64_t channelno = std::stoi(channelno_str);
        int64_t seqno = std::stoull(seqno_str);
        int64_t bizindex = std::stoull(bizindex_str);
        
        Event order_event(datetime, sym, price, size, side, ordertype, orderid, channelno, 
                        seqno, bizindex, -1, -1, -1, "", "", "ord");
        orders.push_back(order_event);
    }
    
    std::cout << "加载了 " << orders.size() << " 条订单记录" << std::endl;
    return orders;
}

std::vector<Event> loadCancelData(const std::string& filename) {
    std::vector<Event> cancels;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cout << "错误：无法打开撤单文件 " << filename << std::endl;
        return cancels;
    }
    
    std::string line;
    std::getline(file, line); // 跳过标题行
    // std::cout << "撤单文件标题: " << line << std::endl;
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        
        std::istringstream ss(line);
        std::string field;
        
        std::string datetime, sym, price_str, size_str, bidorderid_str, askorderid_str, tradeid_str, ordertype_str, tradebsflag, channelno_str, bizindex_str;
        
        // datetime,sym,price,size,bidorderid,askorderid,tradeid,exectype,tradebsflag,channelno,bizindex
        std::getline(ss, datetime, ',');
        std::getline(ss, sym, ',');
        std::getline(ss, price_str, ',');
        std::getline(ss, size_str, ',');
        std::getline(ss, bidorderid_str, ',');
        std::getline(ss, askorderid_str, ',');
        std::getline(ss, tradeid_str, ',');
        std::getline(ss, ordertype_str, ',');
        std::getline(ss, tradebsflag, ',');
        std::getline(ss, channelno_str, ',');
        std::getline(ss, bizindex_str, ',');
        
        // 去除空格
        tradebsflag.erase(std::remove(tradebsflag.begin(), tradebsflag.end(), ' '), tradebsflag.end());
        
        int64_t price = parse_price(price_str);
        int64_t size = static_cast<int64_t>(std::stod(size_str));
        int64_t bidorderid = std::stoull(bidorderid_str);
        int64_t askorderid = std::stoull(askorderid_str);
        int64_t tradeid = std::stoull(tradeid_str);
        int64_t ordertype = std::stoi(ordertype_str);
        int64_t channelno = std::stoi(channelno_str);
        int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoull(bizindex_str);
        
        // 只处理exectype=2的撤单
        if (ordertype == 2) {
            Event cancel_event(datetime, sym, price, size, -1, -1, tradeid, channelno, -1, bizindex, 
                             bidorderid, askorderid, tradeid, std::to_string(ordertype), tradebsflag, "tra");
            cancels.push_back(cancel_event);
        }
    }
    
    std::cout << "加载了 " << cancels.size() << " 条撤单记录" << std::endl;
    return cancels;
}

wangcai_orderbook_cpp::Price loadPrevClosePrice(const std::string& filename) {
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cout << "错误：无法打开cstick文件 " << filename << std::endl;
        return 0;
    }

    std::string line;
    std::getline(file, line); // 读取标题行
    // std::cout << "行情文件标题: " << line.substr(0, 100) << "..." << std::endl;

    // 读取最后一行数据
    std::string last_line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            last_line = line;
        }
    }

    if (!last_line.empty()) {
        std::istringstream ss(last_line);
        std::string datetime, sym, prevclose_str;
        std::getline(ss, datetime, ',');
        std::getline(ss, sym, ',');
        std::getline(ss, prevclose_str, ',');
        double prev_close_raw = std::stod(prevclose_str);
        // 先乘以10000，再四舍五入到100
        double prev_close_multiplied = prev_close_raw * 10000;
        double prev_close_rounded = std::round(prev_close_multiplied / 100.0) * 100.0;
        wangcai_orderbook_cpp::Price prev_close = static_cast<wangcai_orderbook_cpp::Price>(prev_close_rounded);
        return prev_close;
    }

    return 0;
}

wangcai_orderbook_cpp::Price loadOpenPrice(const std::string& filename) {
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cout << "错误：无法打开cstick文件 " << filename << std::endl;
        return 0;
    }
    
    std::string line;
    std::getline(file, line); // 跳过标题行
    
     // 读取最后一行数据
     std::string last_line;
     while (std::getline(file, line)) {
         if (!line.empty()) {
             last_line = line;
         }
     }
 
     if (!last_line.empty()) {
         std::istringstream ss(last_line);
         std::string datetime, sym, prevclose_str, open_str;
         std::getline(ss, datetime, ',');
         std::getline(ss, sym, ',');
         std::getline(ss, prevclose_str, ',');
         std::getline(ss, open_str, ',');
         
        //  std::cout << "开盘价(最后一行): " << open_str << " 元" << std::endl;
         double open_raw = std::stod(open_str);
         // 先乘以10000，再四舍五入到100
         double open_multiplied = open_raw * 10000;
         double open_rounded = std::round(open_multiplied / 100.0) * 100.0;
         wangcai_orderbook_cpp::Price open_price = static_cast<wangcai_orderbook_cpp::Price>(open_rounded);

        //  std::cout << "开盘价(最后一行): " << open_price / 10000.0 << " 元" << std::endl;
         return open_price;
    }
    
    return 0;
}

// 集合竞价验证主函数
void validateCallAuction(const std::string& stock_code, const std::string& date) {
    std::cout << "========== 集合竞价验证程序 ==========" << std::endl;
    std::cout << "股票代码: " << stock_code << ", 日期: " << date << std::endl;
    
    // 构建文件路径
    std::string csord_file = "../logs/csord_" + stock_code + "_" + date + ".csv";
    std::string cstra_file = "../logs/cstra_" + stock_code + "_" + date + ".csv";
    std::string cstick_file = "../logs/cstick_" + stock_code + "_" + date + ".csv";
    
    // 1. 读取前收盘价和实际开盘价
    wangcai_orderbook_cpp::Price prev_close = loadPrevClosePrice(cstick_file);
    wangcai_orderbook_cpp::Price actual_open = loadOpenPrice(cstick_file);
    
    if (prev_close == 0) {
        std::cout << "无法获取前收盘价，退出验证" << std::endl;
        return;
    }
    
    // 2. 初始化订单簿和集合竞价引擎
    double upper_raw = prev_close * 1.1 / 10000.0;  // 按开盘价上10%
    double lower_raw = prev_close * 0.9 / 10000.0;  // 按开盘价下10%
    double upper = std::round(upper_raw * 100) / 100.0;  // 四舍五入到0.01
    double lower = std::floor(lower_raw * 100) / 100.0;  // 向下取0.01
    int lower_int = parse_price(std::to_string(lower));
    int upper_int = parse_price(std::to_string(upper));
    
    std::cout << "价格范围(基于前收盘价): " << lower << " - " << upper << " 元" << std::endl;
    
    wangcai_orderbook_cpp::OrderBook ob(upper_int, lower_int, false);
    ob.setPrevClosePrice(prev_close);
    ob.setExchange(stock_code.substr(stock_code.size() - 2)); // 从股票代码获取交易所
    
    wangcai_orderbook_cpp::CallAuctionEngine auction_engine(ob, prev_close, stock_code.substr(stock_code.size() - 2),
        nullptr, // 价格回调
        nullptr);
    
    // 3. 清空全局事件列表并加载数据
    clear_events();
    auto orders = loadOrderData(csord_file);
    auto cancels = loadCancelData(cstra_file);
    
    // 将数据插入到有序列表中
    for (const auto& order : orders) {
        insert_event(order);
    }
    for (const auto& cancel : cancels) {
        insert_event(cancel);
    }
    
    if (orders.empty()) {
        std::cout << "没有订单数据，无法进行验证" << std::endl;
        return;
    }
    
    std::cout << "\n--- 开始处理集合竞价订单 ---" << std::endl;
    
    // 4. 获取合并后的有序事件列表
    auto merged_events = get_merged_events();
    std::cout << "总事件数: " << merged_events.size() << std::endl;
    
    // 5. 按时间顺序处理事件
    int processed_orders = 0;
    int processed_cancels = 0;
    int skipped_price_range = 0;
    int buy_orders = 0, sell_orders = 0;
    
    for (const auto& event : merged_events) {
        try {
            if (event.source == "ord") { // 订单
                // 检查价格范围并对齐
                wangcai_orderbook_cpp::Price aligned_price = event.price;
                wangcai_orderbook_cpp::Price lower_bound = lower_int;
                wangcai_orderbook_cpp::Price upper_bound = upper_int;
                
                if (aligned_price < lower_bound || aligned_price > upper_bound || aligned_price == 0) {
                    skipped_price_range++;
                    if (skipped_price_range <= 5) { // 只打印前5个被跳过的订单
                        std::cout << "跳过订单: 价格=" << aligned_price << " 超出范围[" << lower_bound << "," << upper_bound << "]" << std::endl;
                    }
                    continue; // 跳过超出涨跌停范围的订单
                }
                
                // 对齐到tick（股票tick=100）
                wangcai_orderbook_cpp::Price offset_from_lower = aligned_price - lower_bound;
                aligned_price = lower_bound + (offset_from_lower / 100) * 100;
                
                wangcai_orderbook_cpp::Direction dir = (event.side == 1) ? 
                    wangcai_orderbook_cpp::Direction::Buy : wangcai_orderbook_cpp::Direction::Sell;
                
                // 根据原始数据判断订单类型
                wangcai_orderbook_cpp::OrderType order_type;
                if (event.sym.substr(event.sym.size() - 2) == "SZ") {
                    // 深市：1为市价；2为限价；3为本方最优
                    if (event.ordertype == 1) order_type = wangcai_orderbook_cpp::OrderType::Market;
                    else if (event.ordertype == 2) order_type = wangcai_orderbook_cpp::OrderType::Limit;
                    else if (event.ordertype == 3) order_type = wangcai_orderbook_cpp::OrderType::BestOwn;
                    else order_type = wangcai_orderbook_cpp::OrderType::Limit;
                } else {
                    // 沪市全部为限价
                    order_type = wangcai_orderbook_cpp::OrderType::Limit;
                }
                
                // if (processed_orders < 5) { // 打印前5个订单的调试信息
                //     std::cout << "订单 " << processed_orders << ": 方向=" << (event.side == 1 ? "买" : "卖") 
                //               << ", 类型=" << event.ordertype << ", 价格=" << aligned_price 
                //               << ", id=" << event.orderid
                //               << ", 数量=" << event.size << std::endl;
                // }
                
                auto order = ob.createOrder(
                    "BROKER", "ACCOUNT", event.sym.substr(event.sym.size() - 2), event.sym, std::to_string(event.orderid),
                    order_type, dir, aligned_price, event.size, event.bizindex
                );
                
                auction_engine.accept(order);
                processed_orders++;
                if (event.side == 1) buy_orders++; else sell_orders++;
                
            } else { // 撤单
                // 根据撤单记录中的bidorderid或askorderid中不为零的那个来撤单
                uint64_t original_order_id = (event.bidorderid != 0) ? event.bidorderid : event.askorderid;
                
                // if (processed_cancels < 3) { // 打印前3个撤单的调试信息
                //     std::cout << "撤单 - 原始订单ID: " << original_order_id << std::endl;
                // }
                auction_engine.cancel_by_input_id(original_order_id);
                // processed_cancels++;
            }
            
        } catch (const std::exception& e) {
            std::cout << "处理事件时发生错误: " << e.what() << std::endl;
        }
    }
    
    std::cout << "处理完成 - 订单: " << processed_orders << ", 撤单: " << processed_cancels << std::endl;
    std::cout << "买单数量: " << buy_orders << ", 卖单数量: " << sell_orders << std::endl;
    std::cout << "因价格超范围跳过的订单: " << skipped_price_range << std::endl;
    
    // 6. 获取集合竞价结果
    wangcai_orderbook_cpp::Price predicted_price = auction_engine.getPredictPrice();
    wangcai_orderbook_cpp::Quantity predicted_volume = auction_engine.getPredictVolume();

    // 7. 打印订单簿到csv
    //auction_engine.print_orderbook_to_csv("../logs/orderbook_" + stock_code + "_" + date + ".csv");
    
    // 添加调试信息
    // std::cout << "\n--- 集合竞价引擎状态 ---" << std::endl;
    // std::cout << "总买量: " << auction_engine.getTotalBuy() << std::endl;
    // std::cout << "总卖量: " << auction_engine.getTotalSell() << std::endl;
    
    // 7. 对比结果
    std::cout << "\n========== 验证结果 ==========" << std::endl;
    std::cout << "前收盘价: " << prev_close / 10000.0 << " 元" << std::endl;
    std::cout << "预测开盘价: " << predicted_price / 10000.0 << " 元" << std::endl;
    std::cout << "实际开盘价: " << actual_open / 10000.0 << " 元" << std::endl;
    std::cout << "预测成交量: " << predicted_volume << " 股" << std::endl;
    
    if (predicted_price == actual_open) {
        std::cout << "✅ 验证成功！预测价格与实际开盘价完全一致" << std::endl;
    } else {
        double diff_percent = std::abs(static_cast<double>(predicted_price - actual_open)) / actual_open * 100;
        std::cout << "❌ 预测价格与实际开盘价不一致，差异: " << 
            std::fixed << std::setprecision(4) << diff_percent << "%" << std::endl;
    }
    
    std::cout << "=================================" << std::endl;
    
    // 清理事件列表
    clear_events();
}
