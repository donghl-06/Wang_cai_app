#include <iostream>
#include "orderbook.h"
#include "call_auction_engine.hpp"

using namespace wangcai_orderbook_cpp;
using namespace std;

int main() {
    cout << "=== 简单价格对齐测试 ===" << endl;
    
    // 初始化订单簿
    Price prev_close = 60300; // 6.03元
    double upper = prev_close * 1.1 / 10000.0;  // 涨停价
    double lower = prev_close * 0.9 / 10000.0;  // 跌停价
    
    cout << "价格范围: " << lower << " - " << upper << " 元" << endl;
    cout << "对应整数: " << (int)(lower * 10000) << " - " << (int)(upper * 10000) << endl;
    
    OrderBook ob(upper, lower, false);
    ob.setPrevClosePrice(prev_close);
    ob.setExchange("SH");
    
    CallAuctionEngine auction_engine(ob, prev_close, "SH");
    
    // 测试几个价格
    vector<Price> test_prices = {60300, 60400, 60270, 62770, 54270};
    
    for (Price px : test_prices) {
        cout << "\n测试价格: " << px << " (" << px/10000.0 << " 元)" << endl;
        cout << "价格检查: 范围内=" << (px >= 54270 && px <= 66330 ? "是" : "否") << endl;
        cout << "对齐检查: (price - 54270) % 100 = " << (px - 54270) % 100 << endl;
        
        if (px >= 54270 && px <= 66330 && (px - 54270) % 100 == 0) {
            try {
                auto order = ob.createOrder(
                    "BROKER", "ACCOUNT", "SH", "600236.SH", to_string(px),
                    OrderType::Limit, Direction::Buy, px, 1000, 1
                );
                
                auction_engine.accept(order);
                cout << "订单创建成功!" << endl;
            } catch (const exception& e) {
                cout << "订单创建失败: " << e.what() << endl;
            }
        } else {
            cout << "跳过: 价格不符合要求" << endl;
        }
    }
    
    cout << "\n测试完成" << endl;
    return 0;
} 