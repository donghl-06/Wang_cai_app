# README_USAGE.md

> **项目名称**：`wangcai_orderbook_cpp`  
> **目的**：教你最快速跑起来、下策略单、拿到成交/预测价记录。  
> **不包含**：架构、算法细节，这些请看 `README_DESIGN.md`。

---

## 1. 环境与依赖
- C++17 编译器（g++ 9+/clang++ 10+/MSVC 2019+）
- CMake 3.15+
- Boost (仅需 `boost::object_pool`)

```bash
sudo apt install g++ cmake libboost-dev   # Ubuntu 示例
```

## 2. 获取与编译
```bash
git clone <your_repo_url>/wangcai_orderbook_cpp.git
cd wangcai_orderbook_cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```
生成的可执行文件一般为 `backtest_main`。

## 3. 准备数据
放在同一目录（例：`../data`）：
```
csord_<symbol>_<date>.csv   # 订单
cstra_<symbol>_<date>.csv   # 成交/撤单 (exectype=2 撤单)
cstick_<symbol>_<date>.csv  # 前收/开盘价
```
示例：`csord_000001SZ_2025-07-08.csv`

## 4. 一行命令跑回测
```bash
./backtest_main 000001SZ 2025-07-08 ../data
```
输出内容包括：
- 集合竞价完成信息（09:25 与 15:30）
- 策略持仓与总盈亏
- 生成的成交/撤单文件：`backtest_trades_<symbol>_<date>.csv`

## 5. 注册策略（最小例）
```cpp
auto s = std::make_shared<MeanReversionStrategy>("MR_1", 0.02);
engine.registerStrategy(s);
```
在 `onMarketData` 里返回 `std::vector<UserOrder>` 即可下单。  
集合竞价阶段默认不允许下单，若要允许，修改 `processUserOrder` 逻辑。

## 6. 预测价回调（集合竞价阶段“流式预测”）
构造 `CallAuctionEngine` 时传 `PxCallback`：
```cpp
call_engine_ = std::make_unique<CallAuctionEngine>(
    *orderbook_, prev_close_, exch,
    [this](Price px, Quantity vol){
        MarketData md;
        md.datetime   = current_datetime_;
        md.symbol     = symbol_;
        md.best_bid   = orderbook_->bestBid();
        md.best_ask   = orderbook_->bestAsk();
        md.last_price = px;
        md.last_volume= vol;
        md.event_type = "predict";
        market_data_queue_.push(md);
        if (market_data_callback_) market_data_callback_(md);
    },
    cancel_cb
);
```
这样每接/撤单一次就会自动发布最新预测价。

## 7. 启用成交/撤单记录
```cpp
engine.enableTradeRecording(data_path + "/backtest_trades_" + symbol + "_" + date + ".csv");
```
回测结束自动写 CSV：  
```
datetime,sym,price,size,bidorderid,askorderid,tradeid,exectype,tradebsflag,channelno,bizindex
```
`exectype=1` 成交，`2` 撤单。

## 8. 常见问题
- **价格精度**：内部单位是“厘”(Price=uint64_t)，tick=100 表示 0.01 元。  
- **市价转换**：深市市价/本方最优单会转成“保护价限价单”。  
- **时间切换点**：09:25/14:57/15:30，用字符串比较实现切换；需要自定义时修改 `run()` 中的逻辑判断。

更多细节请阅读 `README_DESIGN.md`。
