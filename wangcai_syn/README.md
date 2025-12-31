# 旺财回测引擎 (WangCai Syn)

高性能股票回测框架，基于 C++ 实现的订单簿引擎，提供 Python 接口。

## ✨ 特性

- 🚀 **高性能**：基于 C++20 实现，支持并行处理
- 📊 **逐笔回测**：支持逐笔行情回测，精确还原市场
- 🎯 **集合竞价**：完整模拟开盘、收盘集合竞价
- 🔧 **灵活接口**：Python 策略接口，易于开发
- 📈 **多标的**：支持多标的并行回测
- 💾 **完整记录**：记录所有订单、成交、持仓变化

## 📦 安装

### 从 PyPI 安装

```bash
pip install wangcai-syn
```

## 🚀 快速开始

### 1. 准备数据

需要三个 CSV 文件：
- `cstick_<symbol>_<date>.csv` - Tick 行情数据
- `csord_<symbol>_<date>.csv` - 订单数据
- `cstra_<symbol>_<date>.csv` - 成交数据

### 2. 编写策略

```python
from wangcai_syn import Strategy, Direction, OrderType, make_order_event

class MyStrategy(Strategy):
    def __init__(self, strategy_id="MY_STRATEGY"):
        super().__init__()
        self.strategy_id = strategy_id
        self.order_counter = 0
    
    def getStrategyId(self):
        return self.strategy_id
    
    def onTickEvent(self, snapshot):
        """处理 Tick 事件"""
        events = []
        
        # 获取最新价格
        last_price = snapshot.last_price
        
        # 简单策略：买入
        if last_price > 0:
            order_id = f"{self.strategy_id}_{self.order_counter}"
            self.order_counter += 1
            
            event = make_order_event(
                order_id=order_id,
                symbol=snapshot.Instrument,
                direction=Direction.Buy,
                order_type=OrderType.Limit,
                price=last_price,
                volume=100,
                strategy_id=self.strategy_id
            )
            events.append(event)
        
        return events
    
    def onTradeCallback(self, callback):
        """成交回调"""
        print(f"成交: {callback.localid}, 价格: {callback.price/10000:.2f}, "
              f"数量: {callback.volume}")
    
    def onOrderCallback(self, callback):
        """下单回调"""
        print(f"下单: {callback.orderlocalid}, 价格: {callback.price/10000:.2f}")
```

### 3. 运行回测

```python
from wangcai_syn import MultiBacktestEngine

# 创建引擎
engine = MultiBacktestEngine(
    symbols=["000488.SZ"],
    date="2024-12-19",
    data_path="./logs"
)

# 注册策略
strategy = MyStrategy("MY_STRATEGY")
engine.registerStrategy(strategy)

# 运行回测
engine.run()

# 获取结果
positions = engine.getPositions()
pnl = engine.getTotalPnL()

print(f"最终持仓: {positions}")
print(f"总盈亏: {pnl:.2f} 元")
```

## 📖 API 文档

### 核心类

#### BacktestEngine
单标的回测引擎

```python
engine = BacktestEngine(symbol, date, data_path)
engine.registerStrategy(strategy)
engine.run()
```

#### MultiBacktestEngine
多标的并行回测引擎

```python
engine = MultiBacktestEngine(symbols, date, data_path)
engine.registerStrategy(strategy)
engine.run()
positions = engine.getPositions()
pnl = engine.getTotalPnL()
```

#### Strategy
策略基类，需要继承并实现以下方法：

- `getStrategyId()` - 返回策略ID
- `onTickEvent(snapshot)` - 处理 Tick 事件
- `onOrderEvent(order)` - 处理订单事件
- `onTradeEvent(trade)` - 处理成交事件
- `onOrderCallback(callback)` - 下单回调
- `onTradeCallback(callback)` - 交易回调

### 枚举类型

#### Direction
订单方向
- `Direction.Buy` - 买入
- `Direction.Sell` - 卖出

#### OrderType
订单类型
- `OrderType.Limit` - 限价单
- `OrderType.Market` - 市价单

### 辅助函数

#### make_order_event
创建下单事件

```python
event = make_order_event(
    order_id="ORDER_001",
    symbol="000488.SZ",
    direction=Direction.Buy,
    order_type=OrderType.Limit,
    price=79800,  # 价格单位：厘（1元=10000厘）
    volume=100,
    strategy_id="MY_STRATEGY"
)
```

#### make_cancel_event
创建撤单事件

```python
event = make_cancel_event(
    order_id="ORDER_001",
    strategy_id="MY_STRATEGY"
)
```

## 💡 示例策略

### 接口测试策略

```python
from wangcai_syn.strategy_base import InterfaceTestStrategy

# 使用内置的接口测试策略
strategy = InterfaceTestStrategy("TEST")
engine.registerStrategy(strategy)
engine.run()

# 保存测试结果
strategy.save_records("./output")
strategy.print_test_report()
```

## 📊 数据格式

### 价格单位
所有价格使用**厘**作为单位（1元 = 10000厘）

```python
# 7.98 元 = 79800 厘
price = 79800

# 转换为元
price_yuan = price / 10000  # 7.98
```

### Snapshot（快照数据）
- `Exchange` - 交易所（0=上海，1=深圳）
- `Instrument` - 合约代码
- `last_price` - 最新价（厘）
- `asks` - 卖盘价格列表
- `bids` - 买盘价格列表
- `ask_sizes` - 卖盘数量列表
- `bid_sizes` - 买盘数量列表

## 🔧 开发

### 构建 C++ 扩展

```bash
# 清理构建
rm -rf build/ _skbuild/

# 重新构建
pip install -e . -v
```

### 运行测试

```bash
pytest tests/
```

