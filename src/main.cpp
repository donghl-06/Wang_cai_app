#include "../include/orderbook.h"
#include <iostream>
#include <iomanip>

using namespace wangcai_orderbook_cpp;

/* --------- 简单成交回调：打印 Execution --------- */
void onExec(const Execution& ex) {
    std::cout << std::fixed << std::setprecision(4)
              << "[EXEC] id="   << ex.execution_id
              << " px="        << ex.price / 10000.0
              << " qty="       << ex.volume
              << " buy="       << ex.buy_order_id
              << " sell="      << ex.sell_order_id
              << '\n';
}

int main()
{
    /* 1) 构造单股票订单簿（4.7 ~ 6.0 元涨跌停） */
    OrderBook ob(/*hi*/6.0, /*lo*/4.7, /*ETF?*/false, onExec);

    /* 2) 初始挂单：卖 5.0000 / 买 4.9000 */
    ob.addOrder(ob.createOrder("B1","ACC","SH","600000.SH","S1",
                               OrderType::Limit,Direction::Sell, 50000,1000, 0));
    ob.addOrder(ob.createOrder("B2","ACC","SH","600000.SH","B1",
                               OrderType::Limit,Direction::Buy , 49000, 800, 0));
    std::cout << "snap#1 ask=" << ob.bestAsk()/10000.0
              << " bid=" << ob.bestBid()/10000.0 << '\n';

    /* 3) 再挂一档更便宜的卖单：4.9800 */
    ob.addOrder(ob.createOrder("B3","ACC","SH","600000.SH","S2",
                               OrderType::Limit,Direction::Sell, 49800,500, 0));
    std::cout << "snap#2 ask=" << ob.bestAsk()/10000.0
              << " bid=" << ob.bestBid()/10000.0 << '\n';

    /* 4) 买单 5.0500 吃光 4.98/5.00 桶（部分成交） */
    ob.addOrder(ob.createOrder("B4","ACC","SH","600000.SH","B2",
                               OrderType::Limit,Direction::Buy , 50500,1400, 0));
    std::cout << "snap#3 ask=" << ob.bestAsk()/10000.0
              << " bid=" << ob.bestBid()/10000.0 << '\n';

    /* 5) 市价卖单 500 股：应直接成交到 4.9000，剩余撤销 */
    ob.addOrder(ob.createOrder("B5","ACC","SH","600000.SH","M1",
                               OrderType::Market,Direction::Sell, 0, 500, 0));
    std::cout << "snap#4 ask=" << ob.bestAsk()/10000.0
              << " bid=" << ob.bestBid()/10000.0 << '\n';

    /* 6) 撤单成功：撤掉剩余买单 B2（order_id = 2） */
    bool ok = ob.cancel(2);
    std::cout << "cancel(2) -> " << (ok?"ok":"fail") << '\n';

    /* 7) 再撤一次（应失败） */
    ok = ob.cancel(2);
    std::cout << "cancel(2) again -> " << (ok?"ok":"fail") << '\n';

    /* 8) 异常示范：价格越界 */
    try {
        ob.addOrder(ob.createOrder("B6","ACC","SH","600000.SH","X1",
                                   OrderType::Limit,Direction::Buy, 30000,100, 0));
    } catch (const std::exception& ex) {
        std::cout << "[EXCEPT] " << ex.what() << '\n';
    }

    /* 最终快照 */
    std::cout << "snap#final ask=" << ob.bestAsk()/10000.0
              << " bid=" << ob.bestBid()/10000.0 << '\n';
}