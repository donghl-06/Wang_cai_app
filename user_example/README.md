# 旺财回测平台 v1.3.4

高性能逐笔回测引擎

## 安装

```bash
pip install wangcai_syn==1.3.4 -i https://pypi.aitopia.tech/simple/
```

## 快速开始

### 1. 准备数据

将以下 4 个 CSV 文件放入数据目录：

| 文件 | 说明 |
|------|------|
| `cstick_{symbol}_{date}.csv` | Tick 快照数据 |
| `csord_{symbol}_{date}.csv` | 逐笔委托数据 |
| `cstra_{symbol}_{date}.csv` | 逐笔成交数据 |
| `csbar1d_{symbol}_{date}.csv` | 日线数据（含涨跌停） |

### 2. 编写策略

```python
import wangcai_syn

class MyStrategy(wangcai_syn.Strategy):
    def __init__(self, account="user1"):
        super().__init__()
        self.account = account

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick):
        events = []
        if tick.bids[0] > 0:
            events.append(wangcai_syn.make_order(
                Broker='',
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID="order_001",
                OrderType=0,        # 限价单
                Direction=1,        # 1=买, 2=卖
                Price=tick.bids[0],  # 价格（厘）
                Volume=100
            ))
        return events

    def onOrderEvent(self, order): return []
    def onTradeEvent(self, trade): return []

    def onTradeCallback(self, cb):
        if cb.matchtype == 'T':
            print(f"成交: {cb.localid} {cb.volume}@{cb.price/10000:.4f} 持仓={cb.deltapos}")
        elif cb.matchtype == 'D':
            print(f"撤单: {cb.localid}")

    def onOrderCallback(self, cb):
        print(f"下单确认: {cb.orderlocalid}")

    def onOrderFilled(self, order_id, price, volume): pass
    def onOrderCancelled(self, order_id, reason): pass
```

### 3. 运行回测

```python
import pandas as pd
import wangcai_syn

symbol = "300827.SZ"
date = "2025-11-17"

data = {
    symbol: (
        pd.read_csv(f"cstick_{symbol}_{date}.csv"),
        pd.read_csv(f"csord_{symbol}_{date}.csv"),
        pd.read_csv(f"cstra_{symbol}_{date}.csv"),
        pd.read_csv(f"csbar1d_{symbol}_{date}.csv"),
    )
}

strategy = MyStrategy()
wangcai_syn.run_backtest(data, strategy, output_dir="./output")
```

或直接运行示例：

```bash
cd user_example
python run_example.py
```

---

## 示例文件说明

| 文件 | 说明 | 演示功能 |
|------|------|---------|
| `my_strategy.py` | 策略模板 | 完整策略结构、下单、回调处理 |
| `run_example.py` | 基础回测示例 | 单合约默认模式回测 |
| `run_strict_with_custom_data.py` | 严格模式 + 自定义数据 | 欠债限制 + 按信号下单 |
| `run_real_trade_mode.py` | 真实成交替代模式 | 被动单排队 + 主动单严格模式 |
| `run_queue_info_test.py` | 队列信息回调测试 | 下单回调新增字段验证 |
| `run_etf_test.py` | ETF 回测测试 | ETF 三位小数精度 |
| `run_order_latency_test.py` | 下单延迟模拟 | 链路时延、进簿位置选择 |
| `run_event_snapshot_test.py` | 事件驱动快照 | 逐事件十档盘口回调 |
| `run_realtime_tick_test.py` | 实时合成 Tick | 自定间隔盘口推送 |
| `run_price_cage_test.py` | 价格笼子 | 笼内接受 / 笼外废单 |

---

## run_backtest 参数一览

```python
wangcai_syn.run_backtest(
    data_dict,                          # 必填，回测数据
    strategy,                           # 必填，策略实例
    output_dir=None,                    # 输出目录
    strict_active_order_mode=False,     # 严格主动单模式
    real_trade_match_mode=False,        # 真实成交替代模式
    queue_info_enabled=False,           # 下单回调队列信息开关
    custom_data=None,                   # 自定义数据 DataFrame
    enable_custom_data=False,           # 启用自定义数据推送
    release_input=False,                # 转换后清空输入数据省内存
    realtime_tick_interval_ms=0,        # 实时合成 Tick 间隔(0=关闭)
    event_snapshot_enabled=False,       # 事件驱动快照开关
    user_cage_enabled=True,             # 策略单价格笼子判定
    order_latency_enabled=False,        # 下单延迟开关
    order_latency_ms=0,                 # 延迟时长(0=开启时默认 20ms)
    latency_entry_position="head",      # 延迟单进簿位置 head/tail
)
```

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `data_dict` | dict | 必填 | `{symbol: (cstick, order, trade, csbar1d)}` |
| `strategy` | Strategy | 必填 | 策略实例 |
| `output_dir` | str | None | 输出目录，None=不保存 |
| `strict_active_order_mode` | bool | False | 主动单受欠债限制 |
| `real_trade_match_mode` | bool | False | 被动单只用真实成交量匹配 |
| `queue_info_enabled` | bool | False | 在 onOrderCallback 增加队列字段 |
| `custom_data` | DataFrame | None | 自定义数据（必须含 datetime 列） |
| `enable_custom_data` | bool | False | 启用自定义数据推送 |
| `release_input` | bool | False | 转换后清空 data_dict 省内存（数据无法再复用） |
| `realtime_tick_interval_ms` | int | 0 | 实时合成 Tick 间隔毫秒，0=关闭 |
| `event_snapshot_enabled` | bool | False | 每个市场事件后推送十档快照 |
| `user_cage_enabled` | bool | True | 策略单价格笼子判定（废单/暂存） |
| `order_latency_enabled` | bool | False | 下单/撤单延迟进簿开关 |
| `order_latency_ms` | int | 0 | 延迟时长，对齐 10ms 粒度，开启未指定时默认 20ms |
| `latency_entry_position` | str | "head" | 延迟单进簿位置："head"/"tail" |

