"""
被动单回测策略 - 使用 wangcai_syn 新接口

基于 passive_log.csv 挂单的被动单策略
"""

import pandas as pd
import os
import adata
from datetime import datetime, timedelta
from typing import List, Dict, Optional

# 从 wangcai_syn 包导入
from wangcai_syn import (
    run_backtest, Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel
)

class PassiveOrderStrategy(Strategy):
    """
    被动单策略：基于 passive_log.csv 挂单
    - 读取计划文件，按时间顺序挂出被动单
    - 每个订单最多等待1分钟，超时撤单
    - 记录挂单时间和成交时间
    """
    
    def __init__(self, account: str, symbol: str, date: str, plan_df: pd.DataFrame, strategy: str = 'passive_own'):
        super().__init__()
        self.account = account
        self.symbol = symbol
        self.date = date
        self.strategy = strategy  # 价格策略
        
        # 加载并筛选计划
        self.plans = self._load_plans(plan_df, symbol, date)
        self.current_plan_index = 0
        
        # 活跃订单管理：order_id -> {plan_info, order_time, cancel_time}
        self.active_orders = {}
        
        # 回调记录
        self.order_callbacks = []      # 下单回调记录
        self.cancel_callbacks = []     # 撤单回调记录
        self.trade_callbacks = []      # 成交回调记录
        self.position_updates = []     # 持仓更新记录
        
        # 行情数据记录
        self.tick_events = []          # Tick事件记录
        
        # 指令日志记录：plan_index -> 完整交易指令信息
        self.instruction_logs = {}
        
        # 当前持仓
        self.current_position = 0
        
        # 订单ID生成器
        self.order_id_counter = 1000
        
        print(f"[{self.account}] 被动单策略已启动")
        print(f"目标股票: {symbol}")
        print(f"目标日期: {date}")
        print(f"计划总数: {len(self.plans)}")
    
    def _load_plans(self, plan_df: pd.DataFrame, symbol: str, date: str) -> List[Dict]:
        """加载并筛选计划文件"""
        # 直接使用传入的 DataFrame
        df = plan_df.copy()
        
        # 筛选目标股票和日期
        df = df[df['stock_code'] == symbol].copy()
        df['order_date'] = pd.to_datetime(df['order_dt']).dt.date
        target_date = pd.to_datetime(date).date()
        df = df[df['order_date'] == target_date].copy()
        df = df[df["direction"] == "OrderSide.BUY"].copy()
        # 转换时间为datetime对象
        df['order_dt'] = pd.to_datetime(df['order_dt'])
        df['end_dt'] = pd.to_datetime(df['end_dt'])
        
        # 按时间排序
        df = df.sort_values('order_dt').reset_index(drop=True)
        
        # 转换为字典列表
        plans = df.to_dict('records')
        
        if self.symbol == '600105.SH':
            print(f"_load_plans for 600105.SH: loaded {len(plans)} plans")
        
        print(f"加载计划: 共 {len(plans)} 条")
        if len(plans) > 0:
            print(f"  首条: {plans[0]['order_dt']} {plans[0]['direction']} {plans[0]['filled_volume']}@{plans[0]['filled_price']}")
            print(f"  末条: {plans[-1]['order_dt']} {plans[-1]['direction']} {plans[-1]['filled_volume']}@{plans[-1]['filled_price']}")
        
        return plans
    
    def getStrategyId(self) -> str:
        """返回账户标识（必须重载）"""
        return self.account
    
    def _get_next_order_id(self) -> str:
        """生成下一个订单ID"""
        order_id = str(self.order_id_counter)
        self.order_id_counter += 1
        return order_id
    
    def _convert_price(self, price_value) -> int:
        """转换价格到系统内部单位（厘）"""
        if isinstance(price_value, (int, float)):
            if price_value < 1000:
                return int(price_value * 10000)
            else:
                return int(price_value)
        return 50000
    
    def _align_price(self, price: int) -> int:
        """价格对齐到100厘（分）"""
        if price <= 0:
            return 100
        return round(price / 100) * 100
    
    def _get_price_by_strategy(self, snapshot: Snapshot, direction: int, plan: Dict) -> int:
        """根据策略获取下单价格
        
        Args:
            snapshot: 市场快照
            direction: 方向（1=买入, 2=卖出）
            plan: 订单计划
            
        Returns:
            int: 价格（厘）
        """
        # 获取盘口价格
        bid1 = snapshot.bids[0] if snapshot.bids and len(snapshot.bids) > 0 and snapshot.bids[0] > 0 else 0
        ask1 = snapshot.asks[0] if snapshot.asks and len(snapshot.asks) > 0 and snapshot.asks[0] > 0 else 0
        bid2 = snapshot.bids[1] if snapshot.bids and len(snapshot.bids) > 1 and snapshot.bids[1] > 0 else bid1
        ask2 = snapshot.asks[1] if snapshot.asks and len(snapshot.asks) > 1 and snapshot.asks[1] > 0 else ask1
        bid3 = snapshot.bids[2] if snapshot.bids and len(snapshot.bids) > 2 and snapshot.bids[2] > 0 else bid2
        ask3 = snapshot.asks[2] if snapshot.asks and len(snapshot.asks) > 2 and snapshot.asks[2] > 0 else ask2
        
        # 默认价格（如果盘口价格无效）
        fallback_price = self._convert_price(plan['filled_price']) if plan['filled_price'] > 0 else 50000
        
        if bid1 == 0 or ask1 == 0:
            return fallback_price
        
        # 根据策略选择价格
        if self.strategy == 'passive_own':
            # 本方最优价
            price = bid1 if direction == 1 else ask1
        elif self.strategy == 'passive_opponent':
            # 对手最优价
            price = ask1 if direction == 1 else bid1
        elif self.strategy == 'passive_mid_own':
            # 中间价挂单(如果有，没有的话用本方最优)
            spread = int(ask1) - int(bid1)  # 确保整数运算
            if spread > 100:  # spread > 0.01元（100厘 = 0.01元）
                price = bid1 + 100 if direction == 1 else ask1 - 100
            else:
                price = bid1 if direction == 1 else ask1
        elif self.strategy == 'passive_mid_opponent':
            # 中间价挂单(如果有，没有的话用对手最优)
            spread = int(ask1) - int(bid1)  # 确保整数运算
            if spread > 100:  # spread > 0.01元（100厘 = 0.01元）
                price = bid1 + 100 if direction == 1 else ask1 - 100
            else:
                price = ask1 if direction == 1 else bid1
        elif self.strategy == 'passive_own_second':
            # 本方第二档挂单
            price = bid2 if direction == 1 else ask2
        elif self.strategy == 'passive_own_third':
            # 本方第三档挂单
            price = bid3 if direction == 1 else ask3
        else:
            # 默认使用本方最优价
            price = bid1 if direction == 1 else ask1
        
        return price if price > 0 else fallback_price
    
    def _parse_time_from_timestamp(self, timestamp_int: int) -> datetime:
        """从时间戳整数解析为datetime对象
        格式: HHMMSS.sss -> 例如 93000040 表示 09:30:00.040
        """
        hour = timestamp_int // 10000000
        minute = (timestamp_int // 100000) % 100
        second = (timestamp_int // 1000) % 100
        millisecond = timestamp_int % 1000
        
        # 构造完整datetime
        dt = datetime.strptime(self.date, "%Y-%m-%d")
        dt = dt.replace(hour=hour, minute=minute, second=second, microsecond=millisecond * 1000)
        return dt
    
    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        """处理订单推送事件（csord）- 暂不使用"""
        return []
    
    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        """处理成交推送事件（cstra）- 暂不使用"""
        return []
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        """处理Tick推送事件（cstick）- 主要逻辑"""
        # 记录Tick数据
        try:
            tick_record = {
                'Exchange': snapshot.Exchange,
                'Instrument': snapshot.Instrument,
                'Time': snapshot.Time,
                'Last': snapshot.last_price,
                'AskPrice': list(snapshot.asks),
                'BidPrice': list(snapshot.bids),
                'AskVolume': list(snapshot.ask_sizes),
                'BidVolume': list(snapshot.bid_sizes),
                'Volume': snapshot.Volume,
            }
            self.tick_events.append(tick_record)
        except Exception as e:
            print(f"⚠️ [Tick记录] 异常: {e}")
        
        # 解析当前时间
        current_time = self._parse_time_from_timestamp(snapshot.Time)
        events = []
        
        # 1. 检查是否需要挂新单
        if self.current_plan_index < len(self.plans):
            plan = self.plans[self.current_plan_index]
            plan_time = plan['order_dt']
            
            # 如果当前时间 >= 计划时间，挂单
            if current_time >= plan_time:
                order_events = self._create_order_from_plan(plan, snapshot, current_time)
                events.extend(order_events)
                self.current_plan_index += 1  # 移除该计划
                
                # 每10个订单打印进度
                if self.current_plan_index % 10 == 0:
                    print(f"\n📊 进度: {self.current_plan_index}/{len(self.plans)}, 活跃订单: {len(self.active_orders)}\n")
        
        # 2. 检查是否需要撤单（超时1分钟的订单）
        cancel_events = self._check_timeout_orders(current_time, snapshot)
        events.extend(cancel_events)
        
        return events
    
    def _create_order_from_plan(self, plan: Dict, snapshot: Snapshot, current_time: datetime) -> List[UserEvent]:
        """根据计划创建订单"""
        events = []
        
        # 解析方向
        direction_str = plan['direction']
        if 'BUY' in direction_str.upper():
            direction = 1  # 买入
        else:
            direction = 2  # 卖出
        
        # 根据策略获取价格
        price = self._get_price_by_strategy(snapshot, direction, plan)
        
        # 价格对齐
        price = self._align_price(price)
        
        # 订单量
        volume = int(plan['filled_volume'])
        
        # 生成订单ID
        order_id = f"{self.account}_{self._get_next_order_id()}"
        
        # 计算撤单时间（1分钟后）
        cancel_time = current_time + timedelta(minutes=1)
        
        # 记录订单信息
        plan_index = self.current_plan_index  # 当前计划索引
        self.active_orders[order_id] = {
            'plan': plan,
            'plan_index': plan_index,
            'order_time': current_time,
            'cancel_time': cancel_time,
            'filled': False,
            'snapshot': snapshot,  # 保存snapshot用于撤单
            'passive_price': price,  # 被动单价格
        }
        
        # 初始化指令日志
        self.instruction_logs[plan_index] = {
            'stock_code': self.symbol,
            'trade_date': self.date,
            'order_time': current_time.strftime("%Y-%m-%d %H:%M:%S"),
            'direction': 'BUY' if direction == 1 else 'SELL',
            'volume': volume,
            'passive_order_id': order_id,
            'passive_price': price / 10000.0,
            'passive_filled': False,
            'market_order_id': None,
            'market_price': None,
            'final_price': None,
            'final_amount': None,
            'trade_time': None,
            'execution_type': None,  # 'PASSIVE' 或 'MARKET'
        }
        
        # 创建订单事件 - 使用 make_order 函数
        event = make_order(
            Broker='',
            Account=self.account,
            Exchange=snapshot.Exchange,
            Instrument=snapshot.Instrument,
            OrderLocalID=order_id,
            OrderType=0,              # 限价单
            Direction=direction,      # 1=买入, 2=卖出
            Price=price,
            Volume=volume
        )
        events.append(event)
        print(f"📤 [挂单] {current_time.strftime('%H:%M:%S')} {order_id} "
              f"{'买' if direction == 1 else '卖'} {volume}@{price/10000:.2f}")
        return events
    
    def _check_timeout_orders(self, current_time: datetime, snapshot: Snapshot = None) -> List[UserEvent]:
        """检查并撤销超时订单，然后用市价单重新挂出"""
        events = []
        
        # 找出需要撤销的订单（只撤销非市价单且未标记为待撤单的）
        to_cancel = []
        for order_id, info in self.active_orders.items():
            # 跳过已成交的订单
            if info['filled']:
                continue
            # 跳过市价单（市价单不再撤单重挂，避免无限循环）
            if info.get('is_market_order', False):
                continue
            # 跳过已经在撤单中的订单
            if info.get('cancelling', False):
                continue
            # 检查是否超时
            if current_time >= info['cancel_time']:
                to_cancel.append((order_id, info))
        
        # 撤销订单并用市价单重新挂出
        for order_id, info in to_cancel:
            # 标记为撤单中，避免重复撤单
            info['cancelling'] = True
            
            # 使用保存的snapshot或传入的snapshot
            order_snapshot = info.get('snapshot', snapshot)
            if order_snapshot is not None:
                # 1. 先撤单
                cancel_event = make_cancel(
                    Broker='',
                    Account=self.account,
                    Exchange=order_snapshot.Exchange,
                    Instrument=order_snapshot.Instrument,
                    CancelOrderLocalID=f"C_{order_id}",
                    OrderLocalID=order_id
                )
                events.append(cancel_event)
                print(f"⏰ [超时撤单] {current_time.strftime('%H:%M:%S')} {order_id}")
                
                # 2. 用市价单重新挂出原订单的量
                plan = info['plan']
                direction_str = plan['direction']
                volume = int(plan['filled_volume'])
                
                # 解析方向
                if 'BUY' in direction_str.upper():
                    direction = 1  # 买入
                    # 市价买单使用 ask1 价格（吃卖一）
                    if snapshot and snapshot.asks and len(snapshot.asks) > 0 and snapshot.asks[0] > 0:
                        market_price = snapshot.asks[0]
                    else:
                        market_price = order_snapshot.asks[0]
                else:
                    direction = 2  # 卖出
                    # 市价卖单使用 bid1 价格（吃买一）
                    if snapshot and snapshot.bids and len(snapshot.bids) > 0 and snapshot.bids[0] > 0:
                        market_price = snapshot.bids[0]
                    else:
                        market_price = order_snapshot.bids[0]
                
                # 价格对齐
                market_price = self._align_price(market_price)
                
                # 生成新订单ID
                new_order_id = f"{self.account}_MKT_{self._get_next_order_id()}"
                
                # 获取原被动单的plan_index
                original_plan_index = info.get('plan_index')
                
                # 记录新订单信息（市价单标记为已处理，不再撤单重挂）
                self.active_orders[new_order_id] = {
                    'plan': plan,
                    'plan_index': original_plan_index,  # 继承原被动单的plan_index
                    'order_time': current_time,
                    'cancel_time': current_time + timedelta(days=1),  # 市价单不再撤单
                    'filled': False,
                    'snapshot': snapshot if snapshot else order_snapshot,
                    'is_market_order': True,  # 标记为市价单，避免再次撤单重挂
                    'passive_price': info.get('passive_price'),  # 继承原被动单价格
                    'market_price': market_price,  # 市价单价格
                }
                
                # 更新指令日志：记录市价单信息
                if original_plan_index is not None and original_plan_index in self.instruction_logs:
                    self.instruction_logs[original_plan_index]['market_order_id'] = new_order_id
                    self.instruction_logs[original_plan_index]['market_price'] = market_price / 10000.0
                
                # 创建市价单事件（使用限价单模拟，价格为对手价）
                
                market_event = make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=order_snapshot.Exchange,
                    Instrument=order_snapshot.Instrument,
                    OrderLocalID=new_order_id,
                    OrderType=0,              # 限价单（用对手价模拟市价单）
                    Direction=direction,
                    Price=market_price,
                    Volume=volume
                )
                events.append(market_event)
                
                print(f"📤 [市价补单] {current_time.strftime('%H:%M:%S')} {new_order_id} "
                      f"{'买' if direction == 1 else '卖'} {volume}@{market_price/10000:.2f} (原单: {order_id})")

        # 不在这里删除订单，等待撤单回调确认后再删除
        
        # 打印活跃订单数量和详情（大于1时）
        if len(self.active_orders) > 3:
            passive_orders = [(oid, info) for oid, info in self.active_orders.items() if not info.get('is_market_order', False)]
            market_orders = [(oid, info) for oid, info in self.active_orders.items() if info.get('is_market_order', False)]
            cancelling_orders = [(oid, info) for oid, info in self.active_orders.items() if info.get('cancelling', False)]
            
            print(f"⚠️ 活跃订单: {len(self.active_orders)} (被动单:{len(passive_orders)}, 市价单:{len(market_orders)}, 撤单中:{len(cancelling_orders)})")
            # for oid, info in self.active_orders.items():
            #     order_type = "市价" if info.get('is_market_order', False) else "被动"
            #     status = "撤单中" if info.get('cancelling', False) else "挂单中"
            #     order_time = info['order_time'].strftime('%H:%M:%S') if info.get('order_time') else 'N/A'
            #     cancel_time = info['cancel_time'].strftime('%H:%M:%S') if info.get('cancel_time') else 'N/A'
            #     print(f"   - {oid}: {order_type}, {status}, 下单时间:{order_time}, 撤单时间:{cancel_time}")
        
        return events
    
    # ========== 回调函数 ==========
    
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
            direction = callback.direction
            volume = callback.volume
            price = callback.price
            matchamount = callback.matchamount
            deltapos = callback.deltapos
            matchtime = callback.matchtime
            matchtype = callback.matchtype
            
            if matchtype == 'T':
                # 成交回调
                if order_id in self.active_orders:
                    order_info = self.active_orders[order_id]
                    order_info['filled'] = True
                    
                    # 解析成交时间（使用 pd.to_datetime 自动识别格式）
                    trade_time = pd.to_datetime(matchtime)
                    order_time = order_info['order_time']
                    
                    record = {
                        'order_id': order_id,
                        'direction': direction,
                        'volume': volume,
                        'price': price / 10000.0,
                        'match_amount': matchamount,
                        'order_time': order_time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                        'trade_time': trade_time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                        'latency_ms': (trade_time - order_time).total_seconds() * 1000,
                        'delta_position': deltapos,
                    }
                    self.trade_callbacks.append(record)
                    
                    # 更新持仓
                    old_position = self.current_position
                    self.current_position = deltapos
                    
                    position_record = {
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
                    
                    print(f"✅ [成交] {order_id}: {volume}@{price/10000:.2f}, "
                        f"延迟: {record['latency_ms']:.1f}ms, 持仓: {old_position} → {self.current_position}")
                    
                    # 更新指令日志：记录成交信息
                    plan_index = order_info.get('plan_index')
                    if plan_index is not None and plan_index in self.instruction_logs:
                        log = self.instruction_logs[plan_index]
                        is_market = order_info.get('is_market_order', False)
                        
                        log['final_price'] = price / 10000.0
                        log['final_amount'] = matchamount
                        log['trade_time'] = trade_time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                        
                        if is_market:
                            log['execution_type'] = 'MARKET'
                            log['passive_filled'] = False
                        else:
                            log['execution_type'] = 'PASSIVE'
                            log['passive_filled'] = True
                    
                    # 从活跃订单中移除已成交订单
                    del self.active_orders[order_id]
                else:
                    # 订单不在active_orders中，可能已经被处理过
                    print(f"⚠️ [成交回调] 订单 {order_id} 不在活跃订单列表中")
                
            elif matchtype == 'D':
                # 撤单回调 - 在这里删除订单
                record = {
                    'order_id': order_id,
                    'direction': direction,
                    'volume': volume,
                    'price': price / 10000.0,
                    'delta_position': deltapos,
                    'match_time': matchtime,
                }
                self.cancel_callbacks.append(record)
                
                if order_id in self.active_orders:
                    # 从活跃订单中移除
                    del self.active_orders[order_id]
                    print(f"❌ [撤单确认] {order_id}")
                else:
                    print(f"❌ [撤单确认-订单不存在] {order_id}")
                
        except Exception as e:
            print(f"❌ [交易回调] 处理异常: {e}")
            import traceback
            traceback.print_exc()

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
            order_id = callback.orderlocalid
            order_time = callback.time
            exchange = callback.exchange
            ask1 = callback.ask1
            bid1 = callback.bid1
            deltapos = callback.deltapos
            price = callback.price
            volume = callback.volume
            direction = callback.direction
            
            record = {
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
            }
            self.order_callbacks.append(record)
            
        except Exception as e:
            print(f"❌ [下单回调] 处理异常: {e}")
            import traceback
            traceback.print_exc()
    
    def save_records(self, output_dir: str, save_detailed_logs: bool = False):
        """保存记录文件
        
        Args:
            output_dir: 输出目录
            save_detailed_logs: 是否保存详细日志文件
        """
        os.makedirs(output_dir, exist_ok=True)
        
        if save_detailed_logs:
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
            
            # 5. Tick事件记录
            if self.tick_events:
                file_path = os.path.join(output_dir, "tick_events.csv")
                pd.DataFrame(self.tick_events).to_csv(file_path, index=False, encoding='utf-8')
                print(f"✅ Tick事件记录: {file_path} ({len(self.tick_events)} 条)")
        
        # 6. 保存指令日志（整理后的统一格式）
        if save_detailed_logs:
            self._save_instruction_log(output_dir)
    
    def _save_instruction_log(self, output_dir: str):
        """保存整理后的指令日志"""
        if not self.instruction_logs:
            return
        
        # 将instruction_logs转换为DataFrame格式
        records = []
        for plan_index in sorted(self.instruction_logs.keys()):
            log = self.instruction_logs[plan_index]
            record = {
                'stock_code': log['stock_code'],
                'trade_date': log['trade_date'],
                'order_time': log['order_time'],
                'direction': log['direction'],
                'volume': log['volume'],
                'passive_price': log['passive_price'],
                'passive_filled': log['passive_filled'],
                'market_price': log['market_price'],
                'final_price': log['final_price'],
                'final_amount': log['final_amount'],
                'trade_time': log['trade_time'],
                'execution_type': log['execution_type'],
            }
            records.append(record)
        
        # 保存为CSV
        df = pd.DataFrame(records)
        file_path = os.path.join(output_dir, "instruction_log.csv")
        df.to_csv(file_path, index=False, encoding='utf-8')
        
        # 统计信息
        passive_count = df[df['passive_filled'] == True].shape[0] if 'passive_filled' in df.columns else 0
        market_count = df[df['execution_type'] == 'MARKET'].shape[0] if 'execution_type' in df.columns else 0
        
        print(f"\n📋 指令日志已保存: {file_path}")
        print(f"   总计: {len(records)} 条指令")
        print(f"   被动单成交: {passive_count} 条")
        print(f"   市价单成交: {market_count} 条")
    
    def print_summary(self):
        """打印策略执行摘要"""
        print(f"\n{'='*60}")
        print(f"📊 被动单策略执行报告")
        print(f"{'='*60}")
        print(f"目标股票: {self.symbol}")
        print(f"目标日期: {self.date}")
        print(f"计划总数: {len(self.plans)}")
        print(f"已执行计划: {self.current_plan_index}")
        print(f"成交订单数: {len(self.trade_callbacks)}")
        print(f"撤单订单数: {len(self.cancel_callbacks)}")
        print(f"最终持仓: {self.current_position}")
        print(f"{'='*60}")


# ========== 数据加载函数 ==========

def get_cstick_ad(date_str, sym):
    """从adata获取tick数据"""
    cstick = adata.get_data('cstick', date_str, date_str, [sym])
    return cstick

def get_csord_ad(date_str, sym):
    """从adata获取逐笔委托数据"""
    csord = adata.get_data('csord', date_str, date_str, [sym])
    return csord

def get_cstra_ad(date_str, sym):
    """从adata获取逐笔成交数据"""
    cstra = adata.get_data('cstra', date_str, date_str, [sym])
    return cstra

def run_passive_strategy(symbol: str, date: str, plan_df: pd.DataFrame, output_dir: str, save_detailed_logs: bool = True) -> bool:
    """运行被动单策略（新接口）
    
    Args:
        symbol: 股票代码
        date: 日期
        plan_df: 计划 DataFrame
        output_dir: 输出目录
        save_detailed_logs: 是否保存详细日志文件
    """
    try:
        print(f"🚀 开始被动单策略")
        print(f"合约: {symbol}")
        print(f"日期: {date}")
        print(f"计划 DataFrame: {len(plan_df)} 行")
        print(f"输出路径: {output_dir}")
        
        # ========== 1. 准备数据 ==========
        print(f"\n📖 加载数据...")
        cstick_df = get_cstick_ad(date, symbol)
        order_df = get_csord_ad(date, symbol)
        trade_df = get_cstra_ad(date, symbol)
        
        print(f"   ✅ Tick数据: {len(cstick_df)} 行")
        print(f"   ✅ 委托数据: {len(order_df)} 行")
        print(f"   ✅ 成交数据: {len(trade_df)} 行")
        
        # 组织成字典格式: {symbol: (cstick_df, order_df, trade_df)}
        data = {
            symbol: (cstick_df, order_df, trade_df)
        }
        
        # ========== 2. 创建策略 ==========
        strategy = PassiveOrderStrategy(
            account="PASSIVE",
            symbol=symbol,
            date=date,
            plan_df=plan_df
        )
        
        # ========== 3. 运行回测 ==========
        print(f"\n🚀 开始运行回测...")
        success = run_backtest(
            data_dict=data,
            strategy=strategy,
            output_dir=output_dir
        )
        
        if success:
            print(f"\n✅ 回测完成!")
            
            # 保存记录
            strategy.save_records(output_dir, save_detailed_logs)
            
            # 打印报告
            strategy.print_summary()
            
            return True
        else:
            print(f"❌ 回测失败")
            return False
        
    except Exception as e:
        print(f"❌ 回测失败: {e}")
        import traceback
        traceback.print_exc()
        return False


def main():
    """主函数"""
    # 登录adata
    username = os.getenv('MY_USER_NAME') 
    password = os.getenv('MY_PASSWORD')
    adata.login(username, password)
    
    # 配置参数
    symbol = "601607.SH"
    date = "2024-01-02" 
    plan_file = "plan/passive_log.csv"
    output_dir = f"./passive_order_output/{symbol}_{date}"
    
    # 检查计划文件
    if not os.path.exists(plan_file):
        print(f"❌ 缺少计划文件: {plan_file}")
        return 1
    
    print(f"✅ 找到计划文件: {plan_file}")
    
    # 读取计划文件为 DataFrame
    plan_df = pd.read_csv(plan_file)
    
    # 运行被动单策略
    success = run_passive_strategy(symbol, date, plan_df, output_dir, save_detailed_logs=True)
    
    if success:
        print(f"\n🎉 被动单策略执行成功！")
        print(f"\n📁 输出文件目录: {output_dir}")
        return 0
    else:
        print("❌ 被动单策略执行失败")
        return 1


if __name__ == "__main__":
    exit(main())