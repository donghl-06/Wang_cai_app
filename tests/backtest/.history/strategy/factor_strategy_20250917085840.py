# import numpy as np
import os
import random
from dateutil import parser
from signal_gen.factor_calc import *
from utils.shared_logger import logger
from core.conditions import Timeline
# from utils.global_config import AM_START,AM_END,PM_START,PM_END
from signal_gen.factor_engine import FactorEngine
from wangcai_cpp import (
    BacktestEngine, Strategy,
    Direction, OrderType,
    Event, UserEvent, Execution, Snapshot,
    OrderDetail, TradeDetail,  # 新增：原始市场数据结构
    make_order_event, make_cancel_event,
    TradeCallback, OrderCallback, MultiBacktestEngine
)
class FactorStrategy(Strategy):
    def __init__(self,strategy_id,inst,trade_date,snapshot_q,trade_detail_q,order_detail_q,trade_order_detail_q,factor_q,unit_list,timeline):
        super().__init__()
        self.inst = inst
        self.strategy_id = strategy_id
        self.trade_date = trade_date
        self.snapshot_q = snapshot_q
        self.trade_detail_q = trade_detail_q
        self.order_detail_q = order_detail_q
        self.trade_order_detail_q = trade_order_detail_q
        self.factor_q = factor_q
        self.unit_list = unit_list
        self.force_quit = False
        self.timeline = timeline
        self.engine = FactorEngine(
            inst=inst,
            trade_date=trade_date,
            snapshot_q=snapshot_q,
            trade_detail_q=self.trade_detail_q,
            order_detail_q=self.order_detail_q,
            trade_order_detail_q=trade_order_detail_q,
            factor_q=self.factor_q,
            timeline = self.timeline
        )
    
    def getStrategyId(self) -> str:
        return self.strategy_id
    
    def init_work_td(self):
        for unit in self.unit_list:
            self.engine.register(unit)
        self.engine.init_threads()
        self.factor_calc = threading.Thread(target = self._get_factor_,name="FactorCalculationThread")
        self.factor_calc.start()
        logger.info(f"{self.inst} all the thread init successfully.")
    
    def onTradeEvent(self, execution, datetime: str) -> List:
        """处理逐笔成交
        'Exchange': trade.Exchange,
                'Instrument': trade.Instrument,
                'Time': trade.Time,
                'ChannelNo': trade.ChannelNo,
                'TradeIndex': trade.TradeIndex,
                'Price': trade.Price,
                'Volume': trade.Volume,
                'ExecType': trade.ExecType,
                'BuyNo': trade.BuyNo,
                'SellNo': trade.SellNo,
                'TradeBSFlag': trade.TradeBSFlag,
                'BizIndex': trade.BizIndex"""
        data = {}
        data['price'] = execution.price
        data['Instrument']=execution.instrument
        data['Time'] = execution.time
        data['buy_order_id'] = execution.buy_order_id
        data['sell_order_id'] = execution.sell_order_id
        data['volume'] = execution.volume
        self.trade_detail_q.put(data)

    def onOrderEvent(self, event) -> List:
        """处理逐笔委托"""
        return []

    def onTradeCallback(self, callback):       
        """交易回调"""
        # logger.info(callback)
        # self.trade_detail_q.put(callback)
        # print(f"[{self.strategy_id}] 交易回调: "
        #     f"订单={callback.localid}, "
        #     f"方向={callback.direction}, "
        #     f"数量={callback.volume}, "
        #     f"价格={callback.price/10000:.2f}, "
        #     f"持仓={callback.deltapos}")

    def onOrderCallback(self, callback):
        """订单回调"""
        # print(f"[{self.strategy_id}] 订单回调: "
        #     f"订单={callback.orderlocalid}, "
        #     f"方向={callback.direction}, "
        #     f"数量={callback.volume}, "
        #     f"价格={callback.price/10000:.2f}")

    def _get_factor_(self):
        while not self.force_quit:
            try:
                factor = self.factor_q.get()
                # logger.info(factor)
                file_path = f"factor/result/{self.inst['Instrument']}_{self.trade_date}_result.csv"
                if not os.path.exists(file_path):
                    factor.to_csv(file_path,mode='a',index=False)
                else:
                    factor.to_csv(file_path,mode='a',index=False,header=False)
            except queue.Empty:
                continue
            except Exception as e:
                logger.info(f"get factor data has something wrong.{e}")

    def set_force_quit_flag(self):
        self.force_quit = True
        if hasattr(self, 'timeline'):
            self.timeline.stop_event.value = True  # 通知线程停止
            
        # 等待因子计算线程结束（如果不是守护线程）
        if hasattr(self, 'factor_calc') and self.factor_calc.is_alive():
            self.factor_calc.join()
