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

// 加载订单数据
void load_orders_from_csv(const std::string& csv_file, wangcai_orderbook_cpp::OrderBook& order_book) {
    // std::ifstream file(csv_file);
    std::istringstream file(csv_file);
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

            // 解析价格和数量
            int64_t price = price_str.empty() ? 0 : static_cast<int64_t>(std::round(std::stod(price_str) * 10000));
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t side = side_str.empty() ? 0 : std::stoi(side_str);
            int64_t ordertype = ordertype_str.empty() ? 0 : std::stoi(ordertype_str);
            int64_t orderid = orderid_str.empty() ? 0 : std::stoi(orderid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t seqno = (seqno_str == "-9223372036854775808") ? 0 : std::stoi(seqno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoi(bizindex_str);
            
            // 创建订单对象
            auto order = order_book.createOrder(
                "EXCHANGE",  // broker
                "EXCHANGE", // account
                sym.substr(sym.size() - 2), // exchange (取代码后两位作为交易所)
                sym,              // instrument
                orderid_str,      // order_local_id (使用原始字符串)
                // 根据委托价格类型和交易所判断OrderType
                // 委托价格类型，沪深两市不同：
                // 沪市：无委托价格类型，均为0。
                // 深市：1为市价；2为限价；3为本方最优(原标识为U)。
                (sym.substr(sym.size() - 2) == "SZ"
                    ? (ordertype == 1 ? wangcai_orderbook_cpp::OrderType::Market
                        : (ordertype == 2 ? wangcai_orderbook_cpp::OrderType::Limit
                            : (ordertype == 3 ? wangcai_orderbook_cpp::OrderType::BestOwn
                                : wangcai_orderbook_cpp::OrderType::Limit)))
                    : wangcai_orderbook_cpp::OrderType::Limit), // 沪市全部为限价
                // 根据方向判断Direction
                side == 1 ? wangcai_orderbook_cpp::Direction::Buy : wangcai_orderbook_cpp::Direction::Sell, // direction
                price,           // price
                size,            // volume
                bizindex
            );

            // 只处理时间在09:25:00之前的订单
        //     if (datetime.substr(11) < "09:25:00") {
        //         order_book.add_order(price, side, static_cast<int64_t>(size), datetime, sym, ordertype, orderid, channelno, seqno, bizindex, -1, -1);
        //         wangcai_orderbook_cpp::Order order(datetime, sym, price, size, side, ordertype, orderid, channelno, seqno, bizindex, -1, -1, -1, "", "ord");
        //     }
        //     if (datetime.substr(11) >= "09:30:00" && datetime.substr(11) < "14:57:00") {
        //         //datetime,sym,price,size,side,ordertype,orderid,channelno,seqno,bizindex,bidorderid,askorderid,tradeid,exectype,tradebsflag
        //         //Order order(datetime, sym, price, size, side, ordertype, orderid, channelno, seqno, bizindex, bidorderid, askorderid, exectype, tradebsflag, source);
        //         wangcai_orderbook_cpp::Order order(datetime, sym, price, size, side, ordertype, orderid, channelno, seqno, bizindex, -1, -1, -1, "", "ord");
        //         order_book.add_to_tra_ord_list(order);
        //     }
        } catch (const std::exception& e) {
            std::cerr << "处理订单时发生错误: " << e.what() << std::endl;
        }
    }
}

