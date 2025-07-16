/*
 * matching_main.cpp
 * ------------------
 * 整体撮合驱动程序：
 *   1. 09:15:00 ~ <09:25:00   ——   CallAuctionEngine (集合竞价)
 *   2. 09:25:00 settle        ——   结算集合竞价，挂剩余单
 *   3. ≥09:25:00              ——   ConAuctionEngine (连续竞价)
 *
 * 核心思想：
 *   - 先把 ord / tra 全部读入一个 vector<Event> (带时间顺序)
 *   - 遍历 events 时，实时观察下一条事件时间；
 *     发现下一条 ≥09:25:00 且当前 still <09:25:00 时，
 *     在当前事件处理完后立即 CallAuctionEngine::settle()，
 *     然后切换到连续竞价模式。
 *
 * 注：insert_event() 不再做 09:25 之前的时间过滤，让大 list 拥有全时段事件。
 */

 #include "OrderLoader.h"

 #include <iostream>
 #include <iomanip>
 #include <cmath>

 // 工具函数：将价格字符串转为int64_t，*10000并四舍五入到100
inline int64_t parse_price(const std::string& price_str) {
    if (price_str.empty()) return 0;
    double price_raw = std::stod(price_str);
    double price_multiplied = price_raw * 10000;
    double price_rounded = std::round(price_multiplied / 100.0) * 100.0;
    return static_cast<int64_t>(price_rounded);
}

 using namespace wangcai_orderbook_cpp;
 // 事件结构体
 struct Event {
    std::string datetime, sym;
    int64_t price, size, side, ordertype, orderid;
    int64_t channelno, seqno, bizindex, bidorderid, askorderid, tradeid;
    std::string exectype, tradebsflag, source;
    uint64_t sort_key;
};
 // --- 工具：提取 "HH:MM:SS" 并与常量比较 -----------------------------------
 static inline std::string time_part(const std::string& dt)
 {
     return dt.substr(11, 8);  // "YYYY-MM-DD HH:MM:SS"
 }
 
 static constexpr const char* T_MARK = "09:25:00";    // 集合竞价截止
 
 // ---------------------------------------------------------------------------
 int main(int argc, char* argv[])
 {
     if (argc != 4) {
         std::cout << "用法: matching_main <sym> <date> <data_path>\n";
         return 1;
     }
 
     std::string sym  = argv[1];           // e.g. "000027.SZ"
     std::string date = argv[2];           // e.g. "2022-01-07"
     std::string path = argv[3];           // logs 目录
 
     // ----------- 构建文件名 & 读取前收 / 涨跌停 ------------------------------
     std::string cstick  = path + "/cstick_" + sym + "_" + date + ".csv";
     Price prev_close    = loadPrevClosePrice(cstick);
     if (prev_close == 0) {
         std::cerr << "读取前收盘价失败，退出" << std::endl;
         return 1;
     }

     double upper_raw = prev_close * 1.1 / 10000.0;
     double lower_raw = prev_close * 0.9 / 10000.0;
     double upper     = std::round(upper_raw * 100) / 100.0;
     double lower     = std::floor(lower_raw * 100) / 100.0;
     int upper_i      = parse_price(std::to_string(upper));
     int lower_i      = parse_price(std::to_string(lower));

     // ----------- 初始化订单簿 + 两个撮合引擎 -------------------------------
     OrderBook ob(upper_i, lower_i, false);
     ob.setPrevClosePrice(prev_close);
     ob.setExchange(sym.substr(sym.size() - 2));
 
     CallAuctionEngine call_engine(ob, prev_close, ob.getExchange());
     ConAuctionEngine  con_engine (ob);
 
     // ----------- 读入全部事件 --------------------------------------------
     clear_events();
     std::string ord_file = path + "/csord_" + sym + "_" + date + ".csv";
     std::string tra_file = path + "/cstra_" + sym + "_" + date + ".csv";
     load_orders_from_csv(ord_file, ob);      // 这些函数里会 insert_event()
     load_traders_from_csv(tra_file, ob);
 
     auto events = get_merged_events();       // 全部事件，已按时间排序
     std::cout << "总事件数: " << events.size() << std::endl;
 
     bool continuous = false;                // 是否已经进入连续竞价
 
     for (size_t i = 0; i < events.size(); ++i) {
         const Event& ev = events[i];
 
         // ---- 处理集合竞价 --> 连续竞价的切换逻辑 ---------------------------
         if (!continuous) {
             // 查下一条事件的时间（若有）
             std::string tm_cur = time_part(ev.datetime);
             std::string tm_nxt = (i + 1 < events.size()) ? time_part(events[i + 1].datetime) : "99:99:99";
 
             // *本条事件* 仍 <09:25:00，但 *下一条* >=09:25:00 -> 当前处理完后 settle()
             bool hit_cut = (tm_cur <  T_MARK) && (tm_nxt >= T_MARK);
 
             // ---------------- 处理当前事件 (集合竞价) ----------------------
             if (ev.source == "ord") { // 下单
                 Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                 OrderType  typ = OrderType::Limit; 
                 auto ord = ob.createOrder("BRK", "AC", ob.getExchange(), ev.sym, std::to_string(ev.orderid), typ,
                                             dir, ev.price, ev.size, ev.bizindex);
                 call_engine.accept(ord);
             } else { // 撤单
                 uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                 call_engine.cancel_by_input_id(oid_raw);
             }
 
             if (hit_cut) { // 集合竞价结束
                 call_engine.settle();
                 std::cout << "[09:25] 集合竞价完成价="<< call_engine.getPredictPrice() / 10000.0 << " 量=" << call_engine.getPredictVolume() << '\n';
                 continuous = true;
                 break;
             }
         } else {
             // ---------------- 连续竞价模式 --------------------------------
             if (ev.source == "ord") {
                 Direction dir = (ev.side == 1 ? Direction::Buy : Direction::Sell);
                 OrderType  typ = OrderType::Limit; // 简化示例
                 auto ord = ob.createOrder("BRK", "AC", ob.getExchange(), ev.sym, std::to_string(ev.orderid), typ,
                                             dir, ev.price, ev.size, ev.bizindex);
                 con_engine.accept(ord);
             } else {
                 uint64_t oid_raw = ev.bidorderid ? ev.bidorderid : ev.askorderid;
                 con_engine.cancel_by_input_id(oid_raw);
             }
         }
     }
 
     // ---------------- 收尾：打印一些信息 ----------------------------------
     std::cout << "集合竞价预测价: " << call_engine.getPredictPrice() / 10000.0
               << ", 成交量: "       << call_engine.getPredictVolume() << std::endl;
     std::cout << "连续竞价最优买: "  << ob.bestBid() / 10000.0
               << ", 最优卖: "       << ob.bestAsk() / 10000.0 << std::endl;
 
     return 0;
 }