#include "OrderLoader.h"
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

            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;

            // 解析数据（使用 order_book 的 tick 值）
            Price tick = order_book.getTick();
            int64_t price = parse_price(price_str, tick);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t side = side_str.empty() ? 0 : std::stoi(side_str);
            int64_t ordertype = ordertype_str.empty() ? 0 : std::stoi(ordertype_str);
            int64_t orderid = orderid_str.empty() ? 0 : std::stoi(orderid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t seqno = (seqno_str == "-9223372036854775808") ? 0 : std::stoi(seqno_str);
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
            OrderBook::insertEvent(order_event);
            inserted_events++;

            // 创建订单对象用于引擎处理
            auto order = order_book.createOrder(
                "EXCHANGE",  // broker
                "EXCHANGE", // account
                sym.substr(sym.size() - 2), // exchange (取代码后两位作为交易所)
                sym,              // instrument
                orderid_str,      // order_local_id (使用原始字符串)
                wangcai::OrderType::Limit, // 沪市全部为限价
                // 根据方向判断Direction
                side == 1 ? wangcai::Direction::Buy : wangcai::Direction::Sell,
                price,           // price
                size,            // volume
                bizindex
            );

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

            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;

            // 解析数据（使用 order_book 的 tick 值）
            Price tick = order_book.getTick();
            int64_t price = parse_price(price_str, tick);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t side = side_str.empty() ? 0 : std::stoi(side_str);
            int64_t ordertype = ordertype_str.empty() ? 0 : std::stoi(ordertype_str);
            int64_t orderid = orderid_str.empty() ? 0 : std::stoi(orderid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t seqno = (seqno_str == "-9223372036854775808") ? 0 : std::stoi(seqno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoll(bizindex_str);
            
            bool is_sz_mkt = (sym.size() >= 2 &&
                              sym.substr(sym.size() - 2) == "SZ" &&
                              ordertype != 2 && ordertype != 0);              // 非限价视为市价

            if (is_sz_mkt) {
                order_book.first_trade_px_[orderid] = 0;    // 只缓存，先不插事件
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
            OrderBook::insertEvent(order_event);

            // 创建订单对象用于引擎处理
            auto order = order_book.createOrder(
                "EXCHANGE",  // broker
                "EXCHANGE", // account
                sym.substr(sym.size() - 2), // exchange (取代码后两位作为交易所)
                sym,              // instrument
                orderid_str,      // order_local_id (使用原始字符串)
                // 根据委托价格类型和交易所判断OrderType
                (sym.substr(sym.size() - 2) == "SZ"
                    ? (ordertype == 1 ? wangcai::OrderType::Market
                        : (ordertype == 2 ? wangcai::OrderType::Limit
                            : (ordertype == 3 ? wangcai::OrderType::BestOwn
                                : wangcai::OrderType::Limit)))
                    : wangcai::OrderType::Limit), // 沪市全部为限价
                // 根据方向判断Direction
                side == 1 ? wangcai::Direction::Buy : wangcai::Direction::Sell,
                price,           // price
                size,            // volume
                bizindex
            );

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
            
            // 统一时间格式为 "YYYY-MM-DD HH:MM:SS.fff"
            std::string time_part = format_time_to_milliseconds(time);
            std::string datetime = date + " " + time_part;
            
            // 去除空格
            tradebsflag.erase(std::remove(tradebsflag.begin(), tradebsflag.end(), ' '), tradebsflag.end());

            // 使用 order_book 的 tick 值（根据 is_etf 设置：ETF=10，股票=100）
            Price tick = order_book.getTick();
            int64_t price = parse_price(price_str, tick);
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t bidorderid = bidorderid_str.empty() ? 0 : std::stoi(bidorderid_str);
            int64_t askorderid = askorderid_str.empty() ? 0 : std::stoi(askorderid_str);
            int64_t tradeid = tradeid_str.empty() ? 0 : std::stoi(tradeid_str);
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
                auto it = order_book.first_trade_px_.find(bidorderid);
                if (it != order_book.first_trade_px_.end()) {
                    if (it->second == 0 || price > it->second) {  // 买方：记录最高价
                        it->second = price;
                    }
                }
            }

            if (askorderid != 0 && askorderid != bidorderid) {     // 避免同 ID 重复写
                auto it = order_book.first_trade_px_.find(askorderid);
                if (it != order_book.first_trade_px_.end()) {
                    if ((it->second == 0 || price < it->second) && price != 0) {  // 卖方：记录最低价
                        it->second = price;
                    }
                }
            }

            // 兼容Python bytes格式：b'2' -> 2, b'1' -> 1
            if (exectype == "b'2'") exectype = "2";
            if (exectype == "b'1'") exectype = "1";

            Event trade_event(datetime, sym, price, size, -1, -1, tradeid, channelno, -1, bizindex, 
                            bidorderid, askorderid, tradeid, exectype, tradebsflag, 
                            -1, -1, -1, -1, -1, -1, -1, -1, {}, {}, {}, {}, -1, -1, -1, -1, -1,"tra",
                            -1,              // exchange 未知
                            0,               // trading_day 暂设0
                            0,               // action_day
                            "",             // status
                            '\0',            // order_kind
                            tradeid,         // trade_index
                            time_raw);       // time_raw
            // 创建并插入撤单事件
           if (exectype == "2") {   
                OrderBook::insertEvent(trade_event);
            } else if (exectype != "2" && isContinuousTime(datetime)) {
                continuous_trades_.emplace_back(std::move(trade_event));
            }
        } catch (const std::exception& e) {
            std::cerr << "处理撤单时发生错误: " << e.what() << std::endl;
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

            Event tick_event(datetime, sym, -1, -1, -1, -1, -1, -1, -1, -1, 
                            -1, -1, -1, "", "", 
                            prevclose, open, high, low, close, volume, turnover, tradecount, bids, bid_sizes, asks, ask_sizes, avgbid, avgask, totalbsize, totalasize, iopv, "tick" 
                            , -1, -1, -1, "", '\0', -1, time_raw);
                            
            OrderBook::insertTick(tick_event);
        } catch (const std::exception& e) {
            std::cerr << "处理Tick时发生错误: " << e.what() << std::endl;
        }
    }
}

// 清空事件列表
void InfoLoader::clear_events() {
    OrderBook::clearEvents();
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

// 从csbar1d加载涨跌停限制
// is_etf: true=ETF（tick=10厘=0.001元）, false=股票（tick=100厘=0.01元）
std::pair<wangcai::Price, wangcai::Price> InfoLoader::loadPriceLimits(const std::string& csbar1d_csv_content, bool is_etf) {
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
        throw std::runtime_error("无法从csbar1d读取涨跌停限制");
    }
    
    // 添加冗余（ETF=0.01元，股票=0.1元）
    double margin = is_etf ? 0.01 : 0.1;
    upper_limit_yuan += margin;
    lower_limit_yuan -= margin;
    
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
