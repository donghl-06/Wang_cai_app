"""
旺财回测平台 - 策略范例

用户需要:
    1. 继承 Strategy 类
    2. 重载 getStrategyId() 返回账户标识
    3. 重载事件处理函数: onTickEvent, onOrderEvent, onTradeEvent
    4. 重载回调函数: onTradeCallback, onOrderCallback
"""

import os
from datetime import datetime
from typing import List
import pandas as pd

# 从 wangcai_syn 包导入所需类型
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback
)


class ExampleStrategy(Strategy):
    """
    策略范例类
    
    展示了如何:
    - 处理三种市场事件: Tick、订单、成交
    - 发送下单和撤单指令
    - 处理下单和成交回调
    - 记录和保存交易数据
    """
    
    def __init__(self, account: str = "user1"):
        """
        初始化策略
        
        Args:
            account: 账户标识
        """
        super().__init__()
        self.account = account
        
        # 订单ID生成器
        self.order_id_counter = 1000
        
        # 持仓信息
        self.current_position = 0
        
        # 回调记录
        self.order_callbacks = []      # 下单回调记录
        self.cancel_callbacks = []     # 撤单回调记录
        self.trade_callbacks = []      # 成交回调记录
        self.position_updates = []     # 持仓更新记录
        
        # 行情数据记录
        self.tick_events = []          # Tick事件记录
        self.order_events = []         # 订单事件记录
        self.execution_events = []     # 成交事件记录
        
        print(f"[{self.account}] 策略已初始化")
    
    def getStrategyId(self) -> str:
        """
        返回账户标识（必须重载）
        
        Returns:
            str: 账户ID
        """
        return self.account
    
    def _get_next_order_id(self) -> str:
        """生成下一个订单ID"""
        order_id = f"{self.account}_{self.order_id_counter}"
        self.order_id_counter += 1
        return order_id
    
    # ========== 市场事件处理函数 ==========
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        """
        处理Tick快照推送事件
        
        Args:
            snapshot: Tick快照数据，包含以下主要字段:
                - Exchange: 交易所代码 (0=上海, 1=深圳)
                - Instrument: 合约代码
                - Time: 时间戳
                - last_price: 最新价
                - bids[]: 买盘价格数组
                - asks[]: 卖盘价格数组
                - bid_sizes[]: 买盘数量数组
                - ask_sizes[]: 卖盘数量数组
                - Volume: 成交量
                - Turnover: 成交额
        
        Returns:
            List[UserEvent]: 要执行的用户事件列表（下单/撤单）
        """
        # 记录Tick数据
        try:
            tick_record = {
                'Exchange': snapshot.Exchange,
                'Instrument': snapshot.Instrument,
                'Time': snapshot.Time,
                'Last': snapshot.last_price,
                'BidPrice': list(snapshot.bids),
                'AskPrice': list(snapshot.asks),
                'BidVolume': list(snapshot.bid_sizes),
                'AskVolume': list(snapshot.ask_sizes),
                'Volume': snapshot.Volume,
            }
            self.tick_events.append(tick_record)
        except Exception as e:
            print(f"⚠️ [Tick记录] 异常: {e}")
        
        # ===== 在这里实现你的交易逻辑 =====
        events = []
        
        # 示例: 简单的下单逻辑（取消注释启用）
        # if snapshot.last_price > 0 and len(self.order_callbacks) < 5:
        #     order_id = self._get_next_order_id()
        #     event = make_order(
        #         Broker='',
        #         Account=self.account,
        #         Exchange=snapshot.Exchange,
        #         Instrument=snapshot.Instrument,
        #         OrderLocalID=order_id,
        #         OrderType=0,              # 限价单
        #         Direction=1,              # 买入
        #         Price=snapshot.bids[0],   # 买一价
        #         Volume=100
        #     )
        #     events.append(event)
        
        return events
    
    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        """
        处理订单推送事件（逐笔委托）
        
        Args:
            order: 原始订单数据，包含以下字段:
                - Exchange: 交易所代码
                - Instrument: 合约代码
                - Time: 时间戳
                - OrderNo: 订单号
                - Price: 价格（厘）
                - Volume: 数量
                - Side: 买卖方向
        
        Returns:
            List[UserEvent]: 要执行的用户事件列表
        """
        # 记录订单事件
        try:
            order_record = {
                'Exchange': order.Exchange,
                'Instrument': order.Instrument,
                'Time': order.Time,
                'OrderNo': order.OrderNo,
                'Price': order.Price,
                'Volume': order.Volume,
                'Side': order.Side,
            }
            self.order_events.append(order_record)
        except Exception as e:
            print(f"⚠️ [订单事件记录] 异常: {e}")
        
        return []
    
    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        """
        处理成交推送事件（逐笔成交）
        
        Args:
            trade: 原始成交数据，包含以下字段:
                - Exchange: 交易所代码
                - Instrument: 合约代码
                - Time: 时间戳
                - Price: 成交价格（厘）
                - Volume: 成交数量
                - BuyNo: 买方订单号
                - SellNo: 卖方订单号
        
        Returns:
            List[UserEvent]: 要执行的用户事件列表
        """
        # 记录成交事件
        try:
            trade_record = {
                'Exchange': trade.Exchange,
                'Instrument': trade.Instrument,
                'Time': trade.Time,
                'Price': trade.Price,
                'Volume': trade.Volume,
                'BuyNo': trade.BuyNo,
                'SellNo': trade.SellNo,
            }
            self.execution_events.append(trade_record)
        except Exception as e:
            print(f"⚠️ [成交事件记录] 异常: {e}")
        
        return []
    
    # ========== 回调函数 ==========
    
    def onOrderCallback(self, callback: OrderCallback) -> None:
        """
        下单确认回调
        
        Args:
            callback: 下单回调数据，包含以下字段:
                - orderlocalid: 订单本地ID
                - time: 下单时间
                - exchange: 交易所 (0=上海, 1=深圳)
                - direction: 方向 (1=买, 2=卖)
                - price: 下单价格（厘）
                - volume: 下单数量
                - ask1: 下单时卖一价
                - bid1: 下单时买一价
                - deltapos: 当前持仓
        """
        try:
            record = {
                'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                'OrderLocalID': callback.orderlocalid,
                'time': callback.time,
                'Exchange': callback.exchange,
                'Direction': callback.direction,
                'Price': callback.price / 10000.0,
                'Volume': callback.volume,
                'Ask1': callback.ask1 / 10000.0,
                'Bid1': callback.bid1 / 10000.0,
            }
            self.order_callbacks.append(record)
            
            direction_str = "买入" if callback.direction == 1 else "卖出"
            print(f"📝 [下单回调] {record['OrderLocalID']} {direction_str}: "
                  f"{record['Volume']}@{record['Price']:.4f}")
                  
        except Exception as e:
            print(f"❌ [下单回调] 处理异常: {e}")
    
    def onTradeCallback(self, callback: TradeCallback) -> None:
        """
        成交/撤单回调
        
        Args:
            callback: 交易回调数据，包含以下字段:
                - localid: 订单本地ID
                - direction: 方向（'B'=买, 'S'=卖）
                - volume: 成交/撤单数量
                - price: 成交价格（厘）
                - matchamount: 成交金额
                - deltapos: 当前持仓
                - matchtime: 成交时间
                - matchtype: 类型（'T'=成交, 'D'=撤单）
        """
        try:
            order_id = callback.localid
            matchtype = callback.matchtype
            
            if matchtype == 'T':
                # 成交回调
                record = {
                    'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                    'OrderLocalID': order_id,
                    'Direction': callback.direction,
                    'Volume': callback.volume,
                    'Price': callback.price / 10000.0,
                    'MatchTime': callback.matchtime,
                }
                self.trade_callbacks.append(record)
                
                # 更新持仓
                old_position = self.current_position
                self.current_position = callback.deltapos
                
                # 记录持仓变化
                position_record = {
                    'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                    'OrderLocalID': order_id,
                    'action': 'trade',
                    'old_position': old_position,
                    'new_position': self.current_position,
                }
                self.position_updates.append(position_record)
                
                print(f"✅ [成交] {order_id}: {callback.volume}@{callback.price/10000:.4f}, "
                      f"持仓: {old_position} → {self.current_position}")
                      
            elif matchtype == 'D':
                # 撤单回调
                record = {
                    'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                    'OrderLocalID': order_id,
                    'Volume': callback.volume,
                    'MatchTime': callback.matchtime,
                }
                self.cancel_callbacks.append(record)
                print(f"❌ [撤单] {order_id}")
                
        except Exception as e:
            print(f"❌ [交易回调] 处理异常: {e}")
    
    # ========== 辅助函数 ==========
    
    def save_records(self, output_dir: str):
        """
        保存所有记录到CSV文件
        
        Args:
            output_dir: 输出目录路径
        """
        os.makedirs(output_dir, exist_ok=True)
        
        # 回调记录
        if self.order_callbacks:
            path = os.path.join(output_dir, "order_callbacks.csv")
            pd.DataFrame(self.order_callbacks).to_csv(path, index=False)
            print(f"✅ 下单回调: {path} ({len(self.order_callbacks)} 条)")
        
        if self.cancel_callbacks:
            path = os.path.join(output_dir, "cancel_callbacks.csv")
            pd.DataFrame(self.cancel_callbacks).to_csv(path, index=False)
            print(f"✅ 撤单回调: {path} ({len(self.cancel_callbacks)} 条)")
        
        if self.trade_callbacks:
            path = os.path.join(output_dir, "trade_callbacks.csv")
            pd.DataFrame(self.trade_callbacks).to_csv(path, index=False)
            print(f"✅ 成交回调: {path} ({len(self.trade_callbacks)} 条)")
        
        if self.position_updates:
            path = os.path.join(output_dir, "position_updates.csv")
            pd.DataFrame(self.position_updates).to_csv(path, index=False)
            print(f"✅ 持仓更新: {path} ({len(self.position_updates)} 条)")
        
        # 行情数据
        if self.tick_events:
            path = os.path.join(output_dir, "tick_events.csv")
            pd.DataFrame(self.tick_events).to_csv(path, index=False)
            print(f"✅ Tick事件: {path} ({len(self.tick_events)} 条)")
        
        if self.order_events:
            path = os.path.join(output_dir, "order_events.csv")
            pd.DataFrame(self.order_events).to_csv(path, index=False)
            print(f"✅ 订单事件: {path} ({len(self.order_events)} 条)")
        
        if self.execution_events:
            path = os.path.join(output_dir, "trade_events.csv")
            pd.DataFrame(self.execution_events).to_csv(path, index=False)
            print(f"✅ 成交事件: {path} ({len(self.execution_events)} 条)")
    
    def print_summary(self):
        """打印策略运行摘要"""
        print(f"\n{'='*50}")
        print(f"📊 {self.account} 运行摘要")
        print(f"{'='*50}")
        print(f"  Tick事件: {len(self.tick_events)} 条")
        print(f"  委托事件: {len(self.order_events)} 条")
        print(f"  成交事件: {len(self.execution_events)} 条")
        print(f"  下单回调: {len(self.order_callbacks)} 次")
        print(f"  成交回调: {len(self.trade_callbacks)} 次")
        print(f"  撤单回调: {len(self.cancel_callbacks)} 次")
        print(f"  当前持仓: {self.current_position}")
        print(f"{'='*50}")
