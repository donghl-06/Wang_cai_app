"""
旺财回测引擎 - 高性能股票回测框架

这个包提供了基于C++的高性能订单簿回测引擎，支持：
- 逐笔行情回测
- 集合竞价模拟
- 策略接口
- 多标的并行回测

快速开始:
    from wangcai_syn import run_backtest, Strategy
    import pandas as pd
    
    # 1. 准备数据
    cstick_df = pd.read_csv("cstick.csv")
    order_df = pd.read_csv("order.csv")
    trade_df = pd.read_csv("trade.csv")
    
    # 2. 创建策略（继承 Strategy 并重载事件处理函数）
    class MyStrategy(Strategy):
        def getStrategyId(self):
            return "MY_STRATEGY"
        
        def onTickEvent(self, snapshot):
            # 处理Tick事件
            return []
    
    # 3. 运行回测
    data = {"000488.SZ": (cstick_df, order_df, trade_df)}
    run_backtest(data, MyStrategy())
"""

from wangcai_syn.wangcai_cpp import (
    # 核心引擎
    BacktestEngine,
    MultiBacktestEngine,
    Strategy,
    
    # 枚举类型
    Direction,
    OrderType,
    
    # 事件和数据结构
    Event,
    UserEvent,
    Execution,
    Snapshot,
    OrderDetail,
    TradeDetail,
    SymbolData,
    
    # 回调类型
    TradeCallback,
    OrderCallback,
    
    # 辅助函数
    make_order_event,
    make_cancel_event,
)

# 导入工具函数
from wangcai_syn.utils import (
    create_symbol_data,
    create_multi_symbol_data,
    run_backtest,
    make_order,
    make_cancel,
    convert_price_to_li,
    align_price,
    format_price,
)

# 导入策略基类
from wangcai_syn.strategy_base import InterfaceTestStrategy

# 版本信息
__version__ = "1.4.0"
__author__ = "aitopia"

# 导出所有公共接口
__all__ = [
    # 核心引擎
    "BacktestEngine",
    "MultiBacktestEngine", 
    "Strategy",
    
    # 枚举类型
    "Direction",
    "OrderType",
    
    # 事件和数据结构
    "Event",
    "UserEvent",
    "Execution",
    "Snapshot",
    "OrderDetail",
    "TradeDetail",
    "SymbolData",
    
    # 回调类型
    "TradeCallback",
    "OrderCallback",
    
    # 辅助函数
    "make_order_event",
    "make_cancel_event",
    
    # 工具函数
    "create_symbol_data",
    "create_multi_symbol_data",
    "run_backtest",
    "make_order",
    "make_cancel",
    "convert_price_to_li",
    "align_price",
    "format_price",
    
    # 策略范例
    "InterfaceTestStrategy",
    
    # 版本
    "__version__",
    "__author__",
]
