'''
Author: chenlisen
Date: 2025-08-15 04:27:56
LastEditTime: 2025-08-18 12:36:56
FilePath: /wangcai_cpp/wangcai_bt/src/wangcai_bt/__init__.py
'''
from .wangcai_cpp import (
    # 引擎
    BacktestEngine,
    Strategy,
    
    # 枚举
    Direction,
    OrderType,
    
    # 数据结构
    Event,          
    UserEvent,
    UserOrder,
    UserCancel,
    Execution,
    Position,
    MarketData,
    Snapshot,
    TradeCallback,
    OrderCallback,
    
    # 辅助函数
    make_order_event,
    make_cancel_event,
    # 多合约回测引擎
    MultiBacktestEngine
)

# 导出所有
__all__ = [
    'BacktestEngine',
    'Strategy',
    'Direction',
    'OrderType',
    'Event',         
    'UserEvent',
    'UserOrder', 
    'UserCancel',
    'Execution',
    'Position',
    'MarketData',
    'Snapshot',
    'TradeCallback',
    'OrderCallback',
    'make_order_event',
    'make_cancel_event',
    'MultiBacktestEngine'
]