#include "backtest_engine.hpp"
#include "example_strategy.hpp"
#include <iostream>
#include <memory>

using namespace wangcai;

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
        
        // 创建并注册专门的测试策略
        auto test_strategy = std::make_shared<TestCallbackStrategy>("回调测试策略");

        // 创建并注册同步跟单策略
        // auto sync_strategy = std::make_shared<SyncFollowStrategy>("同步跟单策略", 1000, 100, 100);
        // auto mean_strategy = std::make_shared<MeanReversionStrategy>("均值回归策略", 0.01);

        // engine.registerStrategy(sync_strategy);
        // engine.registerStrategy(mean_strategy);
        
        engine.registerStrategy(test_strategy);
        
        std::cout << "已注册测试策略：" << std::endl;
        std::cout << "1. " << test_strategy->getStrategyId() << " - 专门测试成交回报和撤单回报" << std::endl;
        // std::cout << "2. " << sync_strategy->getStrategyId() << " - 同步跟单策略" << std::endl;
        // std::cout << "3. " << mean_strategy->getStrategyId() << " - 均值回归策略" << std::endl;
        
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
        
        // 打印测试策略统计信息
        std::cout << "\n=== 测试策略统计信息 ===" << std::endl;
        if (auto test_ptr = std::dynamic_pointer_cast<TestCallbackStrategy>(test_strategy)) {
            test_ptr->printStatistics();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "回测失败: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 