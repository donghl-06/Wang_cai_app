import sys
import pathlib
import pandas as pd
import os
from datetime import datetime
from typing import List, Dict, Optional
import json

from wangcai_bt import (
    BacktestEngine, Strategy,
    Direction, OrderType,
    Event, UserEvent, Execution, Snapshot,
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
        
        # 4个核心记录文件
        self.order_callbacks = []      # 下单回调记录
        self.cancel_callbacks = []     # 撤单回调记录
        self.trade_callbacks = []      # 成交回调记录
        self.position_updates = []     # 持仓更新记录
        
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
    
    def onOrderEvent(self, event: Event) -> List[UserEvent]:
        """处理订单推送事件（csord）"""
        # 转换价格
        ref_price = self._convert_price(event.price)
        return self._process_event('csord', event.datetime, event.sym, ref_price)
    
    def onTradeEvent(self, execution: Execution, datetime: str) -> List[UserEvent]:
        """处理成交推送事件（cstra）"""
        # 使用成交价格作为参考
        ref_price = execution.price if execution.price > 1000 else execution.price * 10000
        symbol = "000027.SZ"  # 使用实际合约
        return self._process_event('cstra', datetime, symbol, ref_price)
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        """处理Tick推送事件（cstick）"""
        # 限制处理频率
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
        """保存4个核心记录文件"""
        os.makedirs(output_dir, exist_ok=True)
        
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
        engine = BacktestEngine(symbol=symbol, date=date, data_path=data_path)
        
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
    symbol = "000027.SZ"
    date = "2022-01-07"
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
        print(f"\n📁 输出的4个核心文件：")
        print(f"   1️⃣ {output_dir}/order_callbacks.csv   - 下单回调记录")
        print(f"   2️⃣ {output_dir}/cancel_callbacks.csv  - 撤单回调记录")
        print(f"   3️⃣ {output_dir}/trade_callbacks.csv   - 成交回调记录")
        print(f"   4️⃣ {output_dir}/position_updates.csv  - 持仓信息更新")
        return 0
    else:
        print("❌ 接口验证测试失败")
        return 1


if __name__ == "__main__":
    exit(main())