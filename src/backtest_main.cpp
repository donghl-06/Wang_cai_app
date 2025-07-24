#include "backtest_engine.hpp"
#include "example_strategy.hpp"
#include <iostream>
#include <memory>

using namespace wangcai_orderbook_cpp;

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cout << "用法: backtest_main <symbol> <date> <data_path>" << std::endl;
        return 1;
    }
    
    std::string symbol = argv[1];
    std::string date = argv[2];
    std::string data_path = argv[3];
    
    try {
        // 创建回测引擎
        BacktestEngine engine(symbol, date, data_path);
        
        // 启用交易记录功能，输出到指定文件
        std::string trade_output_file = data_path + "/backtest_trades_" + symbol + "_" + date + ".csv";
        engine.enableTradeRecording(trade_output_file);
        
        // 设置市场数据回调（可选，用于调试）
        engine.setMarketDataCallback([](const MarketData& data) {
            // if (data.event_type == "trade") {
            //     std::cout << "[市场数据] " << data.datetime << " 成交: " 
            //              << data.last_price / 10000.0 << " 数量: " << data.last_volume << std::endl;
            // }
        });
        
        // 创建并注册策略
        // auto strategy1 = std::make_shared<MeanReversionStrategy>("均值回归策略1", 0.01);
        // auto strategy2 = std::make_shared<MeanReversionStrategy>("均值回归策略2", 0.02);
        
        // engine.registerStrategy(strategy1);
        // engine.registerStrategy(strategy2);
        
        // 运行回测
        engine.run();
        
        // 输出结果
        auto positions = engine.getPositions();
        double total_pnl = engine.getTotalPnL();
        
        std::cout << "\n最终盈亏: " << total_pnl << " 元" << std::endl;
        
        // 交易记录统计信息
        const auto& trades = engine.getTradeRecords();
        std::cout << "回测期间总成交笔数: " << trades.size() << std::endl;
        if (!trades.empty()) {
            std::cout << "首笔成交时间: " << trades.front().datetime << std::endl;
            std::cout << "末笔成交时间: " << trades.back().datetime << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "回测失败: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 