---

## 三种回测模式

| 模式 | 参数 | 主动单 | 被动单 | 适用场景 |
|------|------|--------|--------|---------|
| 默认 | 无需设置 | 立即全部成交 | 等历史订单移除后成交 | 快速验证策略 |
| 严格主动单 | `strict_active_order_mode=True` | 只吃实际可用量 + 欠债限制 | 同默认 | 估计冲击成本 |
| 真实成交替代 | `real_trade_match_mode=True` | 自动严格模式 | 只用真实成交量，排队等待 | 精确估计交易成本 |

---

## 严格主动单模式

```python
wangcai_syn.run_backtest(data, strategy, strict_active_order_mode=True)
```

- 主动单只能成交对手方实际可用量，剩余自动撤单
- 吃掉的历史订单记入"欠债"，欠债未清前禁止下新主动单
- 被动单（价格不穿越对手盘）不受影响

---

## 真实成交替代模式（v1.3.4 新增）

```python
wangcai_syn.run_backtest(data, strategy, real_trade_match_mode=True)
```

- 被动单只有在同价位发生真实历史成交时才能成交
- 下单时记录前方排队量，真实成交消耗完才轮到你
- 支持部分成交（多次 `matchtype='T'` 回调）
- 主动单自动使用严格模式（无需同时设 `strict_active_order_mode`）

---

## 下单延迟模拟

```python
wangcai_syn.run_backtest(
    data, strategy,
    order_latency_enabled=True,
    order_latency_ms=50,              # 对齐 10ms 粒度: 14→10, 15→20
    latency_entry_position="head",    # 或 "tail"
)
```

- 策略在回调中返回的下单/撤单不立即进簿，延迟到 **发出时刻+latency** 才"到达交易所"：过涨跌停/价格笼子校验、进订单簿、参与撮合；下单确认回调也延迟到到达时刻才发出
- 撤单与下单走同一延迟通道（保 FIFO）；收盘（15:00）后才到达的订单被丢弃
- 实际进簿时机为到达时刻之后的第一个市场事件（回测时钟只随市场事件前进）
- 多笔订单同一时刻到达时，`"head"` 排在同时间订单头部（抢同价位排队优先级），`"tail"` 排在尾部
- 也可在策略 `__init__` 里 `self.setOrderLatencyMs(n)` / `self.setLatencyEntryPosition(LatencyEntryPosition.Tail)`，效果相同
- 适合评估策略对链路时延的敏感性：同一个信号，0ms 与 50ms 延迟的成交结果可能完全不同

示例：`python run_order_latency_test.py`

---

## 事件驱动快照与实时合成 Tick

两种比官方 3 秒快照更细粒度的盘口推送，快照口径一致（十档只含历史订单/公开订单簿）：

**事件驱动快照**（逐事件，最细）：

```python
wangcai_syn.run_backtest(data, strategy, event_snapshot_enabled=True)
```

每个市场事件（逐笔委托/逐笔成交含撤单）的全部处理——撮合 + 策略响应产生的下单/撤单——结束后，推送一次十档 Snapshot 到 `onEventSnapshot`；tick 事件不触发。集合竞价阶段的委托按竞价规则集中处理，不逐事件推送。

**实时合成 Tick**（按间隔网格，适中）：

```python
wangcai_syn.run_backtest(data, strategy, realtime_tick_interval_ms=100)
```

引擎每跨过一个间隔网格边界，从内部订单簿合成十档 Snapshot 推送到 `onRealTimeTickEvent`。字段结构与真实 3 秒 Tick 一致；`Volume/Turnover/NumTrades/High/Low` 由引擎按重建的历史成交逐笔累计，与官方快照的数值出入来自时间戳口径不同，属正常现象。无事件的空白区间（如午休）不补发。

示例：`python run_event_snapshot_test.py` / `python run_realtime_tick_test.py`

