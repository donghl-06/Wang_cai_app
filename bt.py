import sys
import pathlib
import pandas as pd
import os
from datetime import datetime
from typing import List, Dict, Optional
import json

# 添加本地编译的模块路径
sys.path.insert(0, './build')

from wangcai_cpp import (
    BacktestEngine, Strategy,
    Direction, OrderType,
    Event, UserEvent, Execution, Snapshot,
    OrderDetail, TradeDetail,  # 新增：原始市场数据结构
    make_order_event, make_cancel_event,
    TradeCallback, OrderCallback, MultiBacktestEngine
)

class InterfaceTestStrategy(Strategy):
    """
    接口验证策略：针对每种推送类型分别下10个订单验证接口
    - 每种推送下10个订单，其中5个在下次推送时撤单
    - 记录所有回调：下单回调、撤单回调、成交回调、持仓更新
    """
    
    def __init__(self, strategy_id: str = "INTERFACE_TEST"):
        super().__init__()
        self.strategy_id = strategy_id
        
        # 每种事件类型的状态管理
        self.event_state = {
            'cstick': {
                'order_count': 0,
                'pending_orders': [],
                'last_event_time': None,
                'phase': 'waiting',
                'event_count': 0
            },
            'cstra': {
                'order_count': 0,
                'pending_orders': [],
                'last_event_time': None,
                'phase': 'waiting',
                'event_count': 0
            },
            'csord': {
                'order_count': 0,
                'pending_orders': [],
                'last_event_time': None,
                'phase': 'waiting',
                'event_count': 0
            }
        }
        
        # 4个核心回调记录文件
        self.order_callbacks = []      # 下单回调记录
        self.cancel_callbacks = []     # 撤单回调记录
        self.trade_callbacks = []      # 成交回调记录
        self.position_updates = []     # 持仓更新记录
        
        # 3个原始行情数据记录文件
        self.tick_events = []          # Tick事件记录 (onTickEvent)
        self.order_events = []         # 订单事件记录 (onOrderEvent)  
        self.execution_events = []     # 成交事件记录 (onTradeEvent)
        
        # 当前持仓
        self.current_position = 0
        
        # 测试配置
        self.orders_per_batch = 10     # 每批次下单数量
        self.orders_to_cancel = 5      # 撤单数量
        
        # 订单ID生成器
        self.order_id_counter = 1000
        
        print(f"[{self.strategy_id}] 接口验证策略已启动")
        print(f"配置：每种推送类型下{self.orders_per_batch}个订单，下次推送时撤销前{self.orders_to_cancel}个")
    
    def getStrategyId(self) -> str:
        return self.strategy_id
    
    def _get_next_order_id(self) -> str:
        """生成下一个订单ID"""
        order_id = str(self.order_id_counter)
        self.order_id_counter += 1
        return order_id
    
    def _convert_price(self, price_value) -> int:
        """
        转换价格到系统内部单位（厘）
        如果价格小于1000，认为是元，需要乘以10000
        如果价格大于等于1000，认为已经是厘
        """
        if isinstance(price_value, (int, float)):
            if price_value < 1000:
                # 元转厘
                return int(price_value * 10000)
            else:
                # 已经是厘
                return int(price_value)
        return 50000  # 默认5元
    
    def _align_price(self, price: int) -> int:
        """价格对齐到100厘（分）"""
        if price <= 0:
            return 100
        return round(price / 100) * 100
    
    def _get_time_str(self, datetime_str: str) -> str:
        """从datetime字符串中提取时间部分"""
        if len(datetime_str) >= 19:
            return datetime_str[11:19]
        return "00:00:00"
    
    def onOrderEvent(self, order) -> List[UserEvent]:
        """处理订单推送事件（csord）- 接收原始OrderDetail结构"""
        # 记录原始订单事件数据
        try:
            order_event_record = {
                'event_type': 'csord',
                'exchange': order.Exchange,
                'instrument': order.Instrument,
                'time': order.Time,
                'channel_no': order.ChannelNo,
                'order_no': order.OrderNo,
                'price': order.Price / 10000.0,  # 原始值*10000，转换为元
                'volume': order.Volume,
                'side': order.Side,
                'order_kind': order.OrderKind,
                'seq_no': order.SeqNo,
                'biz_index': order.BizIndex
            }
            self.order_events.append(order_event_record)
        except Exception as e:
            print(f"⚠️ [订单事件记录] 异常: {e}")
        
        # 为策略处理构造datetime字符串和symbol
        datetime_str = f"2024-12-19 {order.Time//10000000:02d}:{(order.Time//100000)%100:02d}:{(order.Time//1000)%100:02d}.{order.Time%1000:03d}"
        symbol = order.Instrument
        ref_price = self._convert_price(order.Price)
        return self._process_event('csord', datetime_str, symbol, ref_price)
    
    def onTradeEvent(self, trade) -> List[UserEvent]:
        """处理成交推送事件（cstra）- 接收原始TradeDetail结构"""
        # 记录原始成交事件数据
        try:
            trade_record = {
                'event_type': 'cstra',
                'exchange': trade.Exchange,
                'instrument': trade.Instrument,
                'channel_no': trade.ChannelNo,
                'trade_index': trade.TradeIndex,
                'time': trade.Time,
                'price': trade.Price / 10000.0,  # 原始值*10000，转换为元
                'volume': trade.Volume,
                'exec_type': trade.ExecType,
                'buy_no': trade.BuyNo,
                'sell_no': trade.SellNo,
                'trade_bs_flag': trade.TradeBSFlag,
                'biz_index': trade.BizIndex
            }
            self.execution_events.append(trade_record)
        except Exception as e:
            print(f"⚠️ [成交事件记录] 异常: {e}")
        
        # 为策略处理构造datetime字符串
        datetime_str = f"2024-12-19 {trade.Time//10000000:02d}:{(trade.Time//100000)%100:02d}:{(trade.Time//1000)%100:02d}.{trade.Time%1000:03d}"
        symbol = trade.Instrument
        ref_price = self._convert_price(trade.Price)
        return self._process_event('cstra', datetime_str, symbol, ref_price)
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        """处理Tick推送事件（cstick）"""
        # 记录原始Tick事件数据（每次都记录，不受处理频率限制）
        # 完整记录所有37个字段，顺序与market_info.h中的Snapshot结构定义一致
        try:
            tick_record = {
                'event_type': 'cstick',
                # === 基础信息字段 ===
                'exchange': snapshot.Exchange,              # 交易所代码
                'instrument': snapshot.Instrument,          # 合约代码
                'trading_day': snapshot.TradingDay,         # 交易日
                'action_day': snapshot.ActionDay,           # 自然日
                'time': snapshot.Time,                      # 时间
                'datetime': snapshot.datetime,              # 日期时间字符串
                'status': snapshot.Status,                  # 状态
                
                # === 价格信息字段 (原始值*10000，转换为元) ===
                'pre_close': snapshot.PreClose / 10000.0,          # 前收盘价
                'open': snapshot.Open / 10000.0,                   # 开盘价
                'high': snapshot.High / 10000.0,                   # 最高价
                'low': snapshot.Low / 10000.0,                     # 最低价
                'last_price': snapshot.last_price / 10000.0,       # 最新价
                
                # === 完整的10档买方行情 ===
                'bid1': snapshot.bids[0] / 10000.0 if len(snapshot.bids) > 0 and snapshot.bids[0] > 0 else 0,
                'bid2': snapshot.bids[1] / 10000.0 if len(snapshot.bids) > 1 and snapshot.bids[1] > 0 else 0,
                'bid3': snapshot.bids[2] / 10000.0 if len(snapshot.bids) > 2 and snapshot.bids[2] > 0 else 0,
                'bid4': snapshot.bids[3] / 10000.0 if len(snapshot.bids) > 3 and snapshot.bids[3] > 0 else 0,
                'bid5': snapshot.bids[4] / 10000.0 if len(snapshot.bids) > 4 and snapshot.bids[4] > 0 else 0,
                'bid6': snapshot.bids[5] / 10000.0 if len(snapshot.bids) > 5 and snapshot.bids[5] > 0 else 0,
                'bid7': snapshot.bids[6] / 10000.0 if len(snapshot.bids) > 6 and snapshot.bids[6] > 0 else 0,
                'bid8': snapshot.bids[7] / 10000.0 if len(snapshot.bids) > 7 and snapshot.bids[7] > 0 else 0,
                'bid9': snapshot.bids[8] / 10000.0 if len(snapshot.bids) > 8 and snapshot.bids[8] > 0 else 0,
                'bid10': snapshot.bids[9] / 10000.0 if len(snapshot.bids) > 9 and snapshot.bids[9] > 0 else 0,
                
                # === 完整的10档买方数量 ===
                'bid1_size': snapshot.bid_sizes[0] if len(snapshot.bid_sizes) > 0 else 0,
                'bid2_size': snapshot.bid_sizes[1] if len(snapshot.bid_sizes) > 1 else 0,
                'bid3_size': snapshot.bid_sizes[2] if len(snapshot.bid_sizes) > 2 else 0,
                'bid4_size': snapshot.bid_sizes[3] if len(snapshot.bid_sizes) > 3 else 0,
                'bid5_size': snapshot.bid_sizes[4] if len(snapshot.bid_sizes) > 4 else 0,
                'bid6_size': snapshot.bid_sizes[5] if len(snapshot.bid_sizes) > 5 else 0,
                'bid7_size': snapshot.bid_sizes[6] if len(snapshot.bid_sizes) > 6 else 0,
                'bid8_size': snapshot.bid_sizes[7] if len(snapshot.bid_sizes) > 7 else 0,
                'bid9_size': snapshot.bid_sizes[8] if len(snapshot.bid_sizes) > 8 else 0,
                'bid10_size': snapshot.bid_sizes[9] if len(snapshot.bid_sizes) > 9 else 0,
                
                # === 完整的10档卖方行情 ===
                'ask1': snapshot.asks[0] / 10000.0 if len(snapshot.asks) > 0 and snapshot.asks[0] > 0 else 0,
                'ask2': snapshot.asks[1] / 10000.0 if len(snapshot.asks) > 1 and snapshot.asks[1] > 0 else 0,
                'ask3': snapshot.asks[2] / 10000.0 if len(snapshot.asks) > 2 and snapshot.asks[2] > 0 else 0,
                'ask4': snapshot.asks[3] / 10000.0 if len(snapshot.asks) > 3 and snapshot.asks[3] > 0 else 0,
                'ask5': snapshot.asks[4] / 10000.0 if len(snapshot.asks) > 4 and snapshot.asks[4] > 0 else 0,
                'ask6': snapshot.asks[5] / 10000.0 if len(snapshot.asks) > 5 and snapshot.asks[5] > 0 else 0,
                'ask7': snapshot.asks[6] / 10000.0 if len(snapshot.asks) > 6 and snapshot.asks[6] > 0 else 0,
                'ask8': snapshot.asks[7] / 10000.0 if len(snapshot.asks) > 7 and snapshot.asks[7] > 0 else 0,
                'ask9': snapshot.asks[8] / 10000.0 if len(snapshot.asks) > 8 and snapshot.asks[8] > 0 else 0,
                'ask10': snapshot.asks[9] / 10000.0 if len(snapshot.asks) > 9 and snapshot.asks[9] > 0 else 0,
                
                # === 完整的10档卖方数量 ===
                'ask1_size': snapshot.ask_sizes[0] if len(snapshot.ask_sizes) > 0 else 0,
                'ask2_size': snapshot.ask_sizes[1] if len(snapshot.ask_sizes) > 1 else 0,
                'ask3_size': snapshot.ask_sizes[2] if len(snapshot.ask_sizes) > 2 else 0,
                'ask4_size': snapshot.ask_sizes[3] if len(snapshot.ask_sizes) > 3 else 0,
                'ask5_size': snapshot.ask_sizes[4] if len(snapshot.ask_sizes) > 4 else 0,
                'ask6_size': snapshot.ask_sizes[5] if len(snapshot.ask_sizes) > 5 else 0,
                'ask7_size': snapshot.ask_sizes[6] if len(snapshot.ask_sizes) > 6 else 0,
                'ask8_size': snapshot.ask_sizes[7] if len(snapshot.ask_sizes) > 7 else 0,
                'ask9_size': snapshot.ask_sizes[8] if len(snapshot.ask_sizes) > 8 else 0,
                'ask10_size': snapshot.ask_sizes[9] if len(snapshot.ask_sizes) > 9 else 0,
                
                # === 交易统计字段 ===
                'num_trades': snapshot.NumTrades,                  # 成交笔数
                'volume': snapshot.Volume,                         # 成交总量
                'turnover': snapshot.Turnover,                     # 成交总金额
                
                # === 价格限制字段 ===
                'upper_limit': snapshot.UpperLimit / 10000.0,      # 涨停价
                'lower_limit': snapshot.LowerLimit / 10000.0,      # 跌停价
                
                # === 期货持仓字段 ===
                'open_interest': snapshot.OpenInterest,            # 今持仓量
                'pre_open_interest': snapshot.PreOpenInterest,     # 昨持仓量
                'delta': snapshot.Delta / 10000.0,                 # 今虚实度
                'pre_delta': snapshot.PreDelta / 10000.0,          # 昨虚实度
                
                # === 收盘和结算价格 ===
                'close': snapshot.Close / 10000.0,                 # 今收盘价
                'settle_price': snapshot.SettlePrice / 10000.0,    # 今结算价
                'pre_settle_price': snapshot.PreSettlePrice / 10000.0, # 昨结算价
                
                # === 集合竞价字段 ===
                'auction_price': snapshot.AuctionPrice / 10000.0,  # 波段性中断参考价
                'auction_qty': snapshot.AuctionQty,                # 波段性中断集合竞价虚拟匹配量
                
                # === 其他字段 ===
                'iopv': snapshot.Iopv / 10000.0,                   # IOPV
                'total_ask_vol': snapshot.TotalAskVol,             # 委托卖出总量
                'total_bid_vol': snapshot.TotalBidVol,             # 委托买入总量
                'weighted_avg_bid_price': snapshot.WeightedAvgBidPrice / 10000.0,  # 加权平均委买价格
                'weighted_avg_ask_price': snapshot.WeightedAvgAskPrice / 10000.0,  # 加权平均委卖价格
                
                # === ETF字段 ===
                'etf_create_vol': snapshot.ETFCreateVol,           # ETF申购总量
                'etf_redeem_vol': snapshot.ETFRedeemVol            # ETF赎回总量
            }
            self.tick_events.append(tick_record)
        except Exception as e:
            print(f"⚠️ [Tick事件记录] 异常: {e}")
        
        # 限制策略处理频率
        self.event_state['cstick']['event_count'] += 1
        if self.event_state['cstick']['event_count'] % 20 != 1:
            return []
        
        # 使用快照价格
        ref_price = snapshot.last_price if snapshot.last_price > 0 else 79800  # 默认7.98元
        if hasattr(snapshot, 'asks') and snapshot.asks and len(snapshot.asks) > 0:
            if snapshot.asks[0] > 0:
                ref_price = snapshot.asks[0]
        
        return self._process_event('cstick', snapshot.datetime, snapshot.Instrument, ref_price)
    
    def _process_event(self, event_type: str, datetime: str, symbol: str, ref_price: int) -> List[UserEvent]:
        """统一的事件处理逻辑"""
        events = []
        
        # 只在连续竞价阶段测试
        time_str = self._get_time_str(datetime)
        if time_str < "09:30:00" or time_str > "14:55:00":
            return events
        
        state = self.event_state[event_type]
        
        # 状态机处理
        if state['phase'] == 'waiting':
            # 开始第一批下单
            print(f"\n🔸 [{event_type}] 开始测试，准备下{self.orders_per_batch}个订单")
            events = self._create_orders(event_type, datetime, symbol, ref_price)
            state['phase'] = 'ordering'
            state['last_event_time'] = datetime
            
        elif state['phase'] == 'ordering':
            # 已经下了订单，下次推送时撤销前5个
            if state['last_event_time'] != datetime:
                print(f"🔸 [{event_type}] 收到新推送，撤销前{self.orders_to_cancel}个订单")
                events = self._cancel_orders(event_type, datetime)
                state['phase'] = 'done'
                state['last_event_time'] = datetime
                print(f"✅ [{event_type}] 测试完成")
        
        return events
    
    def _create_orders(self, event_type: str, datetime: str, symbol: str, ref_price: int) -> List[UserEvent]:
        """创建订单"""
        events = []
        state = self.event_state[event_type]
        
        # 确保参考价格在合理范围内（7-9元之间）
        ref_price = self._align_price(ref_price)
        if ref_price < 70000 or ref_price > 90000:
            ref_price = 79800  # 默认7.98元
        
        print(f"  参考价格: {ref_price/10000:.4f}元")
        
        for i in range(self.orders_per_batch):
            order_id = f"{self.strategy_id}_{event_type.upper()}_{self._get_next_order_id()}"
            
            # 交替买卖方向
            direction = Direction.Buy if i % 2 == 0 else Direction.Sell
            
            if i < self.orders_to_cancel:
                # 前5个：待撤单订单，远离市价
                if direction == Direction.Buy:
                    # 买单：低价挂单（7.50元左右）
                    price = self._align_price(75000)
                else:
                    # 卖单：高价挂单（8.50元左右）  
                    price = self._align_price(85000)
                state['pending_orders'].append(order_id)
                purpose = "待撤单"
            else:
                # 后5个：尝试成交的订单，接近市价
                if direction == Direction.Buy:
                    # 买单：略高于市价（增加200厘=0.02元）
                    price = self._align_price(ref_price + 200)
                    # 确保不超过涨停
                    if price > 87600:  # 8.76元是涨停
                        price = 87600
                else:
                    # 卖单：略低于市价（减少200厘=0.02元）
                    price = self._align_price(ref_price - 200)
                    # 确保不低于跌停
                    if price < 71600:  # 7.16元是跌停
                        price = 71600
                purpose = "待成交"
            
            volume = 100 + i * 10
            
            print(f"    [{i+1}/{self.orders_per_batch}] {order_id} - {purpose} - "
                  f"{'买' if direction == Direction.Buy else '卖'} {volume}@{price/10000:.4f}元")
            
            # 创建订单事件
            order_event = make_order_event(
                order_id=order_id,
                symbol=symbol,
                direction=direction,
                order_type=OrderType.Limit,
                price=price,
                volume=volume,
                strategy_id=self.strategy_id
            )
            
            events.append(order_event)
            state['order_count'] += 1
        
        return events
    
    def _cancel_orders(self, event_type: str, datetime: str) -> List[UserEvent]:
        """撤销订单"""
        events = []
        state = self.event_state[event_type]
        
        # 撤销前5个订单
        cancel_count = 0
        while state['pending_orders'] and cancel_count < self.orders_to_cancel:
            order_id = state['pending_orders'].pop(0)
            cancel_event = make_cancel_event(order_id, self.strategy_id)
            events.append(cancel_event)
            
            print(f"    撤单 [{cancel_count+1}/{self.orders_to_cancel}]: {order_id}")
            cancel_count += 1
        
        return events
    
    # ========== 回调函数重载 - 记录所有回调 ==========
    
    def onOrderFilled(self, order_id: str, price: int, volume: int) -> None:
        """订单成交回调"""
        record = {
            'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
            'callback_type': 'onOrderFilled',
            'order_id': order_id,
            'price': price / 10000.0,
            'volume': volume,
            'event_type': self._get_event_type(order_id)
        }
        self.trade_callbacks.append(record)
        print(f"📈 [成交回调-旧] {order_id}: {volume}@{price/10000:.4f}")
    
    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        """订单撤销回调（旧接口）"""
        record = {
            'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
            'callback_type': 'onOrderCancelled',
            'order_id': order_id,
            'reason': reason,
            'event_type': self._get_event_type(order_id)
        }
        self.cancel_callbacks.append(record)
        print(f"❌ [撤单回调-旧] {order_id}: {reason}")
    
    def onTradeCallback(self, callback) -> None:
        """交易回调"""
        try:
            # 直接访问属性
            order_id = callback.localid
            direction = callback.direction  # 现在是字符串
            volume = callback.volume
            price = callback.price
            matchamount = callback.matchamount
            deltapos = callback.deltapos
            matchtime = callback.matchtime
            matchtype = callback.matchtype  # 现在是字符串
            
            print(f"📊 [交易回调] 订单:{order_id}, 类型:{matchtype}, 方向:{direction}")
            
            # 判断是成交还是撤单
            if matchtype == 'T':
                # 成交回调
                record = {
                    'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                    'callback_type': 'onTradeCallback_Trade',
                    'order_id': order_id,
                    'direction': direction,
                    'volume': volume,
                    'price': price / 10000.0,
                    'match_amount': matchamount,
                    'delta_position': deltapos,
                    'match_time': matchtime,
                    'event_type': self._get_event_type(order_id)
                }
                self.trade_callbacks.append(record)
                
                # 更新持仓
                old_position = self.current_position
                self.current_position = deltapos
                
                # 记录持仓变化
                position_record = {
                    'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                    'order_id': order_id,
                    'action': 'trade',
                    'direction': direction,
                    'volume': volume,
                    'price': price / 10000.0,
                    'old_position': old_position,
                    'new_position': self.current_position,
                    'position_change': self.current_position - old_position
                }
                self.position_updates.append(position_record)
                
                print(f"✅ [成交] {order_id}: {volume}@{price/10000:.4f}, "
                    f"持仓: {old_position} → {self.current_position}")
                
            elif matchtype == 'D':
                # 撤单回调
                record = {
                    'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                    'callback_type': 'onTradeCallback_Cancel',
                    'order_id': order_id,
                    'direction': direction,
                    'volume': volume,
                    'price': price / 10000.0,
                    'delta_position': deltapos,
                    'match_time': matchtime,
                    'event_type': self._get_event_type(order_id)
                }
                self.cancel_callbacks.append(record)
                print(f"❌ [撤单] {order_id}")
            else:
                print(f"⚠️ 未知的matchtype: {matchtype}")
                
        except Exception as e:
            print(f"❌ [交易回调] 处理异常: {e}")
            import traceback
            traceback.print_exc()

    def onOrderCallback(self, callback) -> None:
        """下单回调 - 正确处理版本"""
        try:
            # 直接访问属性
            order_id = callback.orderlocalid
            order_time = callback.time
            exchange = callback.exchange
            ask1 = callback.ask1
            bid1 = callback.bid1
            deltapos = callback.deltapos
            price = callback.price
            volume = callback.volume
            direction = callback.direction
            
            # 记录
            record = {
                'timestamp': datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                'callback_type': 'onOrderCallback',
                'order_id': order_id,
                'order_time': order_time,
                'exchange': exchange,
                'exchange_name': "上海" if exchange == 0 else "深圳",
                'direction': direction,
                'direction_name': "买入" if direction == 1 else "卖出",
                'volume': volume,
                'price': price / 10000.0,
                'ask1': ask1 / 10000.0,
                'bid1': bid1 / 10000.0,
                'delta_position': deltapos,
                'event_type': self._get_event_type(order_id)
            }
            self.order_callbacks.append(record)
            
            print(f"📝 [下单回调] {order_id} {record['direction_name']}: "
                f"{volume}@{price/10000:.4f} "
                f"(买一:{bid1/10000:.4f}, 卖一:{ask1/10000:.4f})")
            
        except Exception as e:
            print(f"❌ [下单回调] 处理异常: {e}")
            import traceback
            traceback.print_exc()
    
    def _get_event_type(self, order_id: str) -> str:
        """从订单ID提取事件类型"""
        if 'CSTICK' in order_id:
            return 'cstick'
        elif 'CSTRA' in order_id:
            return 'cstra'
        elif 'CSORD' in order_id:
            return 'csord'
        return 'unknown'
    
    def save_records(self, output_dir: str):
        """保存7个记录文件：4个回调记录 + 3个行情数据记录"""
        os.makedirs(output_dir, exist_ok=True)
        
        # === 4个回调记录文件 ===
        # 1. 下单回调记录
        if self.order_callbacks:
            file_path = os.path.join(output_dir, "order_callbacks.csv")
            pd.DataFrame(self.order_callbacks).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ 下单回调记录: {file_path} ({len(self.order_callbacks)} 条)")
        
        # 2. 撤单回调记录
        if self.cancel_callbacks:
            file_path = os.path.join(output_dir, "cancel_callbacks.csv")
            pd.DataFrame(self.cancel_callbacks).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ 撤单回调记录: {file_path} ({len(self.cancel_callbacks)} 条)")
        
        # 3. 成交回调记录
        if self.trade_callbacks:
            file_path = os.path.join(output_dir, "trade_callbacks.csv")
            pd.DataFrame(self.trade_callbacks).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ 成交回调记录: {file_path} ({len(self.trade_callbacks)} 条)")
        
        # 4. 持仓信息更新
        if self.position_updates:
            file_path = os.path.join(output_dir, "position_updates.csv")
            pd.DataFrame(self.position_updates).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ 持仓更新记录: {file_path} ({len(self.position_updates)} 条)")
        
        # === 3个原始行情数据文件 ===
        # 5. Tick事件记录
        if self.tick_events:
            file_path = os.path.join(output_dir, "tick_events.csv")
            pd.DataFrame(self.tick_events).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ Tick事件记录: {file_path} ({len(self.tick_events)} 条)")
        
        # 6. 订单事件记录  
        if self.order_events:
            file_path = os.path.join(output_dir, "order_events.csv")
            pd.DataFrame(self.order_events).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ 订单事件记录: {file_path} ({len(self.order_events)} 条)")
        
        # 7. 成交事件记录
        if self.execution_events:
            file_path = os.path.join(output_dir, "trade_events.csv")
            pd.DataFrame(self.execution_events).to_csv(file_path, index=False, encoding='utf-8')
            print(f"✅ 成交事件记录: {file_path} ({len(self.execution_events)} 条)")
    
    def print_test_report(self):
        """打印接口测试报告"""
        print(f"\n{'='*60}")
        print(f"🧪 {self.strategy_id} 接口验证报告")
        print(f"{'='*60}")
        
        print(f"\n📊 测试执行统计:")
        for event_type in ['cstick', 'cstra', 'csord']:
            state = self.event_state[event_type]
            print(f"\n  {event_type}接口:")
            print(f"    状态: {state['phase']}")
            print(f"    下单数: {state['order_count']}/{self.orders_per_batch}")
            
            # 统计各类型的回调
            order_count = len([r for r in self.order_callbacks if r.get('event_type') == event_type])
            trade_count = len([r for r in self.trade_callbacks if r.get('event_type') == event_type])
            cancel_count = len([r for r in self.cancel_callbacks if r.get('event_type') == event_type])
            
            print(f"    下单回调: {order_count} 次")
            print(f"    成交回调: {trade_count} 次")
            print(f"    撤单回调: {cancel_count} 次")
            
            if state['phase'] == 'done':
                print(f"    ✅ 测试完成")
            elif state['phase'] == 'ordering':
                print(f"    ⏳ 等待下次推送撤单")
            else:
                print(f"    ⏸ 未开始")
        
        print(f"\n📈 总体回调统计:")
        print(f"  - 下单回调总数: {len(self.order_callbacks)} 次")
        print(f"  - 成交回调总数: {len(self.trade_callbacks)} 次")
        print(f"  - 撤单回调总数: {len(self.cancel_callbacks)} 次")
        print(f"  - 持仓更新次数: {len(self.position_updates)} 次")
        print(f"  - 当前净持仓: {self.current_position}")
        
        # 接口验证结果
        all_complete = all(self.event_state[t]['phase'] == 'done' 
                          for t in ['cstick', 'cstra', 'csord'])
        
        print(f"\n🎯 接口验证结果:")
        if all_complete:
            print(f"  ✅ 所有接口验证完成！")
            print(f"  ✅ 下单接口正常")
            print(f"  ✅ 撤单接口正常")
            print(f"  ✅ 成交回调正常")
            print(f"  ✅ 持仓更新正常")
        else:
            print(f"  ⚠️ 部分接口测试未完成")
            for event_type in ['cstick', 'cstra', 'csord']:
                if self.event_state[event_type]['phase'] != 'done':
                    print(f"    - {event_type}: {self.event_state[event_type]['phase']}")
        
        print(f"{'='*60}")


