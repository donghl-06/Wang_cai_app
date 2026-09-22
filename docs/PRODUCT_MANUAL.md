# 旺财回测平台 · 产品手册

> 适用版本：wangcai_syn 1.4.0 及之后构建（2026-09）
> 读者：使用本平台做策略研究与回测的同事
> 配套示例：仓库 `user_example/` 目录（11 个可直接运行的脚本）

---

## 目录

1. [产品概述](#1-产品概述)
2. [安装](#2-安装)
3. [五分钟上手](#3-五分钟上手)
4. [数据准备](#4-数据准备)
5. [策略编写：回调总览](#5-策略编写回调总览)
6. [交易接口：下单与撤单](#6-交易接口下单与撤单)
7. [回调与快照字段速查](#7-回调与快照字段速查)
8. [撮合与交易语义](#8-撮合与交易语义)
9. [回测模式：默认 / 严格 / 真实成交替代](#9-回测模式默认--严格--真实成交替代)
10. [盘口推送：三级粒度](#10-盘口推送三级粒度)
11. [真实感增强：下单延迟 / 价格笼子 / 队列信息](#11-真实感增强下单延迟--价格笼子--队列信息)
12. [自定义数据推送](#12-自定义数据推送)
13. [多标的回测与性能](#13-多标的回测与性能)
14. [run_backtest 参数总表](#14-run_backtest-参数总表)
15. [示例脚本索引](#15-示例脚本索引)
16. [常见问题 FAQ](#16-常见问题-faq)
17. [版本功能演进](#17-版本功能演进)

---

## 1. 产品概述

旺财（wangcai_syn）是一个**事件驱动的 A 股逐笔回测引擎**：核心引擎用 C++ 编写，通过 pybind11 提供 Python 接口。它基于真实的逐笔委托、逐笔成交和 Tick 快照数据**完整重建订单簿**，把你的策略订单作为"虚拟订单"插入重建的订单簿中，按**价格优先、时间优先**规则与历史订单撮合，从而精确还原你的策略在当日市场中会得到怎样的成交。

### 核心能力

| 能力 | 说明 |
|------|------|
| 逐笔订单簿重建 | 从逐笔委托/成交数据重建全档订单簿，虚拟订单按真实排队位置撮合 |
| 完整交易时段模拟 | 开盘集合竞价（09:15-09:25）、连续竞价、收盘集合竞价（14:57-15:00） |
| 交易所规则内建 | 涨跌停校验、价格笼子（有效申报价格范围）、沪深两市排序差异 |
| 三种撮合模式 | 默认（快速验证）/ 严格主动单（冲击成本）/ 真实成交替代（精确成本） |
| 三级盘口粒度 | 官方 3 秒 Tick / 毫秒级合成 Tick / 逐事件快照 |
| 链路时延模拟 | 可配置的下单延迟与进簿排队位置 |
| 股票与 ETF | 股票两位小数、ETF 三位小数，混合回测 |
| 多标的并行 | 多合约 Taskflow 并行 + 跨标的订单正确路由 |

---

## 2. 安装

```bash
pip install wangcai_syn --upgrade -i https://pypi.aitopia.tech/simple/
```

依赖：Python 3.8+，pandas。

> 版本说明：实时合成 Tick 自 1.4.0 起提供；**下单延迟、价格笼子、事件驱动快照**为 1.4.0 之后合入的能力（2026-09-17 起的构建均包含），如功能不可用请先 `pip install wangcai_syn --upgrade` 更新到最新构建。

---

## 3. 五分钟上手

三步：准备数据 → 写策略 → 运行。

### 第一步：准备数据

每个标的 × 交易日需要 4 个 CSV 文件（命名规范见[第 4 节](#4-数据准备)）：

```text
cstick_300827.SZ_2025-11-17.csv    # Tick 快照（约 3 秒一条）
csord_300827.SZ_2025-11-17.csv     # 逐笔委托
cstra_300827.SZ_2025-11-17.csv     # 逐笔成交
csbar1d_300827.SZ_2025-11-17.csv   # 日线（含涨跌停价）
```

### 第二步：编写策略

继承 `Strategy`，在事件回调里返回下单/撤单指令：

```python
from wangcai_syn import Strategy, make_order

class MyStrategy(Strategy):
    def __init__(self):
        super().__init__()
        self.account = "user1"
        self.order_count = 0

    def getStrategyId(self):
        return self.account          # 必须与下单时的 Account 一致

    def onTickEvent(self, snapshot):
        events = []
        # 收到 Tick：买一价有效时，买一价挂 100 股（被动单）
        if self.order_count < 5 and snapshot.bids[0] > 0:
            events.append(make_order(
                Broker='',
                Account=self.account,
                Exchange=snapshot.Exchange,       # 0=上海, 1=深圳
                Instrument=snapshot.Instrument,
                OrderLocalID=f"ORDER_{self.order_count}",
                OrderType=0,                      # 限价单
                Direction=1,                      # 1=买入, 2=卖出
                Price=snapshot.bids[0],           # 价格单位：厘
                Volume=100,
            ))
            self.order_count += 1
        return events                              # 返回指令列表，可为空

    def onOrderCallback(self, cb):                 # 你的订单被引擎接受
        print(f"下单确认: {cb.orderlocalid}")

    def onTradeCallback(self, cb):                 # 成交 / 撤单通知
        if cb.matchtype == 'T':
            print(f"成交: {cb.localid} {cb.volume}@{cb.price/10000:.4f}")
        elif cb.matchtype == 'D':
            print(f"撤单/废单: {cb.localid}")

    # 以下空实现建议保留（也可不写，见 13.2 性能提示）
    def onOrderEvent(self, order): return []
    def onTradeEvent(self, trade): return []
```

### 第三步：运行回测

```python
import pandas as pd
import wangcai_syn

symbol, date = "300827.SZ", "2025-11-17"
data = {symbol: (
    pd.read_csv(f"cstick_{symbol}_{date}.csv"),
    pd.read_csv(f"csord_{symbol}_{date}.csv"),
    pd.read_csv(f"cstra_{symbol}_{date}.csv"),
    pd.read_csv(f"csbar1d_{symbol}_{date}.csv"),
)}

wangcai_syn.run_backtest(data, MyStrategy(), output_dir="./output")
```

或直接运行现成示例：

```bash
cd user_example
python run_example.py
```

---

## 4. 数据准备

### 4.1 四类数据文件

| 数据 | 内容 | 是否必需 |
|------|------|---------|
| `cstick` | Tick 快照：十档盘口、最新价、累计成交量等，约 3 秒一条 | 必需 |
| `csord` | 逐笔委托：每笔市场委托（含撤单委托），用于重建订单簿 | 必需 |
| `cstra` | 逐笔成交：每笔成交的价、量、买卖订单号 | 必需 |
| `csbar1d` | 日线：当日涨跌停价、昨收等 | 必需 |

命名规范：`{类型}_{代码}_{日期}.csv`，如 `cstick_000488.SZ_2024-12-19.csv`。

### 4.2 股票与 ETF

- 股票：两位小数，最小变动价位 0.01 元（100 厘）
- ETF：三位小数，最小变动价位 0.001 元（10 厘）

`data_dict` 支持 4 元组（默认股票）或 5 元组（末位显式指定 `is_etf`）：

```python
data = {
    "000488.SZ": (cstick1, order1, trade1, bar1),            # 股票
    "159001.SZ": (cstick2, order2, trade2, bar2, True),      # ETF（三位小数）
    "300827.SZ": (cstick3, order3, trade3, bar3, False),     # 显式股票
}
```

### 4.3 价格单位：厘

系统内部所有价格为**厘**（1 元 = 10000 厘），避免浮点误差：

```python
price_li   = int(8.50 * 10000)      # 元 → 厘: 85000
price_yuan = 85000 / 10000          # 厘 → 元: 8.50
```

包内提供辅助函数：`convert_price_to_li()`（元转厘）、`align_price()`（对齐到 100 厘）、`format_price()`（厘转元字符串）。

### 4.4 时间格式

`snapshot.Time` / `cb.time` 为整数 `HHMMSSmmm`：

```python
t = 93015123            # 09:30:15.123
ms     = t % 1000
minute = (t // 100000) % 100
hour   = t // 10000000
```

`snapshot.datetime` 为完整时间字符串（如 `'2025-11-17 09:30:15.123'`）。

---

## 5. 策略编写：回调总览

策略继承 `wangcai_syn.Strategy`，按需重载回调。回调分两类：

### 5.1 市场事件回调（返回 `List[UserEvent]`，可直接下单/撤单）

| 回调 | 触发时机 | 开关 |
|------|---------|------|
| `onTickEvent(snapshot)` | 收到官方 Tick 快照（约 3 秒一条） | 始终开启 |
| `onOrderEvent(order)` | 收到市场逐笔委托 | 始终开启 |
| `onTradeEvent(trade)` | 收到市场逐笔成交 | 始终开启 |
| `onTradeEventsBatch(trades)` | 同一市场事件撮合产生的多笔成交**批量**推送（性能优化，见 13.3） | 覆写即生效 |
| `onRealTimeTickEvent(snapshot)` | 实时合成 Tick，毫秒级间隔网格 | `realtime_tick_interval_ms > 0` |
| `onEventSnapshot(snapshot)` | 每个市场事件处理完毕后的十档快照 | `event_snapshot_enabled=True` |
| `onCustomEvent(data: dict)` | 自定义数据按时间戳推送 | `enable_custom_data=True` |

这些回调返回一个 `UserEvent` 列表（`make_order(...)` / `make_cancel(...)` 的返回值），引擎在**当前事件内**立即执行；返回空列表 `[]` 表示不下单。

### 5.2 订单通知回调（无返回值）

| 回调 | 触发时机 |
|------|---------|
| `onOrderCallback(cb)` | 你的订单被引擎接受（申报回执） |
| `onTradeCallback(cb)` | 你的订单成交（`matchtype='T'`）或撤单/废单（`matchtype='D'`） |
| `onOrderFilled(order_id, price, volume)` | 订单成交（备用接口） |
| `onOrderCancelled(order_id, reason)` | 订单撤销（备用接口） |

### 5.3 必须实现

`getStrategyId()` 返回账户 ID，**必须与 `make_order(Account=...)` 一致**，否则订单无法路由回你的策略。

---

## 6. 交易接口：下单与撤单

### 6.1 下单 make_order

```python
from wangcai_syn import make_order

order = make_order(
    Broker='',                    # 券商代码（可空）
    Account='user1',              # 账户 = getStrategyId() 返回值
    Exchange=1,                   # 交易所: 0=上海, 1=深圳
    Instrument='000488.SZ',       # 合约代码
    OrderLocalID='ORDER_001',     # 本地订单号，自拟，用于追踪
    OrderType=0,                  # 订单类型: 0=限价单
    Direction=1,                  # 1=买入, 2=卖出
    Price=80000,                  # 价格（厘）
    Volume=100,                   # 数量（股）
)
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `OrderLocalID` | str | 订单唯一标识，成交/撤单回调据此对账 |
| `OrderType` | int | 目前支持限价单（0） |
| `Direction` | int | 1=买入，2=卖出 |
| `Price` | int | 厘。需在涨跌停范围内、且满足价格笼子（见 11.2），否则废单 |

### 6.2 撤单 make_cancel

```python
from wangcai_syn import make_cancel

cancel = make_cancel(
    Broker='',
    Account='user1',
    Exchange=1,
    Instrument='000488.SZ',
    CancelOrderLocalID='CANCEL_001',   # 撤单请求号，自拟
    OrderLocalID='ORDER_001',          # 要撤销的订单号
)
```

撤单成功后收到 `onTradeCallback` 的 `matchtype='D'` 回调。

> 除 `make_order` / `make_cancel` 外，包内还提供 C++ 原生风格的 `make_order_event(order_id, symbol, direction, order_type, price, volume, strategy_id)` 与 `make_cancel_event(order_id, strategy_id)`，二者等价，任选其一即可。

---

## 7. 回调与快照字段速查

### 7.1 OrderCallback（onOrderCallback）

| 字段 | 类型 | 说明 |
|------|------|------|
| `orderlocalid` | str | 订单本地 ID |
| `direction` | int | 1=买入，2=卖出 |
| `price` | int | 下单价格（厘） |
| `volume` | int | 下单数量 |
| `bid1` / `ask1` | int | 下单时刻买一/卖一（厘） |
| `deltapos` | int | 当前总持仓 |
| `time` | int | 下单确认时间（HHMMSSmmm） |
| `queue_ahead_count` | int | 前方同价位历史订单数（需 `queue_info_enabled=True`，否则 -1） |
| `queue_ahead_volume` | int | 前方同价位历史订单总量（需开关，否则 -1） |
| `prev_order_ids` | list | 前方最近 3 个历史订单号（需开关，否则空） |

### 7.2 TradeCallback（onTradeCallback）

| 字段 | 类型 | 说明 |
|------|------|------|
| `matchtype` | char | `'T'`=成交，`'D'`=撤单/废单 |
| `localid` | str | 订单本地 ID |
| `direction` | char | `'B'`=买入，`'S'`=卖出 |
| `price` | int | 成交价格（厘） |
| `volume` | int | 成交数量 |
| `matchamount` | float | 成交金额（元） |
| `deltapos` | int | 成交后总持仓 |
| `matchtime` | int/str | 成交时间 |

### 7.3 Snapshot（onTickEvent / onRealTimeTickEvent / onEventSnapshot 共用）

```python
snapshot.Exchange        # 交易所 (0=SH, 1=SZ)
snapshot.Instrument      # 合约代码
snapshot.Time            # 时间 HHMMSSmmm 整数
snapshot.datetime        # 完整时间字符串
snapshot.PreClose        # 昨收价（厘）
snapshot.Open/High/Low   # 开/高/低
snapshot.last_price      # 最新价
snapshot.UpperLimit      # 涨停价
snapshot.LowerLimit      # 跌停价
snapshot.bids            # 买价数组（10 档，厘）
snapshot.bid_sizes         # 买量数组（10 档，股）
snapshot.asks / ask_sizes  # 卖盘价量
snapshot.TotalBidVol / TotalAskVol  # 全簿挂单总量（合成 Tick 提供）
snapshot.Volume          # 累计成交量
snapshot.Turnover        # 累计成交额
snapshot.NumTrades       # 累计成交笔数
snapshot.AuctionPrice / AuctionQty  # 集合竞价预测价/量
```

---

## 8. 撮合与交易语义

### 8.1 交易时段

| 时段 | 时间 | 说明 |
|------|------|------|
| 开盘集合竞价 | 09:15 - 09:25 | 09:20 前可下可撤，09:20-09:25 只下不撤 |
| 连续竞价（上午） | 09:30 - 11:30 | 逐笔撮合 |
| 连续竞价（下午） | 13:00 - 14:57 | 逐笔撮合 |
| 收盘集合竞价 | 14:57 - 15:00 | 只下不撤，15:00 统一撮合 |

### 8.2 订单簿重建与虚拟撮合

引擎用 csord/cstra 实时重建真实订单簿，你的订单作为虚拟订单插入：

```
场景 1: 用户单 vs 历史单  → 虚拟成交 ✅
场景 2: 历史单 vs 用户单  → 虚拟成交 ✅
场景 3: 用户单 vs 用户单  → 不成交 ❌（防止自我对敲）
场景 4: 历史单 vs 历史单  → 真实成交（重建订单簿用）
```

- **价格优先、时间优先**：同价位按订单到达顺序排队，你的虚拟单排在同时刻历史单之后（可用下单延迟的 head/tail 改变同时刻相对位置，见 11.1）
- **无市场冲击**：虚拟单成交不改变历史订单簿，适合小资金策略测试
- **成交价规则**：你的主动单按对手盘最优价成交；历史单撞上你的挂单按你的挂单价成交
- **持仓自动维护**：买入 `deltapos += volume`，卖出 `-= volume`

### 8.3 集合竞价：影子订单

竞价阶段（09:15-09:25、14:57-15:00）你的订单作为**影子单**暂存，不参与真实价格发现：

| 阶段 | 你的订单行为 |
|------|-------------|
| 竞价期间下单 | 暂存为影子单，竞价期间立刻撤单则直接删除（`'D'`） |
| 09:25 开盘价确定 | 价格能穿越开盘价的按**开盘价全量成交**（`'T'`）；不能穿越的**结转连续竞价**继续挂单 |
| 15:00 收盘价确定 | 能穿越的按收盘价成交（`'T'`）；不能穿越的直接撤单（`'D'`，不结转） |

### 8.4 涨跌停与废单

- 超过涨跌停价格的策略限价单**直接废单**（所有时期生效；涨跌停价来自 csbar1d）
- 涨跌幅规则：主板 ±10%，科创板/创业板 ±20%，ST ±5%
- 部分板块适用价格笼子限制，见 [11.2](#112-价格笼子策略单)
- 废单链路：先收到 `onOrderCallback` 申报回执 → 引擎校验拒绝 → `onTradeCallback` 收到 `matchtype='D'`，废单原因打印在引擎日志

### 8.5 沪深两市差异

| 特性 | 上海 (SH) | 深圳 (SZ) |
|------|----------|----------|
| 委托数据 | 需还原（引擎自动处理） | 直接逐笔 |
| 订单排序依据 | BizIndex | OrderID |
| 历史委托类型 | 限价 | 限价/市价/本方最优等 |

---

## 9. 回测模式：默认 / 严格 / 真实成交替代

三种模式控制虚拟订单的成交约束，从乐观到严格：

### 9.1 默认模式

主动单（价格穿越对手盘）立即全部成交；被动单等对手方历史订单被移除后成交。**速度快，适合策略逻辑验证**。

### 9.2 严格主动单模式

```python
run_backtest(data, strategy, strict_active_order_mode=True)
```

- 主动单只能吃掉对手方**实际挂着的量**，吃不完的剩余量自动撤单
- 吃掉的历史订单记入"**欠债**"：这些量被真实市场消化完之前，禁止提交新的主动单（提交会收到 `'D'` 拒单回调）
- 被动单不受欠债限制
- **适合估计主动单的冲击成本**

### 9.3 真实成交替代模式（RT 模式）

```python
run_backtest(data, strategy, real_trade_match_mode=True)
```

在严格模式基础上进一步约束被动单：

- 被动单只有在**同价位发生了真实历史成交**时才能成交，且总量受真实成交量限制，用完即止
- 下单时记录前方排队量（`queue_position`），真实成交先消耗完前方的量才轮到你
- 撤单推进：只有你入场**之前**的历史单被撤销才推进你的排队位置
- 支持多次部分成交（每笔一次 `'T'` 回调）
- 主动单自动启用严格模式（无需重复设置）
- **适合精确估计交易成本**

### 9.4 三模式对比

| | 默认 | 严格主动单 | 真实成交替代 (RT) |
|---|------|-----------|------------------|
| 主动单 | 立即全部成交 | 只吃实际挂量 + 欠债限制 | 同严格模式 |
| 被动单 | 对手历史单移除即成交 | 同默认 | 同价位真实成交才成交，排队等待 |
| 成交难度 | 最容易 | 中等 | 最接近真实 |
| 适用场景 | 策略逻辑验证 | 冲击成本估计 | 交易成本精确估计 |

---

## 10. 盘口推送：三级粒度

平台提供三种盘口观察粒度，按需开启：

| | 官方 Tick | 实时合成 Tick | 事件驱动快照 |
|---|----------|--------------|--------------|
| 回调 | `onTickEvent` | `onRealTimeTickEvent` | `onEventSnapshot` |
| 粒度 | 约 3 秒 | 毫秒级间隔网格（自选 10/50/100ms…） | 每个市场事件一次 |
| 开关 | 始终开启 | `realtime_tick_interval_ms=N` | `event_snapshot_enabled=True` |
| 全天回调量级* | ~4800 | ~8.5 万（100ms） | ~22 万 |
| 适用 | 常规策略 | 日内/高频策略 | 盘口微观结构研究 |

\* 300827.SZ 2025-11-17 实测量：官方 4830 / 100ms 合成 85206 / 事件快照 218731。

### 10.1 实时合成 Tick

```python
run_backtest(data, strategy, realtime_tick_interval_ms=100)
```

```python
def onRealTimeTickEvent(self, snapshot):
    # 字段结构与官方 Tick 完全一致，可直接返回下单指令
    bid1, ask1 = snapshot.bids[0], snapshot.asks[0]
    return []
```

语义要点：

- 按回放时间划分毫秒网格，每跨过一个网格边界、且当前市场事件处理完毕后，从内部订单簿**现合成**一个十档快照推送
- 十档/最新价/涨跌停来自订单簿实时状态；`Volume/Turnover/NumTrades/High/Low` 由引擎按重建成交逐笔累计，**单调不减**，与官方快照的数值出入来自时间戳口径不同，属正常现象，引擎不做对齐
- 无事件的空白区间（如午休）不补发；同一网格不重复推送
- 集合竞价阶段盘口是未交叉的原始挂单分布，买一可能高于卖一，与连续竞价十档语义不同
- 10ms 间隔全天推送可达数十万次，回调内只做轻量计算

### 10.2 事件驱动快照

```python
run_backtest(data, strategy, event_snapshot_enabled=True)
```

每个市场事件（逐笔委托/逐笔成交含撤单）的**全部处理**——撮合 + 你响应产生的下单/撤单——结束后推送一次十档快照，是最高粒度。集合竞价阶段委托集中处理，不逐事件推送，因此推送次数略少于"委托+成交总行数"。

### 10.3 示例

```bash
cd user_example
python run_realtime_tick_test.py     # 100ms 合成 Tick，统计与官方对比
python run_event_snapshot_test.py    # 逐事件快照，抽样打印盘口
```

---

## 11. 真实感增强：下单延迟 / 价格笼子 / 队列信息

### 11.1 下单延迟模拟

```python
run_backtest(
    data, strategy,
    order_latency_enabled=True,
    order_latency_ms=50,              # 10ms 粒度对齐: 14→10, 15→20
    latency_entry_position="head",    # "head"(默认) / "tail"
)
```

模拟策略到交易所的链路时延：

- 回调中返回的下单/撤单**不立即进簿**，延迟到"发出时刻 + latency"才到达交易所：此时才过涨跌停/价格笼子校验、进订单簿、参与撮合；下单确认回调同样延迟到到达时刻
- 撤单与下单走同一延迟通道（保 FIFO）；15:00 后才到达的订单被丢弃
- 实际进簿时机为到达时刻之后的**第一个市场事件**（回测时钟只随市场事件前进）
- `latency_entry_position`：多笔订单同一时刻到达时，`"head"` 排在同时间订单头部（抢同价位排队优先级），`"tail"` 排尾部
- 也可在策略 `__init__` 中设置：`self.setOrderLatencyMs(50)` / `self.setLatencyEntryPosition(LatencyEntryPosition.Tail)`
- **用途**：评估策略对链路时延的敏感性——同一信号，0ms 与 50ms 延迟的成交结果可能完全不同

示例：`python run_order_latency_test.py`（下单后对比确认回调时刻与发出时刻，实测延迟到达）

### 11.2 价格笼子（策略单）

策略限价单默认受交易所**有效申报价格范围**约束（`user_cage_enabled=True`），规则按数据日期 × 板块自动判定：

| 板块 | 时期 | 行为 |
|------|------|------|
| 主板 | 2023-04-10 起 | 基准价 ±2% 与 0.1 元孰高，超范围**废单** |
| 创业板 | 2020-08-24 ~ 2023-04-10 | **暂存模式**：超范围入笼挂起，价格落回范围自动恢复撮合 |
| 创业板 | 2023-04-10 起 | 同主板（废单） |
| 科创板 | 2019-07-22 起 | 拒单（纯 ±2%），2023-04-10 后加 0.1 元兜底 |
| 北交所 / ETF / 债券 | — | 不适用 |

要点：

- 有效申报范围 = 基准价 ±2% **四舍五入至最小变动价位**；基准价链：对手一档 → 本方一档 → 最新成交 → 昨收
- 笼外单流程：先收到 `onOrderCallback` 申报回执 → 引擎废单 → `onTradeCallback` 收到 `matchtype='D'`，废单原因在引擎日志
- 创业板暂存窗口内笼外单不废，挂起等待价格落回后自动激活
- `user_cage_enabled=False` 只关闭**策略单**的笼子判定；订单簿侧（交易所对历史单的行为）始终按真实规则模拟

示例：`python run_price_cage_test.py`（同刻下一笔笼内单 + 一笔笼外单，对比结局：笼内留簿、笼外废单）

### 11.3 下单回调队列信息

```python
run_backtest(data, strategy, queue_info_enabled=True)
```

```python
def onOrderCallback(self, cb):
    print(cb.queue_ahead_count)     # 前方同价位历史订单数
    print(cb.queue_ahead_volume)    # 前方同价位历史订单总量
    print(cb.prev_order_ids)        # 前方最近 3 个历史订单号（真实 orderid）
```

用于精确观察"订单提交瞬间"的排队情况，研究排队损耗与撤单决策。关闭时字段为 -1/空。

示例：`python run_queue_info_test.py`

---

## 12. 自定义数据推送

把外部信号（因子值、目标仓位等）按时间戳注入回测：

```python
import pandas as pd

custom_df = pd.DataFrame({
    'datetime': ['2025-11-17 09:35:00', '2025-11-17 10:00:00'],
    'signal': ['buy', 'sell'],
    'target_price': [44.50, 45.00],
})

run_backtest(data, strategy,
             custom_data=custom_df,
             enable_custom_data=True)
```

要求：DataFrame 必须含 `datetime` 列（格式 `2025-11-17 09:35:00`），其余列自定义。引擎在对应回测时刻把该行作为 dict 推给策略：

```python
def onCustomEvent(self, data: dict):
    # data = {'datetime': ..., 'signal': 'buy', 'target_price': 44.50}
    if data.get('signal') == 'buy':
        return [make_order(...)]
    return []
```

示例：`python run_strict_with_custom_data.py`（严格模式 + 信号驱动下单）

---

## 13. 多标的回测与性能

### 13.1 多标的与跨标的路由

`data_dict` 一次传入任意多标的，引擎 Taskflow 并行构建、按时间戳归并推进：

```python
data = {
    "300827.SZ": (...),    # 创业板股票
    "600519.SH": (...),    # 沪市股票
    "510050.SH": (..., True),  # ETF
}
run_backtest(data, strategy)   # 单策略同时接收三只标的事件
```

在任何标的的回调里都可以返回**其他标的**的订单，引擎自动路由到正确的子引擎、按对应标的的价格精度撮合（不会"串单"）。

数据准备阶段可用 `n_workers` 并行转换 DataFrame（默认串行）：

```python
run_backtest(data, strategy, n_workers=8)
```

### 13.2 性能提示

- **不用的回调不要覆写**——连"return []" 的空覆写也会产生每次事件的跨语言开销；引擎对未覆写的回调做探测缓存，完全跳过
- 大批量回测（数据用完即弃）可开 `release_input=True`：转换后清空 `data_dict`，几十个标的可省数 GB 内存；代价是同一份数据不能跑第二次

### 13.3 批量成交推送（高性能采集）

默认 `onTradeEvent` 逐笔推送。覆写 `onTradeEventsBatch(trades)` 后，同一市场事件撮合产生的多笔成交合并为一个 list 推送，每个市场事件只跨一次 Python/C++ 边界，采集型负载约提速 40%：

```python
def onTradeEventsBatch(self, trades):
    for t in trades:
        ...    # t 字段与 onTradeEvent 的 trade 相同
    return []
```

不覆写则自动逐条回落到 `onTradeEvent`，旧策略零改动兼容。

---

## 14. run_backtest 参数总表

```python
run_backtest(
    data_dict,                          # 必填：回测数据
    strategy,                           # 必填：策略实例
    output_dir=None,                    # 输出目录（供策略 save_records 用）
    strict_active_order_mode=False,     # 严格主动单模式
    real_trade_match_mode=False,        # 真实成交替代模式
    queue_info_enabled=False,           # 下单回调队列信息
    custom_data=None,                   # 自定义数据 DataFrame
    enable_custom_data=False,           # 启用自定义数据推送
    n_workers=None,                     # 数据准备并行 worker 数
    release_input=False,                # 转换后清空输入省内存
    realtime_tick_interval_ms=0,        # 实时合成 Tick 间隔（0=关闭）
    event_snapshot_enabled=False,       # 事件驱动快照
    user_cage_enabled=True,             # 策略单价格笼子判定
    order_latency_enabled=False,        # 下单延迟开关
    order_latency_ms=0,                 # 延迟毫秒（开启未指定时默认 20）
    latency_entry_position="head",      # 延迟单进簿位置 head/tail
)
```

| 参数 | 默认 | 说明 | 详见 |
|------|------|------|------|
| `strict_active_order_mode` | False | 主动单受实际挂量 + 欠债限制 | §9.2 |
| `real_trade_match_mode` | False | 被动单只用真实成交量匹配（自动含严格模式） | §9.3 |
| `queue_info_enabled` | False | onOrderCallback 增加队列三字段 | §11.3 |
| `custom_data` / `enable_custom_data` | None/False | 外部信号按时间戳注入 | §12 |
| `n_workers` | None | 数据准备并行度，>1 且多标的时启用进程池 | §13.1 |
| `release_input` | False | 转换后清空 data_dict 省内存 | §13.2 |
| `realtime_tick_interval_ms` | 0 | 合成 Tick 间隔（10/50/100…），0 关闭 | §10.1 |
| `event_snapshot_enabled` | False | 逐事件十档快照 | §10.2 |
| `user_cage_enabled` | True | 策略单价格笼子（False 仅关策略侧） | §11.2 |
| `order_latency_enabled` | False | 下单/撤单延迟进簿 | §11.1 |
| `order_latency_ms` | 0 | 延迟毫秒，10ms 对齐，开启未指定默认 20 | §11.1 |
| `latency_entry_position` | "head" | 同时到达订单的进簿相对位置 | §11.1 |

---

## 15. 示例脚本索引

`user_example/` 下全部示例可直接运行（数据放在配置区 `DATA_DIR` 指向的目录）：

| 脚本 | 演示功能 | 手册章节 |
|------|---------|---------|
| `my_strategy.py` | 策略模板：完整结构 + 下单 + 回调处理 | §5 |
| `run_example.py` | 基础回测：单标的默认模式 | §3 |
| `run_strict_with_custom_data.py` | 严格模式 + 自定义信号下单 | §9.2 / §12 |
| `run_real_trade_mode.py` | 真实成交替代模式：排队 + 部分成交 | §9.3 |
| `run_queue_info_test.py` | 下单回调队列信息三字段 | §11.3 |
| `run_etf_test.py` | ETF（三位小数）与股票混合回测 | §4.2 |
| `run_auction_shadow_order_test.py` | 集合竞价影子单：竞价成交/结转/撤单 | §8.3 |
| `run_cross_symbol_test.py` | 跨标的订单路由 | §13.1 |
| `run_order_latency_test.py` | 下单延迟 50ms、head/tail 进簿位置 | §11.1 |
| `run_price_cage_test.py` | 价格笼子：笼内接受 / 笼外废单 | §11.2 |
| `run_event_snapshot_test.py` | 事件驱动快照：逐事件十档盘口 | §10.2 |
| `run_realtime_tick_test.py` | 实时合成 Tick：100ms 间隔推送 | §10.1 |

运行方式：

```bash
cd user_example
python run_price_cage_test.py
```

---

## 16. 常见问题 FAQ

**Q: 价格为什么是很大的整数？**
A: 内部单位为厘，1 元 = 10000 厘，避免浮点误差。见 §4.3。

**Q: 订单被拒绝，提示 price out of limit？**
A: 检查价格是否超出 csbar1d 提供的涨跌停范围；或触发了价格笼子（§11.2），废单原因会打印在引擎日志。

**Q: 收到 matchtype='D' 但我没撤单？**
A: `'D'` 除撤单确认外还表示**废单/拒单**：超涨跌停、笼外单、严格模式欠债拦截、RT 模式量不足的剩余撤单等。结合引擎日志中的原因判断。

**Q: 回调没有触发？**
A: 检查 `getStrategyId()` 返回值与 `make_order(Account=...)` 是否一致。

**Q: 如何获取当前持仓？**
A: `onTradeCallback` 中 `cb.deltapos` 为成交后总持仓，引擎自动维护。

**Q: 订单什么时候会成交？**
A: 取决于模式：默认模式穿越价差立即成交；严格模式受实际挂量限制；RT 模式被动单需等真实成交消耗完前方排队量。见 §9。

**Q: real_trade_match_mode 和 strict_active_order_mode 要同时设吗？**
A: 不需要，RT 模式自动包含严格主动单模式。

**Q: 支持多策略吗？**
A: 每次回测运行单个策略实例；多策略请分别运行。

**Q: 合成 Tick 的成交量与官方 Tick 对不上？**
A: 正常现象。合成 Tick 的累计量是引擎重建口径（按事件时间累计），官方 Tick 有独立时间戳，两者口径不同；合成 Tick 内部各字段彼此严格自洽。见 §10.1。

**Q: ETF 和股票数据有什么区别？**
A: ETF 三位小数（tick=0.001 元），股票两位小数（tick=0.01 元），data_dict 用 5 元组指定 `is_etf=True`。见 §4.2。

**Q: 上海股票的委托数据好像不完整？**
A: 沪市逐笔委托需要还原处理，引擎已自动完成，无需额外操作。

---

## 17. 版本功能演进

| 版本/时期 | 主要能力 |
|-----------|---------|
| 1.3.1（2025-12） | 首个可用版本：逐笔回测、订单簿重建、基础策略接口 |
| 1.3.2（2026-01） | 严格主动单模式（欠债限制）、自定义数据推送 |
| 1.3.3（2026-01） | ETF 三位小数、沪市行情流与委托还原 |
| 1.3.9（2026-08） | 真实成交替代模式、下单回调队列信息、市场订单身份体系 |
| 1.4.0（2026-09-04） | 实时合成 Tick（`onRealTimeTickEvent`，毫秒级间隔网格） |
| 最新构建（2026-09） | 事件驱动快照（`onEventSnapshot`）、策略单价格笼子（含创业板暂存模式）、下单延迟模拟、批量成交推送（`onTradeEventsBatch`）、集合竞价影子单、跨标的订单路由、盘中临时停牌与复牌竞价语义、引擎性能优化（多合约并行提速约 50%） |

---

*本手册由 docs/ 下 CHANGELOG 系列、QUICKSTART、TRADING_LOGIC 与 user_example 示例整理而成；各功能的完整测试记录见 `docs/` 对应文档。有问题请联系平台维护同事。*