---

## 价格笼子（策略单）

策略限价单默认受交易所价格笼子约束（`user_cage_enabled=True`），规则按数据日期×板块自动判定：

| 板块 | 时期 | 行为 |
|------|------|------|
| 主板 | 2023-04-10 起 | ±2% 与 0.1 元孰高，超范围**废单** |
| 创业板 | 2020-08-24 ~ 2023-04-10 | **暂存模式**：超范围入笼，价格落回范围自动恢复参与撮合 |
| 创业板 | 2023-04-10 起 | 同主板（废单） |
| 科创板 | 2019-07-22 起 | 拒单（纯 ±2%），2023-04-10 后加 0.1 元兜底 |
| 北交所/ETF/债券 | - | 不适用 |

- 有效申报范围 = 基准价±2% 四舍五入至最小变动价位（基准价链：对手一档→本方一档→最新成交→昨收）
- 笼外单流程：先收到 `onOrderCallback` 申报回执，随后引擎废单，策略收到 `onTradeCallback` 的 `matchtype='D'` 回调，引擎日志打印废单原因
- 创业板暂存窗口内：笼外单不废，而是挂起等待价格落回后自动激活
- 另有涨跌停前置校验：超涨跌停的策略限价单直接废单（所有时代）
- `user_cage_enabled=False` 只关闭**策略单**的笼子判定，历史订单簿侧的笼子语义（交易所行为）始终生效

示例：`python run_price_cage_test.py`

---

## 自定义数据推送

```python
import pandas as pd

custom_df = pd.DataFrame({
    "datetime": ["2025-11-17 09:35:00", "2025-11-17 10:00:00"],
    "signal": ["buy", "sell"],
    "target_price": [44.50, 45.00]
})

wangcai_syn.run_backtest(
    data_dict=data,
    strategy=strategy,
    custom_data=custom_df,
    enable_custom_data=True
)
```

策略中实现 `onCustomEvent` 接收：

```python
def onCustomEvent(self, data: dict):
    if data.get('signal') == 'buy':
        return [make_order(...)]
    return []
```

---

## 回调字段速查

### onOrderCallback(cb)

| 字段 | 说明 |
|------|------|
| `cb.orderlocalid` | 订单本地ID |
| `cb.direction` | 1=买, 2=卖 |
| `cb.price` | 下单价格（厘） |
| `cb.volume` | 下单数量 |
| `cb.bid1` / `cb.ask1` | 当时盘口价（厘） |
| `cb.deltapos` | 当前持仓 |
| `cb.time` | 下单时间 |
| `cb.queue_ahead_count` | （可选）前方历史订单数量（需 queue_info_enabled=True） |
| `cb.queue_ahead_volume` | （可选）前方历史订单总量（需 queue_info_enabled=True） |
| `cb.prev_order_ids` | （可选）前方最近 3 个历史订单ID（真实 orderid） |

### onTradeCallback(cb)

| 字段 | 说明 |
|------|------|
| `cb.matchtype` | `'T'`=成交, `'D'`=撤单 |
| `cb.localid` | 订单本地ID |
| `cb.direction` | `'B'`=买, `'S'`=卖 |
| `cb.price` | 成交价格（厘） |
| `cb.volume` | 成交数量 |
| `cb.matchamount` | 成交金额（元） |
| `cb.deltapos` | 成交后持仓 |
| `cb.matchtime` | 成交时间 |

---

## 数据格式

### 价格单位

系统使用**厘**（1 元 = 10000 厘）

```python
price_li = int(price_yuan * 10000)   # 元 → 厘
price_yuan = price_li / 10000        # 厘 → 元
```

### Tick 时间格式

`tick.Time` 整数格式 `HHMMSSmmm`：

```python
time_int = tick.Time  # 93015123 = 09:30:15.123
ms     = time_int % 1000
second = (time_int // 1000) % 100
minute = (time_int // 100000) % 100
hour   = time_int // 10000000
```

### ETF 数据

ETF 使用三位小数（tick=0.001 元），在 data_dict 中用 5 元组指定：

```python
data = {"159001.SZ": (cstick, order, trade, csbar1d, True)}  # is_etf=True
```

---

## 常见问题

**Q: 订单被拒绝，提示 "price out of limit"?**
A: 检查 `csbar1d` 中的涨跌停价格是否正确

**Q: 回调没有触发?**
A: 确保 `getStrategyId()` 返回的 ID 与下单时的 `Account` 一致

**Q: 上海股票订单数据不完整?**
A: 系统会自动进行上海股票订单还原

**Q: real_trade_match_mode 和 strict_active_order_mode 同时设？**
A: 不需要，`real_trade_match_mode=True` 会自动包含严格模式

---

## 涨跌停限制

| 市场 | 限制 |
|------|------|
| 主板 (60/00) | ±10% |
| 科创板 (688) | ±20% |
| 创业板 (300/301) | ±20% |
| ST 股 | ±5% |
