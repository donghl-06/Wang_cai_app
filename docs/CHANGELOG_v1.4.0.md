# 1.4.0 更新：实时合成 Tick（onRealTimeTickEvent）

## 1. 功能总结

本版本新增**实时合成 Tick**能力：真实 Tick 快照约 3 秒一条，开启本功能后，
引擎按用户指定的毫秒间隔（如 10/50/100ms），从内部维护的订单簿合成十档快照，
通过新回调 `onRealTimeTickEvent` 推送给策略，可以在两条真实 Tick 之间以更细
的粒度观察盘口。

- **新增可选开关**：`realtime_tick_interval_ms`（默认 0=关闭，需显式传入间隔才启用）
- **新增策略回调**：`onRealTimeTickEvent(snapshot)`，快照类型与 `onTickEvent` 完全相同
- **快照字段结构与真实 3 秒 Tick 一致**：十档价量、最新价、涨跌停、累计成交等
- **原有接口零影响**：未开启时行为与旧版完全一致；未实现该回调的老策略无需任何修改

---

## 2. 使用方法

```python
from wangcai_syn import run_backtest

run_backtest(
    data_dict=data,
    strategy=my_strategy,
    realtime_tick_interval_ms=100,   # 每 100ms 最多推送一次；10/50/100 均可
)
```

在策略中实现新回调（与 `onTickEvent` 一样可返回下单/撤单事件）：

```python
def onRealTimeTickEvent(self, snapshot):
    bid1, ask1 = snapshot.bids[0], snapshot.asks[0]
    bid1_vol = snapshot.bid_sizes[0]
    last = snapshot.last_price
    return []   # 或返回 [make_order(...), make_cancel(...)]
```

底层接口（不经 `run_backtest` 时）：

```python
engine = MultiBacktestEngine(symbol_data_list)
engine.setRealTimeTickInterval(100)   # 0=关闭；扇出到所有子引擎
```

---

## 3. 触发语义

- **网格对齐**：按"当日毫秒 ÷ 间隔"划分网格，推送落点确定、可复现
  （如 100ms 间隔的推送落在 09:30:00.000 / .100 / .200 …所在网格）。
- **每个网格最多推一次**：跨过网格边界后的第一条市场事件处理完成后推送，
  快照反映该事件处理后的盘口。
- **回测时间只随市场事件前进**：无事件的空白区间（如午休）不补发；
  同一网格内的后续事件不再重复推送。
- 推送范围与真实 Tick 一致：09:15:00 起，15:00:00 止。
- 返回的用户事件与 `onTickEvent` 走同一条分发链路，支持跨标的路由。

---

## 4. 字段口径

| 字段 | 来源 |
|------|------|
| 十档 `bids/asks/bid_sizes/ask_sizes` | 内部订单簿实时状态（只含历史订单，不含用户虚拟单） |
| `last_price` / `UpperLimit` / `LowerLimit` / `PreClose` | 订单簿 |
| `TotalBidVol` / `TotalAskVol` | 订单簿全簿挂单总量（增量维护，O(1)） |
| `Volume` / `Turnover` / `NumTrades` / `High` / `Low` | 引擎按重建的历史成交逐笔累计，单调不减 |
| `Open` / `Close` / `Iopv` / `Status` 等 | 承接最近一条真实 Tick |
| `AuctionPrice` / `AuctionQty` | 集合竞价阶段填预测价/预测量 |
| `Time` / `datetime` | 当前事件时间（`HHMMSSsss` 整数 / 完整时间字符串） |

注意：

- 累计字段是**引擎重建口径**，覆盖连续竞价撮合与开/收盘集合竞价 settle；
  与官方 3 秒快照的数值出入来自时间戳口径不同（官方 Tick 有独立时间戳），
  属正常现象，引擎不做对齐。合成 Tick 内部各字段彼此严格自洽。
  实测全天累计偏差约 1%-2%（收盘竞价成交发生在最后一次推送之后，不计入）。
- 集合竞价阶段（09:15-09:25、14:57-15:00）盘口是未交叉的原始挂单分布，
  买一可能高于卖一，与连续竞价的十档语义不同。
- 10ms 间隔全天推送量可达数十万次，回调内请只做轻量计算。
  未实现该回调的策略经一次性探测后完全跳过，无逐次开销。

---

## 5. 新增底层接口

| 接口 | 说明 |
|------|------|
| `Strategy.onRealTimeTickEvent(snapshot)` | 新回调，默认实现返回空列表 |
| `BacktestEngine.setRealTimeTickInterval(ms)` / `getRealTimeTickInterval()` | 单引擎开关 |
| `MultiBacktestEngine.setRealTimeTickInterval(ms)` / `getRealTimeTickInterval()` | 扇出到所有子引擎 |
| `OrderBook::fillDepth(px, sz, is_buy)` | 沿非空桶链表零分配填充前 10 档 |
| `OrderBook::getTotalBidVol()` / `getTotalAskVol()` | 全簿挂单总量 |
| `OrderBook::getUpperLimit()` / `getLowerLimit()` / `getPrevClose()` | 只读价格信息 |

---

## 6. 兼容性

- `realtime_tick_interval_ms` 默认 0：不开启时不进入任何新逻辑，
  回测结果与 1.3.9 逐字节一致（有开关等价性测试保证）。
- `Snapshot` 结构未改动；`onRealTimeTickEvent` 为非纯虚/带默认实现，
  现存 C++ 与 Python 策略无需修改即可升级。

---

## 7. 测试

新增 `tests/realtime_tick/`（8 项）：

- 默认关闭时回调次数为 0；开启（空回调）不改变下单/成交/撤单结果
- 合成盘口逐字段手算断言：十档价量、last_price、Volume/Turnover/NumTrades、
  TotalBidVol/TotalAskVol
- 推送序列：网格严格递增、无重复；09:15 起始与 15:00 收盘边界
- 累计量严格单调；与官方 Tick 偏差仅记录不断言
- 真实数据覆盖：SZ 股票 / SH 股票 / SH ETF，开盘集合竞价、连续竞价、
  收盘集合竞价三阶段均有推送；多标的并行推送互不串号

实测推送量（100ms 间隔，全天）：300827.SZ 85206 次、600050.SH 134282 次、
518880.SH 87416 次。
