# 旺财回测平台 - 快速入门指南

> 请先下载回测范例压缩包，里面有使用范例
> - 使用 `my_strategy.py` + `run_example.py` 查看交易接口
> - 使用 `run_strict_with_custom_data.py` 验证严格模式 + 自定义数据
> - 使用 `run_real_trade_mode.py` 验证真实成交替代模式

## 1. 安装

```bash
pip install wangcai_syn==1.3.4 -i https://pypi.aitopia.tech/simple/
```

## 2. 快速开始

### 2.1 准备数据

回测需要四个 DataFrame：

| 数据类型 | 说明 | 文件示例 |
|---------|------|---------|
| `cstick_df` | Tick 快照数据 | `cstick_000488.SZ_2024-12-19.csv` |
| `order_df` | 逐笔委托数据 | `csord_000488.SZ_2024-12-19.csv` |
| `trade_df` | 逐笔成交数据 | `cstra_000488.SZ_2024-12-19.csv` |
| `csbar1d_df` | 日线数据（含涨跌停） | `csbar1d_000488.SZ_2024-12-19.csv` |

```python
import pandas as pd

cstick_df = pd.read_csv("cstick_000488.SZ_2024-12-19.csv")
order_df  = pd.read_csv("csord_000488.SZ_2024-12-19.csv")
trade_df  = pd.read_csv("cstra_000488.SZ_2024-12-19.csv")
csbar1d_df = pd.read_csv("csbar1d_000488.SZ_2024-12-19.csv")
```

### 2.2 编写策略

继承 `Strategy` 类，重载事件处理函数：

```python
from wangcai_syn import Strategy, make_order

class MyStrategy(Strategy):
    def __init__(self):
        super().__init__()
        self.order_count = 0
        self.account = "user1"

    def getStrategyId(self):
        return self.account

    def onTickEvent(self, snapshot):
        events = []
        if self.order_count < 5 and snapshot.bids[0] > 0:
            event = make_order(
                Broker='',
                Account=self.account,
                Exchange=1,                      # 0=上海, 1=深圳
                Instrument=snapshot.Instrument,
                OrderLocalID=f"ORDER_{self.order_count}",
                OrderType=0,                     # 0=限价单
                Direction=1,                     # 1=买入, 2=卖出
                Price=snapshot.bids[0],          # 买一价（厘）
                Volume=100
            )
            events.append(event)
            self.order_count += 1
        return events

    def onTradeCallback(self, callback):
        if callback.matchtype == 'T':
            print(f"成交: {callback.localid} {callback.volume}@{callback.price/10000:.4f}")
        elif callback.matchtype == 'D':
            print(f"撤单: {callback.localid}")

    def onOrderCallback(self, callback):
        print(f"下单确认: {callback.orderlocalid}")

    # 以下为必须实现的空方法
    def onOrderEvent(self, order):
        return []
    def onTradeEvent(self, trade):
        return []
    def onOrderFilled(self, order_id, price, volume):
        pass
    def onOrderCancelled(self, order_id, reason):
        pass
```

### 2.3 运行回测

```python
from wangcai_syn import run_backtest

data = {
    "000488.SZ": (cstick_df, order_df, trade_df, csbar1d_df)
}

strategy = MyStrategy()
run_backtest(data, strategy, output_dir="./output")
```

---

## 3. run_backtest 完整参数说明

```python
run_backtest(
    data_dict,                          # 必填，回测数据
    strategy,                           # 必填，策略实例
    output_dir=None,                    # 输出目录，None=不保存
    strict_active_order_mode=False,     # 严格主动单模式
    real_trade_match_mode=False,        # 真实成交替代模式
    custom_data=None,                   # 自定义数据 DataFrame
    enable_custom_data=False,           # 是否启用自定义数据推送
    realtime_tick_interval_ms=0,        # 实时合成 Tick 间隔（毫秒），0=关闭
)
```

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `data_dict` | dict | 必填 | `{symbol: (cstick_df, order_df, trade_df, csbar1d_df)}` 或 5 元组加 `is_etf` |
| `strategy` | Strategy | 必填 | 策略实例，需继承 `Strategy` 类 |
| `output_dir` | str | None | 输出目录路径，None 表示不保存 |
| `strict_active_order_mode` | bool | False | 开启后主动单受"欠债"限制，详见第 8 节 |
| `real_trade_match_mode` | bool | False | 开启后被动单只用真实成交量匹配，详见第 9 节 |
| `custom_data` | DataFrame | None | 自定义数据，必须含 `datetime` 列，详见第 10 节 |
| `enable_custom_data` | bool | False | 是否启用自定义数据推送 |
| `realtime_tick_interval_ms` | int | 0 | 实时合成 Tick 推送间隔（毫秒），0=关闭，详见第 13 节 |