// 加载撤单数据
void load_traders_from_csv(const std::string& csv_file, wangcai_orderbook_cpp::OrderBook& order_book) {
    // std::ifstream file(csv_file);
    std::istringstream file(csv_file);
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    while (std::getline(file, line)) {
        try {
            std::istringstream ss(line);
            std::string datetime, sym, exectype, tradebsflag, channelno_str, bizindex_str, price_str, size_str, bidorderid_str, askorderid_str, tradeid_str;

            // 解析CSV行
            std::getline(ss, datetime, ','); // 第二个datetime
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

            int64_t price = price_str.empty() ? 0 : static_cast<int64_t>(std::round(std::stod(price_str) * 10000));
            int64_t size = size_str.empty() ? 0 : std::stod(size_str);
            int64_t bidorderid = bidorderid_str.empty() ? 0 : std::stoi(bidorderid_str);
            int64_t askorderid = askorderid_str.empty() ? 0 : std::stoi(askorderid_str);
            int64_t tradeid = tradeid_str.empty() ? 0 : std::stoi(tradeid_str);
            int64_t channelno = channelno_str.empty() ? 0 : std::stoi(channelno_str);
            int64_t bizindex = (bizindex_str == "-9223372036854775808") ? 0 : std::stoi(bizindex_str);

            // // 只处理时间在09:25:00之前的撤单
            // if (datetime.substr(11) < "09:25:00" && exectype == "2") {
            //     wangcai_orderbook_cpp::Order trade(price, tradebsflag == "B" ? 1 : -1, static_cast<int64_t>(size), datetime, sym, 2, bidorderid, 0, 0, 0, bidorderid, askorderid);
            //     // std::cout << "Trade: " << "价格: " << trade.price << ", 方向: " << (trade.side == 1 ? "买入" : "卖出") << ", 数量: " << trade.size << ", 时间: " << trade.datetime << ", 类型: " << trade.ordertype << ", 买单ID: " << trade.bidorderid << ", 卖单ID: " << trade.askorderid << std::endl;
            //     order_book.delete_order(trade);

            // }
            // if (datetime.substr(11) >= "09:30:00" && datetime.substr(11) < "14:57:00") {
            //     //datetime,sym,price,size,side,ordertype,orderid,channelno,seqno,bizindex,bidorderid,askorderid,tradeid,exectype,tradebsflag
            //     //Order ord(datetime, sym, price, size, side, ordertype, orderid, channelno, seqno, bizindex, bidorderid, askorderid, exectype, tradebsflag, source);
            //     wangcai_orderbook_cpp::Order trade(datetime, sym, price, size, -1, -1, tradeid, channelno, -1, bizindex, bidorderid, askorderid, std::stoi(exectype), tradebsflag, "tra");
            //     order_book.add_to_tra_ord_list(trade);
            // }
        } catch (const std::exception& e) {
            std::cerr << "处理撤单时发生错误: " << e.what() << std::endl;
        }
    }
}

// 加载cstick用来获取开盘价
void load_cstick_from_csv(const std::string& csv_file, wangcai_orderbook_cpp::OrderBook& order_book) {
    // std::ifstream file(csv_file);
    std::istringstream file(csv_file);
    std::string line;
    std::getline(file, line); // 跳过CSV文件的标题行

    std::string last_line;
    while (std::getline(file, line)) {
        last_line = line;
    }

    try {
        ////datetime,sym,prevclose,open,high,low,close,volume,turnover,tradecount,bid1,bsize1,bid2,bsize2,bid3,bsize3,bid4,bsize4,bid5,bsize5,bid6,bsize6,bid7,bsize7,bid8,bsize8,bid9,bsize9,bid10,bsize10,ask1,asize1,ask2,asize2,ask3,asize3,ask4,asize4,ask5,asize5,ask6,asize6,ask7,asize7,ask8,asize8,ask9,asize9,ask10,asize10,avgbid,avgask,totalbsize,totalasize,iopv
        std::istringstream ss(last_line);
        std::string datetime, sym, prevclose_str;

        // 解析CSV行
        std::getline(ss, datetime, ',');
        std::getline(ss, sym, ',');
        std::getline(ss, prevclose_str, ',');

        // 去除空格
        sym.erase(std::remove(sym.begin(), sym.end(), ' '), sym.end());

        // // 将字符串转换为数值
        // int64_t prevclose = prevclose_str.empty() ? 0 : static_cast<int64_t>(std::round(std::stod(prevclose_str) * 10000));
        // order_book.update_prevclose(prevclose);
        // // 更新exchange为sym的最后两个字符
        // order_book.update_exchange(sym.substr(sym.size() - 2));
        // // std::cout << "prevclose: " << prevclose / 10000.0 << "， 股票号: " << sym << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "处理cstick时发生错误: " << e.what() << std::endl;
    }
}
// void get_snapshot(wangcai_orderbook_cpp::OrderBook& order_book, const std::string& cstick_file, const std::string& orders_file, const std::string& traders_file) {
//     // 读取csv文件 获取prevclose,处理撤单

//     load_cstick_from_csv(cstick_file, order_book);
//     load_orders_from_csv(orders_file, order_book);
//     load_traders_from_csv(traders_file, order_book);
//     // 计算集合竞价成交价格
//     order_book.cal_auction_trade();
//     // 输出集合竞价结果
//     order_book.print_auction_result();
//     // 应用集合竞价成交结果
//     order_book.apply_auction_trade();
//     // 打印Oder加trade队列
//     order_book.print_order_trade_list();
   
//     // 处理连续竞价
//     order_book.process_continuous_auction();
// }