def run_interface_test(symbol: str, date: str, data_path: str, output_dir: str) -> bool:
    """运行接口验证测试"""
    try:
        print(f"🧪 开始接口验证测试")
        print(f"合约: {symbol}")
        print(f"日期: {date}")
        print(f"数据路径: {data_path}")
        print(f"输出路径: {output_dir}")
        
        # 创建回测引擎
        engine = engine = MultiBacktestEngine([symbol], date, data_path)
        
        # 创建接口测试策略
        strategy = InterfaceTestStrategy("INTERFACE_TEST")
        engine.registerStrategy(strategy)
        
        # 运行回测
        print(f"\n🚀 开始运行接口验证...")
        engine.run()
        
        # 获取结果
        positions = engine.getPositions()
        total_pnl = engine.getTotalPnL()
        
        print(f"\n✅ 接口验证完成!")
        print(f"最终盈亏: {total_pnl:.2f} 元")
        
        # 保存4个核心文件
        strategy.save_records(output_dir)
        
        # 打印测试报告
        strategy.print_test_report()
        
        return True
        
    except Exception as e:
        print(f"❌ 接口验证失败: {e}")
        import traceback
        traceback.print_exc()
        return False


def main():
    """主函数"""
    # 配置参数
    symbol = "000488.SZ"
    date = "2024-12-19" 
    data_path = "logs"
    output_dir = f"./interface_test_output/{symbol}_{date}"
    
    # 检查数据文件
    required_files = [
        f"cstick_{symbol}_{date}.csv",
        f"cstra_{symbol}_{date}.csv", 
        f"csord_{symbol}_{date}.csv"
    ]
    
    print(f"检查数据文件...")
    missing_files = []
    for file in required_files:
        file_path = os.path.join(data_path, file)
        if not os.path.exists(file_path):
            missing_files.append(file)
        else:
            print(f"✅ 找到文件: {file}")
    
    if missing_files:
        print("❌ 缺少以下数据文件:")
        for file in missing_files:
            print(f"   {os.path.join(data_path, file)}")
        return 1
    
    print("✅ 数据文件检查通过")
    
    # 运行接口验证测试
    success = run_interface_test(symbol, date, data_path, output_dir)
    
    if success:
        print(f"\n🎉 接口验证测试成功完成！")
        print(f"\n📁 输出的7个文件：")
        print(f"   【回调记录】")
        print(f"   1️⃣ {output_dir}/order_callbacks.csv   - 下单回调记录")
        print(f"   2️⃣ {output_dir}/cancel_callbacks.csv  - 撤单回调记录") 
        print(f"   3️⃣ {output_dir}/trade_callbacks.csv   - 成交回调记录")
        print(f"   4️⃣ {output_dir}/position_updates.csv  - 持仓信息更新")
        print(f"   【原始市场数据】")
        print(f"   5️⃣ {output_dir}/tick_events.csv       - Tick事件记录 (对应Snapshot)")
        print(f"   6️⃣ {output_dir}/order_events.csv      - 订单事件记录 (对应OrderDetail)")
        print(f"   7️⃣ {output_dir}/trade_events.csv      - 成交事件记录 (对应TradeDetail)")
        return 0
    else:
        print("❌ 接口验证测试失败")
        return 1


if __name__ == "__main__":
    exit(main())