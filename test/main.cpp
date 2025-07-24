#include "../include/orderbook.h"
#include "../include/call_auction_engine.hpp"
#include "../include/con_auction_engine.hpp"
#include <iostream>
#include <iomanip>

using namespace wangcai_orderbook_cpp;

/* --------- 成交回调：打印 Execution --------- */
void onExec(const Execution& ex) {
    std::cout << std::fixed << std::setprecision(4)
              << "[成交] id="   << ex.execution_id
              << " 价格="      << ex.price / 10000.0
              << " 数量="      << ex.volume
              << " 买方="      << ex.buy_order_id
              << " 卖方="      << ex.sell_order_id
              << '\n';
}

/* --------- 集合竞价预测价回调 --------- */
void onPricePredict(Price px, Quantity vol) {
    std::cout << std::fixed << std::setprecision(4)
              << "[预测] 价格=" << px / 10000.0
              << " 数量=" << vol << '\n';
}

int main()
{
    std::cout << "=== 订单簿撮合引擎测试 ===" << '\n';
    
    /* 1) 构造单股票订单簿（4.7 ~ 6.0 元涨跌停） */
    OrderBook ob(/*hi*/6.0, /*lo*/4.7, /*ETF?*/false, onExec);
    
    /* 2) 集合竞价阶段 (09:15-09:25) */
    std::cout << "\n--- 集合竞价阶段 ---" << '\n';
    Price prev_close = 50000; // 前收盘价 5.0000 元
    CallAuctionEngine call_engine(ob, prev_close, "SH", onPricePredict);
    
    // 模拟集合竞价期间的订单
    std::cout << "添加集合竞价订单..." << '\n';
    
    // 买单（价格从低到高）
    auto buy1 = ob.createOrder("券商1","账户1","SH","600000.SH","B001",
                               OrderType::Limit, Direction::Buy, 49800, 500, 1);  // 4.98元
    call_engine.accept(buy1);
    
    auto buy2 = ob.createOrder("券商2","账户2","SH","600000.SH","B002", 
                               OrderType::Limit, Direction::Buy, 50000, 800, 2);  // 5.00元
    call_engine.accept(buy2);
    
    auto buy3 = ob.createOrder("券商3","账户3","SH","600000.SH","B003",
                               OrderType::Limit, Direction::Buy, 50200, 1000, 3); // 5.02元
    call_engine.accept(buy3);
    
    // 卖单（价格从高到低）
    auto sell1 = ob.createOrder("券商4","账户4","SH","600000.SH","S001",
                                OrderType::Limit, Direction::Sell, 50300, 600, 4); // 5.03元
    call_engine.accept(sell1);
    
    auto sell2 = ob.createOrder("券商5","账户5","SH","600000.SH","S002",
                                OrderType::Limit, Direction::Sell, 50100, 700, 5); // 5.01元
    call_engine.accept(sell2);
    
    auto sell3 = ob.createOrder("券商6","账户6","SH","600000.SH","S003",
                                OrderType::Limit, Direction::Sell, 49900, 400, 6); // 4.99元
    call_engine.accept(sell3);
    
    std::cout << "\n测试撤单..." << '\n';
    call_engine.cancel(2); // 撤销买单B002
    
    std::cout << "\n09:25 集合竞价结算..." << '\n';
    call_engine.settle();
    
    std::cout << "结算后最优价位: 买=" << ob.bestBid()/10000.0 
              << " 卖=" << ob.bestAsk()/10000.0 << '\n';
    
    /* 3) 连续竞价阶段 (09:30-) */
    std::cout << "\n--- 连续竞价阶段 ---" << '\n';
    // 使用与集合竞价相同的市场类型（SH）
    ConAuctionEngine con_engine(ob, MarketType::SH);
    
    // 新的限价单
    std::cout << "添加新限价买单..." << '\n';
    auto new_buy = ob.createOrder("券商7","账户7","SH","600000.SH","B004",
                                  OrderType::Limit, Direction::Buy, 50200, 300, 7);
    con_engine.accept(new_buy);
    
    std::cout << "当前最优价位: 买=" << ob.bestBid()/10000.0 
              << " 卖=" << ob.bestAsk()/10000.0 << '\n';
    
    // 市价卖单，直接成交 - 修复：使用涨停价作为市价单价格
    std::cout << "\n添加市价卖单（吃买盘）..." << '\n';
    auto market_sell = ob.createOrder("券商8","账户8","SH","600000.SH","M001",
                                      OrderType::Market, Direction::Sell, 47000, 400, 8); // 使用跌停价
    con_engine.accept(market_sell);
    
    std::cout << "成交后最优价位: 买=" << ob.bestBid()/10000.0 
              << " 卖=" << ob.bestAsk()/10000.0 << '\n';
    
    // 测试撤单
    std::cout << "\n测试连续竞价撤单..." << '\n';
    bool cancel_ok = con_engine.cancel(7); // 撤销订单7
    std::cout << "撤单结果: " << (cancel_ok ? "成功" : "失败") << '\n';
    
    // 跨价位的大单
    std::cout << "\n添加跨价位大买单..." << '\n';
    auto big_buy = ob.createOrder("券商9","账户9","SH","600000.SH","B005",
                                  OrderType::Limit, Direction::Buy, 51000, 2000, 9);
    con_engine.accept(big_buy);
    
    std::cout << "最终最优价位: 买=" << ob.bestBid()/10000.0 
              << " 卖=" << ob.bestAsk()/10000.0 << '\n';
    
    std::cout << "\n=== 测试完成 ===" << '\n';
    return 0;
}