### data_dict 格式

```python
# 股票（默认，两位小数）
data = {"000488.SZ": (cstick_df, order_df, trade_df, csbar1d_df)}

# ETF（三位小数，需用5元组指定 is_etf=True）
data = {"159001.SZ": (cstick_df, order_df, trade_df, csbar1d_df, True)}

# 多合约
data = {
    "000488.SZ": (cstick_df1, order_df1, trade_df1, csbar1d_df1),
    "600519.SH": (cstick_df2, order_df2, trade_df2, csbar1d_df2),
}
```

---

## 4. 核心接口

### 事件处理函数（需返回 `List[UserEvent]`）

| 函数 | 触发时机 | 说明 |
|------|---------|------|
| `onTickEvent(snapshot)` | 收到 Tick 快照 | 主要策略逻辑入口 |
| `onOrderEvent(order)` | 收到逐笔委托 | 可用于观察市场委托流 |
| `onTradeEvent(trade)` | 收到逐笔成交 | 可用于观察市场成交流 |
| `onCustomEvent(data)` | 收到自定义数据推送 | 需开启 `enable_custom_data` |
| `onRealTimeTickEvent(snapshot)` | 收到实时合成 Tick | 需开启 `realtime_tick_interval_ms`，详见第 13 节 |

### 回调函数（无返回值）

| 函数 | 触发时机 | 说明 |
|------|---------|------|
| `onOrderCallback(cb)` | 下单确认 | 你的订单已被引擎接受 |
| `onTradeCallback(cb)` | 成交或撤单 | `cb.matchtype='T'` 成交，`'D'` 撤单 |

---

## 5. 下单接口

```python
from wangcai_syn import make_order

order_event = make_order(
    Broker='',                    # 券商代码（可为空）
    Account='user1',              # 账户标识（= getStrategyId 返回值）
    Exchange=1,                   # 交易所: 0=上海, 1=深圳
    Instrument='000488.SZ',       # 合约代码
    OrderLocalID='ORDER_001',     # 订单本地ID（自定义，用于追踪）
    OrderType=0,                  # 订单类型: 0=限价单
    Direction=1,                  # 方向: 1=买入, 2=卖出
    Price=80000,                  # 价格（厘）: 8元 = 80000厘
    Volume=100                    # 数量（股）
)
```

## 6. 撤单接口

```python
from wangcai_syn import make_cancel

cancel_event = make_cancel(
    Broker='',                        # 券商代码（可为空）
    Account='user1',                  # 账户标识
    Exchange=1,                       # 交易所
    Instrument='000488.SZ',           # 合约代码
    CancelOrderLocalID='CANCEL_001',  # 撤单请求ID（自定义）
    OrderLocalID='ORDER_001'          # 要撤销的订单ID
)
```

## 7. 回调字段说明

### onOrderCallback(cb) 字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `cb.orderlocalid` | str | 订单本地ID |
| `cb.direction` | int | 1=买入, 2=卖出 |
| `cb.price` | int | 下单价格（厘） |
| `cb.volume` | int | 下单数量 |
| `cb.bid1` | int | 当时买一价（厘） |
| `cb.ask1` | int | 当时卖一价（厘） |
| `cb.deltapos` | int | 当前总持仓量 |
| `cb.time` | str | 下单时间 |

### onTradeCallback(cb) 字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `cb.matchtype` | char | `'T'`=成交, `'D'`=撤单 |
| `cb.localid` | str | 订单本地ID |
| `cb.direction` | char | `'B'`=买入, `'S'`=卖出 |
| `cb.price` | int | 成交价格（厘） |
| `cb.volume` | int | 成交数量 |
| `cb.matchamount` | float | 成交金额（元） |
| `cb.deltapos` | int | 成交后总持仓量 |
| `cb.matchtime` | str | 成交时间 |

### Snapshot 快照字段

