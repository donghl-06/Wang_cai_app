# 旺财回测平台 v1.3.0

高性能逐笔回测引擎

## 📦 安装

```bash
pip install wangcai_syn-1.3.0-cp312-cp312-linux_x86_64.whl
```

## 🚀 快速开始

### 1. 准备数据

将以下4个CSV文件放入 `data/` 目录:

| 文件 | 说明 |
|------|------|
| `cstick_{symbol}_{date}.csv` | Tick快照数据 |
| `csord_{symbol}_{date}.csv` | 逐笔委托数据 |
| `cstra_{symbol}_{date}.csv` | 逐笔成交数据 |
| `csbar1d_{symbol}_{date}.csv` | 日线数据（含涨跌停） |

### 2. 编写策略

参考 `my_strategy.py`：

```python
from wangcai_syn import Strategy, make_order, make_cancel

class MyStrategy(Strategy):
    def __init__(self, account="user1"):
        super().__init__()
        self.account = account
    
    def getStrategyId(self) -> str:
        return self.account
    
    def onTickEvent(self, tick):
        # 你的交易逻辑
        events = []
        
        # 下单示例
        events.append(make_order(
            Account=self.account,
            Exchange=tick.Exchange,
            Instrument=tick.Instrument,
            OrderLocalID="order_001",
            Direction=1,      # 1=买, 2=卖
            Price=tick.bids[0],
            Volume=100
        ))
        
        return events
    
    def onTradeCallback(self, cb):
        if cb.matchtype == 'T':
            print(f"成交: {cb.volume}@{cb.price/10000:.4f}")
        elif cb.matchtype == 'D':
            print(f"撤单: {cb.localid}")
```

### 3. 运行回测

```python
import pandas as pd
from wangcai_syn import run_backtest
from my_strategy import MyStrategy

# 加载数据
symbol = "688503.SH"
date = "2025-11-17"

data = {
    symbol: (
        pd.read_csv(f"cstick_{symbol}_{date}.csv"),
        pd.read_csv(f"csord_{symbol}_{date}.csv"),
        pd.read_csv(f"cstra_{symbol}_{date}.csv"),
        pd.read_csv(f"csbar1d_{symbol}_{date}.csv"),
    )
}

# 运行
strategy = MyStrategy()
run_backtest(data, strategy, output_dir="./output")
```

或者直接运行示例：

```bash
cd user_example
python run_example.py
```

## 📁 文件说明

| 文件 | 说明 |
|------|------|
| `run_example.py` | 运行入口（配置参数后直接运行） |
| `my_strategy.py` | 策略模板（修改这个文件） |

## 📊 数据格式

### csbar1d（日线数据）

```csv
sym,prevclose,upperlimit,lowerlimit,...
688503.SH,58.00,69.60,46.40,...
```

- `upperlimit`: 涨停价（元）
- `lowerlimit`: 跌停价（元）

### Tick数据时间格式

`tick.Time` 是整数格式：`HHMMSSmmm`

```python
# 解析示例
time_int = tick.Time  # 如 93015123 = 09:30:15.123
ms = time_int % 1000          # 123
second = (time_int // 1000) % 100    # 15
minute = (time_int // 100000) % 100  # 30
hour = time_int // 10000000          # 9
```

## 🔧 常见问题

**Q: 订单被拒绝，提示 "price out of limit"?**

A: 检查 `csbar1d` 中的涨跌停价格是否正确

**Q: 回调没有触发?**

A: 确保 `getStrategyId()` 返回的ID与下单时的 `Account` 一致

## 📝 涨跌停限制

| 市场 | 限制 |
|------|------|
| 主板 (60/00) | ±10% |
| 科创板 (688) | ±20% |
| 创业板 (300/301) | ±20% |
| ST股 | ±5% |
