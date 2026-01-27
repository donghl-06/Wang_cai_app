# 旺财回测平台 v1.3.2

高性能逐笔回测引擎

## 安装

```bash
pip install wangcai_syn-1.3.2-cp312-cp312-linux_x86_64.whl
```

## 快速开始

### 1. 准备数据

将以下4个CSV文件放入数据目录：

| 文件 | 说明 |
|------|------|
| `cstick_{symbol}_{date}.csv` | Tick快照数据 |
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
        """Tick回调 - 处理行情快照"""
        events = []
        # 下单示例
        if tick.bids[0] > 0:
            events.append(wangcai_syn.make_order(
                Broker='',
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID="order_001",
                OrderType=0,      # 限价单
                Direction=1,      # 1=买, 2=卖
                Price=tick.bids[0],
                Volume=100
            ))
        return events
    
    def onOrderEvent(self, order):
        """逐笔委托回调（可选）"""
        return []
    
    def onTradeEvent(self, trade):
        """逐笔成交回调（可选）"""
        return []
    
    def onTradeCallback(self, cb):
        """成交/撤单回报"""
        if cb.matchtype == 'T':
            print(f"成交: {cb.volume}@{cb.price/10000:.4f}")
        elif cb.matchtype == 'D':
            print(f"撤单: {cb.localid}")
    
    def onOrderCallback(self, cb):
        """下单确认回报"""
        print(f"下单确认: {cb.orderlocalid}")
```

### 3. 运行回测

```python
import pandas as pd
import wangcai_syn
from my_strategy import MyStrategy

# 加载数据
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

# 运行回测
strategy = MyStrategy()
wangcai_syn.run_backtest(data, strategy, output_dir="./output")
```

或直接运行示例：

```bash
cd user_example
python run_example.py
```

## 示例文件说明

| 文件 | 说明 |
|------|------|
| `run_example.py` | 单合约回测示例 |
| `run_example_multi.py` | 顺序多合约回测（依次测试多只股票） |
| `run_strict_with_custom_data.py` | 综合示例：严格主动单模式 + 自定义数据推送 |
| `my_strategy.py` | 策略模板 |

## 高级功能

### 严格主动单模式

启用后，虚拟主动单会受到以下限制：
- 只能成交市场实际提供的量，剩余部分自动撤单
- 主动单"吃掉"历史订单后会产生"欠债"
- 欠债未还清前，禁止下新的主动单

```python
wangcai_syn.run_backtest(
    data_dict=data,
    strategy=strategy,
    strict_active_order_mode=True  # 启用严格主动单模式
)
```

### 自定义数据推送

支持传入用户自定义的时间序列数据，引擎会在对应时间点推送给策略。

```python
import pandas as pd

# 构造自定义数据（必须有 datetime 列）
custom_df = pd.DataFrame({
    "datetime": ["2025-11-17 09:35:00", "2025-11-17 10:00:00"],
    "signal": ["buy", "sell"],
    "target_price": [44.50, 45.00]
})

# 策略中实现 onCustomEvent 回调
class MyStrategy(wangcai_syn.Strategy):
    def onCustomEvent(self, data: dict):
        """收到自定义数据回调"""
        signal = data.get('signal', '')
        if signal == 'buy':
            # 下单逻辑...
            return [order]
        return []

# 运行回测
wangcai_syn.run_backtest(
    data_dict=data,
    strategy=strategy,
    custom_data=custom_df,       # 自定义数据
    enable_custom_data=True      # 启用推送
)
```

## 数据格式

### csbar1d（日线数据）

```csv
sym,prevclose,upperlimit,lowerlimit,...
300827.SZ,44.00,52.80,35.20,...
```

- `upperlimit`: 涨停价（元）
- `lowerlimit`: 跌停价（元）

### Tick时间格式

`tick.Time` 是整数格式：`HHMMSSmmm`

```python
time_int = tick.Time  # 如 93015123 = 09:30:15.123
ms = time_int % 1000          # 123
second = (time_int // 1000) % 100    # 15
minute = (time_int // 100000) % 100  # 30
hour = time_int // 10000000          # 9
```

### 价格单位

系统内部使用**厘**作为价格单位（1元 = 10000厘）

```python
# 元 → 厘
price_li = int(price_yuan * 10000)

# 厘 → 元
price_yuan = price_li / 10000
```

## 常见问题

**Q: 订单被拒绝，提示 "price out of limit"?**

A: 检查 `csbar1d` 中的涨跌停价格是否正确

**Q: 回调没有触发?**

A: 确保 `getStrategyId()` 返回的ID与下单时的 `Account` 一致

**Q: 上海股票订单数据不完整?**

A: 系统会自动进行上海股票订单还原，从成交数据中提取被动方订单

## 涨跌停限制

| 市场 | 限制 |
|------|------|
| 主板 (60/00) | ±10% |
| 科创板 (688) | ±20% |
| 创业板 (300/301) | ±20% |
| ST股 | ±5% |