```python
def onTickEvent(self, snapshot):
    snapshot.Exchange       # 交易所代码 (0=SH, 1=SZ)
    snapshot.Instrument     # 合约代码
    snapshot.Time           # 时间戳 (HHMMSSmmm 格式，如 93000000)
    snapshot.PreClose       # 昨收价（厘）
    snapshot.Open           # 开盘价
    snapshot.High           # 最高价
    snapshot.Low            # 最低价
    snapshot.last_price     # 最新价
    snapshot.UpperLimit     # 涨停价
    snapshot.LowerLimit     # 跌停价
    snapshot.bids           # 买盘价格数组（10档，厘）
    snapshot.asks           # 卖盘价格数组（10档，厘）
    snapshot.bid_sizes      # 买盘数量数组（10档，股）
    snapshot.ask_sizes      # 卖盘数量数组（10档，股）
    snapshot.Volume         # 累计成交量
    snapshot.Turnover       # 累计成交额
```

---

## 8. 价格单位

系统内部价格单位为**厘**（1 元 = 10000 厘）

```python
price_yuan = 8.50                    # 元
price_li = int(price_yuan * 10000)   # 厘 -> 85000
price_yuan = price_li / 10000        # 元 -> 8.50
```

---

## 9. 严格主动单模式

开启后，虚拟主动单只能成交市场实际提供的量。吃掉的历史订单记入"欠债"，欠债未清前禁止下新主动单。被动单不受影响。

```python
run_backtest(data, strategy, strict_active_order_mode=True)
```

**行为说明：**
- 主动单（价格穿越对手盘）：只吃对手方实际可用量，剩余自动撤单
- 成交后产生"欠债"：被吃掉的历史订单需要被真实市场消耗后才能解锁
- 有欠债时：新的主动单会被拒绝（收到 `matchtype='D'` 撤单回调）
- 被动单（价格不穿越对手盘）：正常挂单，不受欠债限制

---

## 10. 真实成交替代模式（v1.3.4 新增）

开启后，虚拟被动单只有在同价位发生了真实历史成交时才能成交，且量用完就没有了。用于更真实地估计交易成本。

```python
run_backtest(data, strategy, real_trade_match_mode=True)
```

**行为说明：**
- **主动单**：自动使用严格主动单模式（同第 9 节）
- **被动单排队**：下单时记录前方排队量，只有真实成交消耗完前方排队量后才轮到你
- **真实成交池**：每笔真实历史成交的量累加到成交池，虚拟被动单从池中消耗
- **部分成交**：被动单可以多次部分成交，每次回调 `matchtype='T'`
- **撤单推进**：只有你入场之前的历史单被撤才推进你的排队位置

> 注意：`real_trade_match_mode=True` 会自动开启 `strict_active_order_mode`，二者无需同时设置

**示例：**

```python
from wangcai_syn import run_backtest, Strategy, make_order

class RTStrategy(Strategy):
    def __init__(self):
        super().__init__()
        self.account = "rt_user"
        self.placed = False

    def getStrategyId(self):
        return self.account

    def onTickEvent(self, tick):
        events = []
        if not self.placed and tick.Time >= 93100000 and tick.bids[0] > 0:
            events.append(make_order(
                Broker='', Account=self.account,
                Exchange=tick.Exchange, Instrument=tick.Instrument,
                OrderLocalID='RT_BUY_001', OrderType=0,
                Direction=1,
                Price=tick.bids[0],   # 买一价挂单（被动单）
                Volume=100
            ))
            self.placed = True
        return events

    def onTradeCallback(self, cb):
        if cb.matchtype == 'T':
            print(f"[RT成交] {cb.localid} {cb.volume}@{cb.price/10000:.4f} 持仓={cb.deltapos}")
        elif cb.matchtype == 'D':
            print(f"[RT撤单] {cb.localid}")

    def onOrderCallback(self, cb):
        print(f"[RT下单确认] {cb.orderlocalid}")

    def onOrderEvent(self, order): return []
    def onTradeEvent(self, trade): return []
    def onOrderFilled(self, order_id, price, volume): pass
    def onOrderCancelled(self, order_id, reason): pass

# 运行
run_backtest(data, RTStrategy(), real_trade_match_mode=True)
```

---

## 11. 自定义数据推送

允许注入自定义数据（如因子信号），系统会在指定时间戳推送给策略。

```python
import pandas as pd
from wangcai_syn import run_backtest

custom_df = pd.DataFrame({
    'datetime': ['2025-11-17 09:35:00', '2025-11-17 10:00:00'],
    'signal': ['buy', 'hold'],
    'target_price': [44.5, 0]
})

run_backtest(
    data_dict=data,
    strategy=strategy,
    custom_data=custom_df,
    enable_custom_data=True
)
```

**策略中接收：**

