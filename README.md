# 使用指南（User README）

> 本文面向**策略开发者 / 回测使用者**，介绍如何编译、运行以及编写自定义策略。

## 1. 环境准备
```
# 依赖
C++17           # 编译标准
CMake ≥ 3.16    # 构建工具
Boost           # 仅用到 pool 组件
```
Ubuntu 示例：
```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake libboost-all-dev
```

## 2. 代码编译
```bash
git clone https://github.com/yourname/wangcai_orderbook_cpp.git
cd wangcai_orderbook_cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```
生成：
```
libwangcai_orderbook_cpp.a   # 静态库
backtest_main                # 示例 CLI
```

## 3. 数据文件格式
框架默认读取三类 CSV（列顺序请保持一致）：

| 文件 | 作用 | 命名示例 |
| ---- | ---- | -------- |
| `csord_<sym>_<date>.csv` | 集合竞价/连续竞价阶段**委托** | `csord_000027.SZ_2022-01-07.csv` |
| `cstra_<sym>_<date>.csv` | 成交 & 撤单 | `cstra_000027.SZ_2022-01-07.csv` |
| `cstick_<sym>_<date>.csv`| K 线，用于获取昨收/开盘 | `cstick_000027.SZ_2022-01-07.csv` |

各字段含义详见 `include/OrderLoader.h` 头部注释。

## 4. 运行回测
```bash
./backtest_main <symbol> <date> <data_path>
# 例：
./backtest_main 000027.SZ 2022-01-07 ../logs
```
启动后将：
1. 加载 3 份 CSV → `OrderBook::whole_events`
2. 调用 `BacktestEngine::run()` 驱动全流程
3. 默认输出：
   - 关键日志到 stdout
   - 交易记录 `backtest_trades_<sym>_<date>.csv`

### CLI 日志示例
```
[09:25] 集合竞价完成，开盘价=7.98 ...
[14:57:02] 进入收盘集合竞价阶段
[收盘集合竞价] 成交价=7.97 成交量=125500
回测完成
```

## 5. 编写自定义策略
### 5.1 实现接口
```cpp
// example_strategy.hpp
#include "backtest_engine.hpp"

class MeanReversionStrategy : public wangcai_orderbook_cpp::Strategy {
public:
    explicit MeanReversionStrategy(const std::string& id,double thr)
        : id_(id), threshold_(thr) {}

    std::vector<UserOrder> onMarketData(const MarketData& data) override {
        std::vector<UserOrder> ords;
        // 简例：若价差超阈值，下单买入 100 股
        if(data.best_ask - data.best_bid > threshold_ * 10000) {
            ords.push_back({
                std::to_string(seq_++), data.symbol,
                Direction::Buy, OrderType::Limit,
                data.best_bid, 100, id_});
        }
        return ords;
    }
    void onOrderFilled(const std::string&,Price,Quantity) override {}
    void onOrderCancelled(const std::string&,const std::string&) override {}
    std::string getStrategyId() const override {return id_;}
private:
    std::string id_;
    double threshold_;
    uint64_t seq_{1};
};
```

### 5.2 注册策略
```cpp
// backtest_main.cpp（片段）
auto strategy1 = std::make_shared<MeanReversionStrategy>("MR1",0.01);
engine.registerStrategy(strategy1);
```

重新编译并运行即可在回测中执行您的策略。

## 6. 结果解释
- **trade CSV** 格式与沪深交易所 `cstra` 一致，便于后续风控 / 监控工具复用
- **回测日志** 输出预测 vs 实际开盘价、收盘价，以及策略盈亏

## 7. 常见问题
| 问题 | 解决方案 |
| ---- | --------- |
| 报 `price out of limit` | 检查订单价格是否超涨跌停范围 & 是否对齐 0.01 元 |
| 委托号冲突 | 自定义策略请保证 `order_id` 唯一，可直接用 `std::to_string(counter++)` |
| 想要实时可视化盘口 | 可在 `market_data_callback_` 里推送到外部 WebSocket/UI |

---
Enjoy back-testing 🚀 