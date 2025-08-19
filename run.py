'''
Author: chenlisen
Date: 2025-08-18 12:40:34
LastEditTime: 2025-08-18 12:50:44
FilePath: /wangcai_cpp/run.py
'''

import wangcai_bt
from wangcai_bt import MultiBacktestEngine, Strategy, Direction, OrderType


class PrintStrategy(Strategy):
    """打印所有收到的订单、成交和行情事件"""
    def __init__(self):
        super().__init__()

    def getStrategyId(self) -> str:
        """返回策略 ID"""
        return "print"

    def onOrderEvent(self, event):
        """处理逐笔委托"""
        print(f"{event.datetime} | {event.sym} | ord | price={event.price} size={event.size} "
              f"side={event.side} ordertype={event.ordertype}")
        return []  # 不生成额外指令

    def onTradeEvent(self, execution, datetime):
        """处理逐笔成交"""
        print(f"{datetime} | trade | price={execution.price} volume={execution.volume} "
              f"buy_order_id={execution.buy_order_id} sell_order_id={execution.sell_order_id}")
        return []

    def onTickEvent(self, snapshot):
        """处理行情快照事件"""
        print(f"{snapshot.datetime} | {snapshot.Instrument} | tick | last_price={snapshot.last_price}")
        return []


def main() -> None:
    symbols = ["002179.SZ", "002466.SZ"] 
    date = "2024-12-19" 
    data_path = "logs/"

    # 创建多股票回测引擎
    engine = MultiBacktestEngine(symbols, date, data_path)
    # 注册策略
    strategy = PrintStrategy()
    engine.registerStrategy(strategy)
    # 运行回测
    engine.run()
    # 打印持仓和盈亏
    positions = engine.getPositions()
    total_pnl = engine.getTotalPnL()
    print("最终持仓：", positions)
    print("总盈亏：", total_pnl)


if __name__ == "__main__":
    main()