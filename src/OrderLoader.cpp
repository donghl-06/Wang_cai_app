#include "OrderLoader.h"
#include "price_cage.h"
#include <iostream>
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


namespace wangcai {

// 获取 tick 大小（ETF=10厘=0.001元，股票=100厘=0.01元）
inline Price get_tick(bool is_etf) { return is_etf ? 10 : 100; }

// pandas/CSV 中 object 列可能保留为 "b'1'"、"b'B'" 等字面量。
// 在 C++ 边界再次规范化，避免方向/成交类型被首字符 'b' 误判。
inline std::string normalize_encoded_scalar(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);

    if (value.size() >= 3 && (value[0] == 'b' || value[0] == 'B') &&
        ((value[1] == '\'' && value.back() == '\'') ||
         (value[1] == '"' && value.back() == '"'))) {
        return value.substr(2, value.size() - 3);
    }
    if (value.size() >= 2 &&
        ((value.front() == '\'' && value.back() == '\'') ||
         (value.front() == '"' && value.back() == '"'))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// 工具函数：将价格字符串转为int64_t，*10000并四舍五入到tick
// tick: ETF=10, 股票=100
inline uint64_t parse_price(const std::string& price_str, Price tick = 100) {
    if (price_str.empty()) return 0;
    double price_raw = std::stod(price_str);
    double price_multiplied = price_raw * 10000;
    double price_rounded = std::round(price_multiplied / tick) * tick;
    return static_cast<int64_t>(price_rounded);
}

// 辅助函数：将 adata 的时间格式转换为统一格式 "HH:MM:SS.fff"
// 输入 time 格式: "0 days HH:MM:SS.ffffff" 或 "0 days HH:MM:SS"
// 输出格式: "HH:MM:SS.fff"
std::string format_time_to_milliseconds(const std::string& time) {
    // 从 "0 days " 后开始提取
    std::string time_part = time.substr(7);
    
    size_t dot_pos = time_part.find('.');
    if (dot_pos == std::string::npos) {
        // 没有小数点，添加 ".000"
        return time_part + ".000";
    } else if (time_part.length() > dot_pos + 4) {
        // 超过3位毫秒，截断到3位
        return time_part.substr(0, dot_pos + 4);
    } else if (time_part.length() < dot_pos + 4) {
        // 不足3位毫秒，补零
        return time_part + std::string(dot_pos + 4 - time_part.length(), '0');
    }
    return time_part;
}

inline bool isContinuousTime(std::string_view datetime_str) {
    return datetime_str.length() >= 19 && 
            datetime_str.substr(11, 8) >= "09:30:00";
}

void InfoLoader::load_sh_info(const std::string& order_csv_content, const std::string& trade_csv_content, wangcai::OrderBook& order_book) {
    std::istringstream file(order_csv_content);
    std::string line;
    naked_market_ids_.clear();  // 沪市无裸市价单事件驱动路径,防实例复用残留
    std::getline(file, line); // 跳过标题行

    int total_lines = 0;
    int inserted_events = 0;

    while (std::getline(file, line)) {
        total_lines++;
        try {
            std::istringstream ss(line);
            std::string date, time, sym, ordertype_str, channelno_str, seqno_str, bizindex_str, price_str, size_str, side_str, orderid_str, updatetime_str;

            // 新格式: date,time,sym,price,size,side,ordertype,orderid,channelno,seqno,bizindex,updatetime
            std::getline(ss, date, ','); 
            std::getline(ss, time, ',');
            std::getline(ss, sym, ',');
            std::getline(ss, price_str, ',');
            std::getline(ss, size_str, ',');
            std::getline(ss, side_str, ',');
            std::getline(ss, ordertype_str, ',');
            std::getline(ss, orderid_str, ',');
            std::getline(ss, channelno_str, ',');
            std::getline(ss, seqno_str, ',');
            std::getline(ss, bizindex_str, ',');
            std::getline(ss, updatetime_str, ',');

            side_str = normalize_encoded_scalar(side_str);
            ordertype_str = normalize_encoded_scalar(ordertype_str);

            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;

            // 解析数据（使用 order_book 的 tick 值）
            Price tick = order_book.getTick();
            int64_t price = parse_price(price_str, tick);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t side = side_str.empty() ? 0 : std::stoi(side_str);
            int64_t ordertype = ordertype_str.empty() ? 0 : std::stoi(ordertype_str);
            int64_t orderid = orderid_str.empty() ? 0 : std::stoll(orderid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t seqno = (seqno_str == "-9223372036854775808") ? 0 : std::stoll(seqno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoll(bizindex_str);
            
            // 解析时间字段用于填充time_raw
            int time_raw = 0;
            if (datetime.length() >= 19) {
                // 从 "YYYY-MM-DD HH:MM:SS.sss" 格式提取HHMMSSsss
                std::string time_part = datetime.substr(11, 8); // HH:MM:SS
                std::string ms_part = datetime.length() > 20 ? datetime.substr(20, 3) : "000"; // .sss
                // 移除冒号并拼接：HHMMSSsss
                time_part.erase(std::remove(time_part.begin(), time_part.end(), ':'), time_part.end());
                time_raw = std::stoi(time_part + ms_part);
            }
            
            // 提取trading_day (从datetime中提取YYYYMMDD)
            int trading_day = 0;
            if (datetime.length() >= 10) {
                std::string date_part = datetime.substr(0, 10); // YYYY-MM-DD
                date_part.erase(std::remove(date_part.begin(), date_part.end(), '-'), date_part.end());
                trading_day = std::stoi(date_part);
            }

            // 创建并插入订单事件，填充新增的原始市场数据字段
            Event order_event(datetime, sym, price, size, side, ordertype, orderid, channelno, 
                            seqno, bizindex, -1, -1, -1, "", "", 
                            -1, -1, -1, -1, -1, -1, -1, -1, {}, {}, {}, {}, -1, -1, -1, -1, -1, "ord",
                            // === 新增的原始市场数据字段 ===
                            0,                              // exchange (SH上交所=0)
                            trading_day,                    // trading_day  
                            trading_day,                    // action_day (暂时与trading_day相同)
                            "",                             // status (委托数据中通常没有状态字段)
                            static_cast<char>('0' + ordertype), // order_kind ('1'市价/'2'限价/'3'本方最优等)
                            -1,                             // trade_index (订单数据中无此字段)
                            time_raw                        // time_raw (HHMMSSsss格式)
            );
            order_book.insertEvent(order_event);
            inserted_events++;

        } catch (const std::exception& e) {
            std::cerr << "处理订单时发生错误: " << e.what() << std::endl;
            // 输出是哪笔订单的错误
            std::cerr << "错误订单信息: " << line << std::endl;
            std::cerr << "错误详情: " << e.what() << std::endl;
            std::throw_with_nested(std::runtime_error("加载订单数据时发生错误"));
        }
    }

    load_traders(trade_csv_content, order_book);
}

void InfoLoader::load_sz_info(const std::string& order_csv_content, const std::string& trade_csv_content, wangcai::OrderBook& order_book) {
    std::istringstream file(order_csv_content);
    std::string line;
    // 深市裸市价单(ordertype=1 且 price=0,≥2023-04-10)集合:其成交记录需进
    // 事件流,供 BacktestEngine 事件驱动执行(子类被 adata 压平,见 accept_sz 注释)
    naked_market_ids_.clear();
    std::getline(file, line); // 跳过CSV文件的标题行

    while (std::getline(file, line)) {
        try {
            std::istringstream ss(line);
            std::string date, time, sym, ordertype_str, channelno_str, seqno_str, bizindex_str, price_str, size_str, side_str, orderid_str, updatetime_str;

            // 新格式: date,time,sym,price,size,side,ordertype,orderid,channelno,seqno,bizindex,updatetime
            std::getline(ss, date, ','); 
            std::getline(ss, time, ',');
            std::getline(ss, sym, ',');
            std::getline(ss, price_str, ',');
            std::getline(ss, size_str, ',');
            std::getline(ss, side_str, ',');
            std::getline(ss, ordertype_str, ',');
            std::getline(ss, orderid_str, ',');
            std::getline(ss, channelno_str, ',');
            std::getline(ss, seqno_str, ',');
            std::getline(ss, bizindex_str, ',');
            std::getline(ss, updatetime_str, ',');

            side_str = normalize_encoded_scalar(side_str);
            ordertype_str = normalize_encoded_scalar(ordertype_str);

            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;

            // 解析数据（使用 order_book 的 tick 值）
            Price tick = order_book.getTick();
            int64_t price = parse_price(price_str, tick);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t side = side_str.empty() ? 0 : std::stoi(side_str);
            int64_t ordertype = ordertype_str.empty() ? 0 : std::stoi(ordertype_str);
            int64_t orderid = orderid_str.empty() ? 0 : std::stoll(orderid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t seqno = (seqno_str == "-9223372036854775808") ? 0 : std::stoll(seqno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoll(bizindex_str);
            
            bool is_sz_mkt = (sym.size() >= 2 &&
                              sym.substr(sym.size() - 2) == "SZ" &&
                              ordertype != 2 && ordertype != 0);              // 非限价视为市价

            if (is_sz_mkt) {
                order_book.first_trade_px_[{static_cast<int>(channelno),
                                            static_cast<uint64_t>(orderid)}] = 0;
            }

            // 解析时间字段用于填充time_raw
            int time_raw = 0;
            if (datetime.length() >= 19) {
                // 从 "YYYY-MM-DD HH:MM:SS.sss" 格式提取HHMMSSsss
                std::string time_part = datetime.substr(11, 8); // HH:MM:SS
                std::string ms_part = datetime.length() > 20 ? datetime.substr(20, 3) : "000"; // .sss
                // 移除冒号并拼接：HHMMSSsss
                time_part.erase(std::remove(time_part.begin(), time_part.end(), ':'), time_part.end());
                time_raw = std::stoi(time_part + ms_part);
            }
            
            // 提取trading_day (从datetime中提取YYYYMMDD)
            int trading_day = 0;
            if (datetime.length() >= 10) {
                std::string date_part = datetime.substr(0, 10); // YYYY-MM-DD
                date_part.erase(std::remove(date_part.begin(), date_part.end(), '-'), date_part.end());
                trading_day = std::stoi(date_part);
            }

            // 收集裸市价单(其成交记录需进事件流,见 load_traders 放行)
            if (ordertype == 1 && price == 0 && trading_day >= 20230410) {
                naked_market_ids_.insert(orderid);
            }

            // 创建并插入订单事件，填充新增的原始市场数据字段
            Event order_event(datetime, sym, price, size, side, ordertype, orderid, channelno,
                            seqno, bizindex, -1, -1, -1, "", "",
                            -1, -1, -1, -1, -1, -1, -1, -1, {}, {}, {}, {}, -1, -1, -1, -1, -1, "ord",
                            // === 新增的原始市场数据字段 ===
                            1,                              // exchange (SZ深交所=1)
                            trading_day,                    // trading_day  
                            trading_day,                    // action_day (暂时与trading_day相同)
                            "",                             // status (委托数据中通常没有状态字段)
                            static_cast<char>('0' + ordertype), // order_kind ('1'市价/'2'限价/'3'本方最优等)
                            -1,                             // trade_index (订单数据中无此字段)
                            time_raw                        // time_raw (HHMMSSsss格式)
            );
            order_book.insertEvent(order_event);

        } catch (const std::exception& e) {
            std::cerr << "处理订单时发生错误: " << e.what() << std::endl;
            // 输出是哪笔订单的错误
            std::cerr << "错误订单信息: " << line << std::endl;
            std::cerr << "错误详情: " << e.what() << std::endl;
            std::throw_with_nested(std::runtime_error("加载订单数据时发生错误"));
        }
    }

    load_traders(trade_csv_content, order_book);
}


// 加载成交/撤单数据
void InfoLoader::load_traders(const std::string& csv_content, wangcai::OrderBook& order_book) {
    std::istringstream file(csv_content);
    
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    while (std::getline(file, line)) {
        try {
            std::istringstream ss(line);
            std::string date, time, sym, exectype, tradebsflag, channelno_str, bizindex_str, price_str, size_str, bidorderid_str, askorderid_str, tradeid_str, updatetime_str;

            // 新格式: date,time,sym,price,size,bidorderid,askorderid,tradeid,exectype,tradebsflag,channelno,bizindex,updatetime
            std::getline(ss, date, ',');
            std::getline(ss, time, ',');
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
            std::getline(ss, updatetime_str, ',');

            exectype = normalize_encoded_scalar(exectype);
            tradebsflag = normalize_encoded_scalar(tradebsflag);
            
            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;
            
            // 使用 order_book 的 tick 值（根据 is_etf 设置：ETF=10，股票=100）
            Price tick = order_book.getTick();
            int64_t price = parse_price(price_str, tick);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t bidorderid = bidorderid_str.empty() ? 0 : std::stoll(bidorderid_str);
            int64_t askorderid = askorderid_str.empty() ? 0 : std::stoll(askorderid_str);
            int64_t tradeid = tradeid_str.empty() ? 0 : std::stoll(tradeid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoll(bizindex_str);
            
            // 计算 time_raw (HHMMSSsss)
            int time_raw = 0;
            if (datetime.length() >= 19) {
                std::string time_part = datetime.substr(11, 8); // HH:MM:SS
                std::string ms_part   = datetime.length() > 20 ? datetime.substr(20, 3) : "000";
                time_part.erase(std::remove(time_part.begin(), time_part.end(), ':'), time_part.end());
                time_raw = std::stoi(time_part + ms_part);
            }

            // 记录最优成交价（买方记录最高价，卖方记录最低价）
            if (bidorderid != 0) {
                auto it = order_book.first_trade_px_.find(
                    {static_cast<int>(channelno), static_cast<uint64_t>(bidorderid)});
                if (it != order_book.first_trade_px_.end()) {
                    if (it->second == 0 || price > it->second) {  // 买方：记录最高价
                        it->second = price;
                    }
                }
            }

            if (askorderid != 0 && askorderid != bidorderid) {     // 避免同 ID 重复写
                auto it = order_book.first_trade_px_.find(
                    {static_cast<int>(channelno), static_cast<uint64_t>(askorderid)});
                if (it != order_book.first_trade_px_.end()) {
                    if ((it->second == 0 || price < it->second) && price != 0) {  // 卖方：记录最低价
                        it->second = price;
                    }
                }
            }

            int trading_day = 0;
            if (date.size() >= 10) {
                std::string date_part = date.substr(0, 10);
                date_part.erase(std::remove(date_part.begin(), date_part.end(), '-'), date_part.end());
                trading_day = std::stoi(date_part);
            }
            int exchange = sym.ends_with(".SZ") ? 1 : (sym.ends_with(".SH") ? 0 : -1);

            Event trade_event(datetime, sym, price, size, -1, -1, tradeid, channelno, -1, bizindex, 
                            bidorderid, askorderid, tradeid, exectype, tradebsflag, 
                            -1, -1, -1, -1, -1, -1, -1, -1, {}, {}, {}, {}, -1, -1, -1, -1, -1,"tra",
                            exchange,
                            trading_day,
                            trading_day,
                            "",             // status
                            '\0',            // order_kind
                            tradeid,         // trade_index
                            time_raw);       // time_raw
            // 创建并插入撤单事件
           if (exectype == "2") {
                order_book.insertEvent(trade_event);
            } else if (exectype != "2" && isContinuousTime(datetime)) {
                // 价格笼子反推法（仅深市创业板暂存窗口）：真实成交事件进事件流，
                // 供 BacktestEngine 做"穿价历史单的下一条消息确认"（不推策略、不动簿）。
                // 连续竞价段限定（笼子不管集合竞价，收盘竞价段无反推需要）。
                const bool in_continuous = datetime.substr(11, 8) < "14:57:00";
                // 裸市价单的成交同样进流：事件驱动执行的执行依据（见 load_sz_info 开头）
                const bool refs_naked_market =
                    naked_market_ids_.count(bidorderid) > 0 ||
                    naked_market_ids_.count(askorderid) > 0;
                if (in_continuous && refs_naked_market) {
                    order_book.insertEvent(trade_event);
                } else if (in_continuous && needsHistoricalCageInference(sym, date)) {
                    order_book.insertEvent(trade_event);
                } else {
                    continuous_trades_.emplace_back(std::move(trade_event));
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "处理撤单时发生错误: " << e.what() << std::endl;
            std::cerr << "错误成交信息: " << line << std::endl;
            std::throw_with_nested(std::runtime_error("加载成交/撤单数据时发生错误"));
        }
    }

}


// 加载cstick用来获取开盘价
void InfoLoader::load_cstick(const std::string& csv_content, wangcai::OrderBook& order_book) {
    std::istringstream file(csv_content);
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    while (std::getline(file, line)) {
        try {
            std::istringstream ss(line);
            std::string date, time, sym, prevclose_str, open_str, high_str, low_str,
                        close_str, volume_str, turnover_str, tradecount_str, 

                        bid1_str, bsize1_str, 
                        bid2_str, bsize2_str, 
                        bid3_str, bsize3_str, 
                        bid4_str, bsize4_str, 
                        bid5_str, bsize5_str, 
                        bid6_str, bsize6_str, 
                        bid7_str, bsize7_str, 
                        bid8_str, bsize8_str, 
                        bid9_str, bsize9_str, 
                        bid10_str, bsize10_str, 

                        ask1_str, asize1_str, 
                        ask2_str, asize2_str, 
                        ask3_str, asize3_str, 
                        ask4_str, asize4_str, 
                        ask5_str, asize5_str, 
                        ask6_str, asize6_str, 
                        ask7_str, asize7_str, 
                        ask8_str, asize8_str, 
                        ask9_str, asize9_str, 
                        ask10_str, asize10_str, 
                        
                        avgbid_str, avgask_str, totalbsize_str, totalasize_str, iopv_str;

            // 新格式: date,time,sym,prevclose,open,high,low,close,volume,turnover,tradecount,bid1,bsize1,...,ask10,asize10,avgbid,avgask,totalbsize,totalasize,iopv,...
            std::getline(ss, date, ',');
            std::getline(ss, time, ',');
            std::getline(ss, sym, ',');

            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;

            std::getline(ss, prevclose_str, ',');
            std::getline(ss, open_str, ',');
            std::getline(ss, high_str, ',');
            std::getline(ss, low_str, ',');
            std::getline(ss, close_str, ',');
            std::getline(ss, volume_str, ',');
            std::getline(ss, turnover_str, ',');
            std::getline(ss, tradecount_str, ',');
            // 10 bid
            std::getline(ss, bid1_str, ',');
            std::getline(ss, bsize1_str, ',');
            std::getline(ss, bid2_str, ',');
            std::getline(ss, bsize2_str, ',');
            std::getline(ss, bid3_str, ',');
            std::getline(ss, bsize3_str, ',');
            std::getline(ss, bid4_str, ',');
            std::getline(ss, bsize4_str, ',');
            std::getline(ss, bid5_str, ',');
            std::getline(ss, bsize5_str, ',');
            std::getline(ss, bid6_str, ',');
            std::getline(ss, bsize6_str, ',');
            std::getline(ss, bid7_str, ',');
            std::getline(ss, bsize7_str, ',');
            std::getline(ss, bid8_str, ',');
            std::getline(ss, bsize8_str, ',');
            std::getline(ss, bid9_str, ',');
            std::getline(ss, bsize9_str, ',');
            std::getline(ss, bid10_str, ',');
            std::getline(ss, bsize10_str, ',');
            // 10 ask
            std::getline(ss, ask1_str, ',');
            std::getline(ss, asize1_str, ',');
            std::getline(ss, ask2_str, ',');
            std::getline(ss, asize2_str, ',');
            std::getline(ss, ask3_str, ',');
            std::getline(ss, asize3_str, ',');
            std::getline(ss, ask4_str, ',');
            std::getline(ss, asize4_str, ',');
            std::getline(ss, ask5_str, ',');
            std::getline(ss, asize5_str, ',');
            std::getline(ss, ask6_str, ',');
            std::getline(ss, asize6_str, ',');
            std::getline(ss, ask7_str, ',');
            std::getline(ss, asize7_str, ',');
            std::getline(ss, ask8_str, ',');
            std::getline(ss, asize8_str, ',');
            std::getline(ss, ask9_str, ',');
            std::getline(ss, asize9_str, ',');
            std::getline(ss, ask10_str, ',');
            std::getline(ss, asize10_str, ',');

            std::getline(ss, avgbid_str, ',');
            std::getline(ss, avgask_str, ',');
            std::getline(ss, totalbsize_str, ',');
            std::getline(ss, totalasize_str, ',');
            std::getline(ss, iopv_str, ',');

            // 使用 order_book 的 tick 值（根据 is_etf 设置：ETF=10，股票=100）
            Price tick = order_book.getTick();

            int64_t prevclose = parse_price(prevclose_str, tick);
            int64_t open = parse_price(open_str, tick);
            int64_t high = parse_price(high_str, tick);
            int64_t low = parse_price(low_str, tick);
            int64_t close = parse_price(close_str, tick);
            int64_t volume = volume_str.empty() ? 0 : std::stod(volume_str);
            int64_t turnover = turnover_str.empty() ? 0 : std::stod(turnover_str);
            int64_t tradecount = tradecount_str.empty() ? 0 : std::stoi(tradecount_str);
            std::array<std::uint64_t, 10> bids = {
                parse_price(bid1_str, tick), parse_price(bid2_str, tick), parse_price(bid3_str, tick),
                parse_price(bid4_str, tick), parse_price(bid5_str, tick), parse_price(bid6_str, tick),
                parse_price(bid7_str, tick), parse_price(bid8_str, tick), parse_price(bid9_str, tick),
                parse_price(bid10_str, tick)
            };
            std::array<std::uint64_t, 10> asks = {
                parse_price(ask1_str, tick), parse_price(ask2_str, tick), parse_price(ask3_str, tick),
                parse_price(ask4_str, tick), parse_price(ask5_str, tick), parse_price(ask6_str, tick),
                parse_price(ask7_str, tick), parse_price(ask8_str, tick), parse_price(ask9_str, tick),
                parse_price(ask10_str, tick)
            };
            std::array<std::uint64_t, 10> bid_sizes = {
                bsize1_str.empty() ? 0 : std::stoul(bsize1_str), 
                bsize2_str.empty() ? 0 : std::stoul(bsize2_str),
                bsize3_str.empty() ? 0 : std::stoul(bsize3_str),
                bsize4_str.empty() ? 0 : std::stoul(bsize4_str),
                bsize5_str.empty() ? 0 : std::stoul(bsize5_str),
                bsize6_str.empty() ? 0 : std::stoul(bsize6_str),
                bsize7_str.empty() ? 0 : std::stoul(bsize7_str),
                bsize8_str.empty() ? 0 : std::stoul(bsize8_str),
                bsize9_str.empty() ? 0 : std::stoul(bsize9_str),
                bsize10_str.empty() ? 0 : std::stoul(bsize10_str)
            };
            std::array<std::uint64_t, 10> ask_sizes = {
                asize1_str.empty() ? 0 : std::stoul(asize1_str),
                asize2_str.empty() ? 0 : std::stoul(asize2_str),
                asize3_str.empty() ? 0 : std::stoul(asize3_str),
                asize4_str.empty() ? 0 : std::stoul(asize4_str),
                asize5_str.empty() ? 0 : std::stoul(asize5_str),
                asize6_str.empty() ? 0 : std::stoul(asize6_str),
                asize7_str.empty() ? 0 : std::stoul(asize7_str),
                asize8_str.empty() ? 0 : std::stoul(asize8_str),
                asize9_str.empty() ? 0 : std::stoul(asize9_str),
                asize10_str.empty() ? 0 : std::stoul(asize10_str)
            };

            int64_t avgbid = parse_price(avgbid_str, tick);
            int64_t avgask = parse_price(avgask_str, tick);
            int64_t totalbsize = totalbsize_str.empty() ? 0 : std::stoul(totalbsize_str);
            int64_t totalasize = totalasize_str.empty() ? 0 : std::stoul(totalasize_str);
            int64_t iopv = iopv_str.empty() ? 0 : std::stod(iopv_str);

            // ================= 计算 time_raw (HHMMSSsss) =================
            int time_raw = 0;
            if (datetime.length() >= 19) {
                // 从 "YYYY-MM-DD HH:MM:SS.sss" 中提取 HHMMSSsss
                std::string time_part = datetime.substr(11, 8);                     // HH:MM:SS
                std::string ms_part   = datetime.length() > 20 ? datetime.substr(20, 3) : "000"; // 毫秒
                time_part.erase(std::remove(time_part.begin(), time_part.end(), ':'), time_part.end()); // 移除冒号
                time_raw = std::stoi(time_part + ms_part);                                           // 拼接并转为int
            }

            int trading_day = 0;
            if (date.size() >= 10) {
                std::string date_part = date.substr(0, 10);
                date_part.erase(std::remove(date_part.begin(), date_part.end(), '-'), date_part.end());
                trading_day = std::stoi(date_part);
            }
            int exchange = sym.ends_with(".SZ") ? 1 : (sym.ends_with(".SH") ? 0 : -1);

            Event tick_event(datetime, sym, -1, -1, -1, -1, -1, -1, -1, -1,
                            -1, -1, -1, "", "", 
                            prevclose, open, high, low, close, volume, turnover, tradecount, bids, bid_sizes, asks, ask_sizes, avgbid, avgask, totalbsize, totalasize, iopv, "tick" 
                            , exchange, trading_day, trading_day, "", '\0', -1, time_raw);
                            
            order_book.insertTick(tick_event);
        } catch (const std::exception& e) {
            std::cerr << "处理Tick时发生错误: " << e.what() << std::endl;
        }
    }
}



wangcai::Price InfoLoader::loadPrevClosePrice(const std::string& csv_content, bool is_etf) {
    std::istringstream file(csv_content);

    std::string line;
    std::getline(file, line); // 读取标题行

    // 读取最后一行数据
    std::string last_line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            last_line = line;
        }
    }

    if (!last_line.empty()) {
        std::istringstream ss(last_line);
        std::string date, time, sym, prevclose_str;
        // 新格式: date,time,sym,prevclose,...
        std::getline(ss, date, ',');
        std::getline(ss, time, ',');
        std::getline(ss, sym, ',');
        std::getline(ss, prevclose_str, ',');
        double prev_close_raw = std::stod(prevclose_str);
        // 先乘以10000，再四舍五入到tick（ETF=10, 股票=100）
        Price tick = get_tick(is_etf);
        double prev_close_multiplied = prev_close_raw * 10000;
        double prev_close_rounded = std::round(prev_close_multiplied / tick) * tick;
        wangcai::Price prev_close = static_cast<wangcai::Price>(prev_close_rounded);
        return prev_close;
    }

    return 0;
}

wangcai::Price InfoLoader::loadOpenPrice(const std::string& csv_content, bool is_etf) {
    std::istringstream file(csv_content);
    
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
        std::string date, time, sym, prevclose_str, open_str;
        // 新格式: date,time,sym,prevclose,open,...
        std::getline(ss, date, ',');
        std::getline(ss, time, ',');
        std::getline(ss, sym, ',');
        std::getline(ss, prevclose_str, ',');
        std::getline(ss, open_str, ',');
        
        double open_raw = std::stod(open_str);
        // 先乘以10000，再四舍五入到tick（ETF=10, 股票=100）
        Price tick = get_tick(is_etf);
        double open_multiplied = open_raw * 10000;
        double open_rounded = std::round(open_multiplied / tick) * tick;
        wangcai::Price open_price = static_cast<wangcai::Price>(open_rounded);

        return open_price;
    }
    
    return 0;
}

// 扫描 CSV 第 4 列(price,元)取价格范围:新股无涨跌幅日的簿边界构造用
//（该日委托可含远离市价的申报,如 688327.SH 2022-06-01 出现 6790 元卖单,
// 簿边界必须覆盖全部申报价,否则 pxToIdx 越界）
std::pair<double, double> InfoLoader::scanPriceRange(
    const std::string& csv_a, const std::string& csv_b) {
    double lo = std::numeric_limits<double>::max(), hi = 0.0;
    auto scan = [&](const std::string& content) {
        std::istringstream file(content);
        std::string line;
        std::getline(file, line);  // 标题行
        while (std::getline(file, line)) {
            size_t p1 = line.find(',');
            if (p1 == std::string::npos) continue;
            size_t p2 = line.find(',', p1 + 1);
            if (p2 == std::string::npos) continue;
            size_t p3 = line.find(',', p2 + 1);
            if (p3 == std::string::npos) continue;
            size_t p4 = line.find(',', p3 + 1);
            try {
                double px = std::stod(line.substr(p3 + 1, p4 == std::string::npos
                                                    ? std::string::npos : p4 - p3 - 1));
                if (px > 0.0) {
                    if (px < lo) lo = px;
                    if (px > hi) hi = px;
                }
            } catch (const std::exception&) {
                continue;  // 价格列解析失败的行跳过（与主加载路径的容错一致）
            }
        }
    };
    scan(csv_a);
    scan(csv_b);
    if (hi <= 0.0) return {0.0, 0.0};
    return {lo, hi};
}

// 从csbar1d加载涨跌停限制
// is_etf: true=ETF（tick=10厘=0.001元）, false=股票（tick=100厘=0.01元）
std::pair<wangcai::Price, wangcai::Price> InfoLoader::loadPriceLimits(
    const std::string& csbar1d_csv_content, bool is_etf,
    double prev_close_yuan, double px_lo_yuan, double px_hi_yuan,
    double trade_lo_yuan, double trade_hi_yuan) {
    std::istringstream file(csbar1d_csv_content);
    std::string line;
    std::getline(file, line); // 跳过标题行
    
    double upper_limit_yuan = 0.0;
    double lower_limit_yuan = 0.0;
    
    if (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string field;
        int col = 0;
        
        // 列顺序: sym,prevclose,open,high,low,close,volume,turnover,tradecount,af,upperlimit,lowerlimit
        while (std::getline(ss, field, ',')) {
            if (col == 10) { // upperlimit
                upper_limit_yuan = std::stod(field);
            } else if (col == 11) { // lowerlimit
                lower_limit_yuan = std::stod(field);
            }
            col++;
        }
    }
    
    if (upper_limit_yuan == 0.0 || lower_limit_yuan == 0.0) {
        // 新股上市前 5 日(科创板/创业板)无涨跌幅限制,csbar1d 记 0:
        // 簿边界改用全天申报/成交价格范围(含前收)构造,此期间交易所不拒
        // 远离市价的申报,边界必须覆盖之(688327.SH 2022-06-01 实证:直接
        // throw 致引擎初始化即崩,孙进程 OOM 式连环退出)。
        double hi = px_hi_yuan, lo = px_lo_yuan;
        // 但申报价可以是恶作剧天价/地板价(301408.SZ 2023-03-01 上市首日
        // 出现 26033921 元卖单与 0.01 元买单,按申报全范围建簿需 26 亿档,
        // int 桶数溢出 + 上百 GB 分配,初始化必崩)。此类申报永不成交:
        //   - 连续竞价:界外单若成交,成交价必落在真实成交范围之外,矛盾;
        //   - 集合竞价:天价卖/地板买只延伸累计曲线的两端,不改变界内
        //     最大可成交量价位,清算价不变。
        // 故边界以全天成交价范围 ×kOutOfBookFactor 封顶,界外历史委托
        // 由 accept 段登记簿外价吸收表(不进簿),撤单凭表吸收为 no-op。
        if (trade_hi_yuan > 0.0) {
            hi = std::min(hi, trade_hi_yuan * kOutOfBookFactor);
        }
        if (trade_lo_yuan > 0.0 && lo > 0.0) {
            lo = std::max(lo, trade_lo_yuan / kOutOfBookFactor);
        }
        if (prev_close_yuan > 0.0) {           // 前收必在簿内(竞价基准)
            hi = std::max(hi, prev_close_yuan);
            lo = lo > 0.0 ? std::min(lo, prev_close_yuan) : prev_close_yuan;
        }
        if (hi <= 0.0) {
            throw std::runtime_error("无法从csbar1d读取涨跌停限制");
        }
        upper_limit_yuan = hi;
        lower_limit_yuan = lo > 0.0 ? lo : 0.0;
    }
    
    // 添加冗余（ETF=0.01元，股票=0.1元）
    double margin = is_etf ? 0.01 : 0.1;
    upper_limit_yuan += margin;
    lower_limit_yuan -= margin;
    // 簿下界不低于一个 tick(无涨跌幅日 min 价可能极小,减冗余后会 ≤0)
    const double min_tick_yuan = is_etf ? 0.001 : 0.01;
    if (lower_limit_yuan < min_tick_yuan) lower_limit_yuan = min_tick_yuan;
    
    // 获取 tick（ETF=10厘=0.001元，股票=100厘=0.01元）
    wangcai::Price tick = get_tick(is_etf);
    double tick_yuan = tick / 10000.0;  // 转换为元
    
    // 先四舍五入到 tick 精度
    upper_limit_yuan = std::round(upper_limit_yuan / tick_yuan) * tick_yuan;
    lower_limit_yuan = std::round(lower_limit_yuan / tick_yuan) * tick_yuan;
    
    // 转换为厘（1元=10000厘）
    wangcai::Price upper_limit = static_cast<wangcai::Price>(std::round(upper_limit_yuan * 10000.0));
    wangcai::Price lower_limit = static_cast<wangcai::Price>(std::round(lower_limit_yuan * 10000.0));
    
    // 确保对齐到tick
    upper_limit = ((upper_limit + tick - 1) / tick) * tick; // 向上对齐
    lower_limit = (lower_limit / tick) * tick;               // 向下对齐
    
    return {upper_limit, lower_limit};
}


};