```python
def onCustomEvent(self, data: dict):
    # data = {'datetime': '2025-11-17 09:35:00', 'signal': 'buy', 'target_price': 44.5}
    if data.get('signal') == 'buy':
        return [make_order(...)]
    return []
```

---

## 12. 三种模式对比

| 模式 | 主动单行为 | 被动单行为 | 适用场景 |
|------|-----------|-----------|---------|
| 默认模式 | 立即全部成交 | 等历史订单移除后成交 | 快速验证策略逻辑 |
| 严格主动单模式 | 只吃实际可用量 + 欠债限制 | 同默认 | 估计主动单冲击成本 |
| 真实成交替代模式 | 自动严格模式 | 只用真实成交量，排队等待 | 精确估计交易成本 |

---

## 13. 实时合成 Tick（onRealTimeTickEvent）

真实 Tick 快照约 3 秒一条。开启本功能后，引擎按设定的毫秒间隔（如 10/50/100ms）
从内部维护的订单簿合成十档快照，推送给策略的 `onRealTimeTickEvent` 回调，
可以在两条真实 Tick 之间以更细的粒度观察盘口。

```python
class MyStrategy(Strategy):
    def onRealTimeTickEvent(self, snapshot):
        # snapshot 字段与 onTickEvent 的真实 Tick 完全一致
        bid1, ask1 = snapshot.bids[0], snapshot.asks[0]
        return []  # 同样可以返回下单/撤单事件

run_backtest(data, strategy, realtime_tick_interval_ms=100)  # 每 100ms 最多推一次
```

字段口径：

| 字段 | 来源 |
|------|------|
| 十档 `bids/asks/bid_sizes/ask_sizes` | 内部订单簿实时状态（只含历史订单，不含你的虚拟单） |
| `last_price` / `UpperLimit` / `LowerLimit` | 订单簿 |
| `TotalBidVol` / `TotalAskVol` | 订单簿全簿挂单总量 |
| `Volume` / `Turnover` / `NumTrades` / `High` / `Low` | 引擎按重建的历史成交逐笔累计，单调不减 |
| `Open` / `PreClose` / `Iopv` 等 | 承接最近一条真实 Tick |
| `AuctionPrice` / `AuctionQty` | 集合竞价阶段填预测价/预测量 |

注意事项：

- 本功能默认关闭，必须显式传入 `realtime_tick_interval_ms` 才启用，间隔由你指定。
- 回测时间只随市场事件前进：跨过间隔网格边界后的第一条市场事件处理完即推送，
  无事件的空白区间（如午休）不补发，同一网格不重复推送。
- 集合竞价阶段（09:15-09:25、14:57-15:00）盘口是未交叉的原始挂单分布，
  买一可能高于卖一，与连续竞价的十档语义不同。
- 累计字段是引擎重建口径，与官方 3 秒快照的数值出入来自时间戳口径不同
  （官方 tick 有独立时间戳），属正常现象，引擎不做对齐；合成 tick 内部
  各字段（盘口、最新价、累计量）彼此严格自洽。
- 10ms 间隔全天推送量可达数十万次，回调内请只做轻量计算。

---

## 14. 完整示例

参考 `user_example/` 目录：

```
user_example/
├── my_strategy.py                  # 策略模板（含完整注释）
├── run_example.py                  # 基础回测示例
├── run_strict_with_custom_data.py  # 严格模式 + 自定义数据示例
└── run_real_trade_mode.py          # 真实成交替代模式示例
```

---

## 15. 常见问题

**Q: 价格为什么是很大的整数？**
A: 为了避免浮点误差，系统使用厘作为价格单位，1 元 = 10000 厘

**Q: 如何获取当前持仓？**
A: 在 `onTradeCallback` 中通过 `callback.deltapos` 获取成交后的总持仓

**Q: 订单什么时候会成交？**
A: 取决于模式：默认模式下穿越价差立即成交；严格模式下受可用量限制；RT 模式下被动单需等真实成交消耗排队量

**Q: 支持多策略吗？**
A: 每次回测运行单个策略，如需多策略请分别运行

**Q: real_trade_match_mode 和 strict_active_order_mode 有什么关系？**
A: RT 模式自动包含严格主动单模式，开启 `real_trade_match_mode=True` 即可，不需要同时设 `strict_active_order_mode=True`

**Q: ETF 和股票有什么区别？**
A: ETF 使用三位小数（tick=0.001 元），股票使用两位小数（tick=0.01 元）。在 data_dict 中用 5 元组指定 `is_etf=True`
