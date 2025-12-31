# 旺财回测平台 - 快速入门指南

## 📦 安装

```bash
pip install wangcai_syn-0.1.1-cp312-cp312-linux_x86_64.whl
```

## 🚀 5分钟快速开始

### 1. 准备数据

回测需要三个 DataFrame：

| 数据类型 | 说明 | 文件示例 |
|---------|------|---------|
| `cstick_df` | Tick 快照数据 | `cstick_000488.SZ_2024-12-19.csv` |
| `order_df` | 逐笔委托数据 | `csord_000488.SZ_2024-12-19.csv` |
| `trade_df` | 逐笔成交数据 | `cstra_000488.SZ_2024-12-19.csv` |

```python
import pandas as pd

# 从本地文件加载
cstick_df = pd.read_csv("cstick_000488.SZ_2024-12-19.csv")
order_df = pd.read_csv("csord_000488.SZ_2024-12-19.csv")
trade_df = pd.read_csv("cstra_000488.SZ_2024-12-19.csv")

# 或从数据库加载
# cstick_df = pd.read_sql("SELECT * FROM tick WHERE symbol='000488.SZ'", engine)
```

### 2. 编写策略

继承 `Strategy` 类，重载事件处理函数：

```python
from wangcai_syn import Strategy, make_order

class MyStrategy(Strategy):
    def __init__(self):
        super().__init__()
        self.order_count = 0
        self.account = "user1"
    
    def getStrategyId(self):
        """返回账户标识（必须重载）"""
        return self.account
    
    def onTickEvent(self, snapshot):
        """
        处理Tick快照推送
        
        Returns:
            List[UserEvent]: 下单/撤单事件列表
        """
        events = []
        
        # 示例：在买一价下一个买单
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
        """成交回调"""
        print(f"成交: {callback.localid} {callback.volume}@{callback.price/10000:.4f}")
    
    def onOrderCallback(self, callback):
        """下单回调"""
        print(f"下单确认: {callback.orderlocalid}")
```

### 3. 运行回测

```python
from wangcai_syn import run_backtest

# 组织数据（支持多合约）
data = {
    "000488.SZ": (cstick_df, order_df, trade_df)
}

# 创建策略并运行（单策略）
strategy = MyStrategy()
run_backtest(data, strategy, output_dir="./output")
```

## 📋 核心接口

### 事件处理函数

| 函数 | 触发时机 | 返回值 |
|------|---------|--------|
| `onTickEvent(snapshot)` | 收到Tick快照 | `List[UserEvent]` |
| `onOrderEvent(order)` | 收到逐笔委托 | `List[UserEvent]` |
| `onTradeEvent(trade)` | 收到逐笔成交 | `List[UserEvent]` |

### 回调函数

| 函数 | 触发时机 |
|------|---------|
| `onOrderCallback(callback)` | 下单确认 |
| `onTradeCallback(callback)` | 成交/撤单 |

## 📝 下单接口

```python
from wangcai_syn import make_order

order_event = make_order(
    Broker='',                    # 券商代码（可为空）
    Account='user1',              # 账户标识
    Exchange=1,                   # 交易所: 0=上海, 1=深圳
    Instrument='000488.SZ',       # 合约代码
    OrderLocalID='ORDER_001',     # 订单本地ID
    OrderType=0,                  # 订单类型: 0=限价单
    Direction=1,                  # 方向: 1=买入, 2=卖出
    Price=80000,                  # 价格（厘）: 8元 = 80000厘
    Volume=100                    # 数量
)
```

## 📝 撤单接口

```python
from wangcai_syn import make_cancel

cancel_event = make_cancel(
    Broker='',                    # 券商代码（可为空）
    Account='user1',              # 账户标识
    Exchange=1,                   # 交易所
    Instrument='000488.SZ',       # 合约代码
    CancelOrderLocalID='CANCEL_001',  # 撤单请求ID
    OrderLocalID='ORDER_001'      # 要撤销的订单ID
)
```

## 💰 价格单位

**重要**：系统内部价格单位为**厘**（1元 = 10000厘）

```python
# 价格转换
price_yuan = 8.50      # 元
price_li = 85000       # 厘

# 元 → 厘
price_li = int(price_yuan * 10000)

# 厘 → 元
price_yuan = price_li / 10000
```

## 📊 Snapshot 快照字段

```python
def onTickEvent(self, snapshot):
    # 基础信息
    snapshot.Exchange      # 交易所代码
    snapshot.Instrument    # 合约代码
    snapshot.Time          # 时间戳 (HHMMSSmmm)
    
    # 价格信息
    snapshot.last_price    # 最新价（厘）
    snapshot.PreClose      # 昨收价
    snapshot.Open          # 开盘价
    snapshot.High          # 最高价
    snapshot.Low           # 最低价
    
    # 盘口数据（10档）
    snapshot.bids          # 买盘价格数组
    snapshot.asks          # 卖盘价格数组
    snapshot.bid_sizes     # 买盘数量数组
    snapshot.ask_sizes     # 卖盘数量数组
    
    # 成交统计
    snapshot.Volume        # 成交量
    snapshot.Turnover      # 成交额
    
    # 涨跌停
    snapshot.UpperLimit    # 涨停价
    snapshot.LowerLimit    # 跌停价
```

## 🔄 多合约回测

```python
# 多合约数据（单策略同时回测多个合约）
data = {
    "000488.SZ": (cstick_df1, order_df1, trade_df1),
    "600519.SH": (cstick_df2, order_df2, trade_df2),
    "000001.SZ": (cstick_df3, order_df3, trade_df3),
}

# 统一运行
run_backtest(data, strategy)
```

## 📁 完整示例

参考 `user_example/` 目录：

```
user_example/
├── main.py              # 主程序入口
├── strategy_example.py  # 策略范例（含完整注释）
├── strategy_test.py     # 接口验证策略
└── run_test.py          # 运行接口验证
```

### 接口验证策略 (strategy_test.py)

快速验证下单、撤单、成交功能：

```python
from strategy_test import TestStrategy
from wangcai_syn import run_backtest

strategy = TestStrategy(account="test")
run_backtest(data, strategy)
```

验证流程：
1. 下5个买单（低于市价，不成交）→ 验证下单回调
2. 下5个卖单（市价，尝试成交）→ 验证成交回调
3. 撤销未成交的买单 → 验证撤单回调

## ❓ 常见问题

### Q: 价格为什么是很大的整数？
A: 系统使用**厘**作为价格单位，8元 = 80000厘

### Q: 如何获取当前持仓？
A: 在 `onTradeCallback` 中通过 `callback.deltapos` 获取成交后的持仓

### Q: 订单什么时候会成交？
A: 当订单价格达到对手盘价格时立即成交（同步模式，无延迟）

### Q: 支持多策略吗？
A: 每次回测运行单个策略，如需多策略请分别运行
