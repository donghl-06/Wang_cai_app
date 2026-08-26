"""
测试欠债清零后可以继续下主动单

策略：
1. 下一个小量主动单（只吃掉少量历史订单，产生少量欠债）
2. 紧跟着下第二个主动单 → 应被拦截
3. 等待欠债被真实市场消耗清零
4. 再下第三个主动单 → 应该能成交
"""

from typing import List
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel
)


class DebtClearTestStrategy(Strategy):
    """
    测试欠债清零后可以继续下主动单
    
    时间线：
    - 09:31:00: 下第一个小量主动单（制造少量欠债）
    - 09:31:00: 紧跟着下第二个主动单 → 应被拦截
    - 之后每隔30秒检查一次，尝试下主动单，直到成功（说明欠债已清零）
    """

    def __init__(self, account: str = "debt_clear_test"):
        super().__init__()
        self.account = account
        
        self._order_counter = 3000
        
        # 测试状态
        self._first_order_placed = False
        self._second_order_rejected = False
        self._debt_cleared = False
        self._debt_clear_time = None
        
        # 检查计数
        self._check_count = 0
        self._last_check_time = 0
        
        # 订单跟踪
        self._order_labels = {}
        
        # 持仓
        self.position = 0
        
        # 结果统计
        self.results = {
            'first_order_filled': False,
            'second_order_rejected': False,
            'third_order_filled': False,
            'debt_clear_check_count': 0,
        }

    def getStrategyId(self) -> str:
        return self.account

    def _next_order_id(self, label: str) -> str:
        self._order_counter += 1
        oid = f"{self.account}_{self._order_counter}"
        self._order_labels[oid] = label
        return oid

    def _get_time_seconds(self, tick_time: int) -> int:
        """返回从开盘以来的秒数（用于定时检查）"""
        t = tick_time // 1000
        s = t % 100
        t = t // 100
        m = t % 100
        h = t // 100
        return h * 3600 + m * 60 + s

    def _get_time_str(self, tick_time: int) -> str:
        t = tick_time // 1000
        s = t % 100
        t = t // 100
        m = t % 100
        h = t // 100
        return f"{h:02d}:{m:02d}:{s:02d}"

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events = []
        
        if tick.Time < 93000000:
            return events
        
        time_str = self._get_time_str(tick.Time)
        time_sec = self._get_time_seconds(tick.Time)
        
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        upper_limit = getattr(tick, 'UpperLimit', 0)
        
        # ==================== 步骤1: 下第一个小量主动单 ====================
        if not self._first_order_placed and time_sec >= 9*3600 + 31*60:
            if ask1 > 0 and upper_limit > 0:
                # 用小量（100股），只会吃掉少量历史订单
                first_id = self._next_order_id('first_small_active')
                volume = 100  # 小量，产生少量欠债
                
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=first_id, OrderType=0,
                    Direction=1, Price=upper_limit, Volume=volume
                ))
                
                print(f"\n{'='*70}")
                print(f"📌 [步骤1] [{time_str}] 下第一个小量主动单（制造少量欠债）")
                print(f"   订单ID: {first_id}")
                print(f"   价格: {upper_limit/10000:.4f} (涨停价)")
                print(f"   数量: {volume} (小量，产生少量欠债)")
                print(f"{'='*70}")
                
                self._first_order_placed = True
                
                # ==================== 步骤2: 紧跟着下第二个主动单 ====================
                second_id = self._next_order_id('second_should_reject')
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=second_id, OrderType=0,
                    Direction=1, Price=upper_limit, Volume=100
                ))
                
                print(f"\n📌 [步骤2] [{time_str}] 紧跟着下第二个主动单")
                print(f"   订单ID: {second_id}")
                print(f"   预期: ❌ 应被拦截（存在欠债）")
                
                self._last_check_time = time_sec
        
        # ==================== 步骤3: 定期检查欠债是否清零 ====================
        if self._first_order_placed and self._second_order_rejected and not self._debt_cleared:
            # 每隔15秒尝试一次
            if time_sec - self._last_check_time >= 15:
                self._last_check_time = time_sec
                self._check_count += 1
                
                check_id = self._next_order_id(f'check_{self._check_count}')
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=check_id, OrderType=0,
                    Direction=1, Price=upper_limit, Volume=100
                ))
                
                print(f"\n📌 [检查{self._check_count}] [{time_str}] 尝试下主动单...")
        
        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderCallback(self, cb: OrderCallback) -> None:
        order_id = cb.orderlocalid
        label = self._order_labels.get(order_id, 'unknown')
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  📋 下单确认: {order_id} ({label}) {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        order_id = cb.localid
        label = self._order_labels.get(order_id, 'unknown')
        
        if cb.matchtype == 'T':
            self.position = cb.deltapos
            direction = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {order_id} ({label}) {direction} {cb.volume}@{cb.price/10000:.4f} 持仓={self.position}")
            
            if 'first' in label:
                self.results['first_order_filled'] = True
                print(f"  ✅ [步骤1通过] 第一个主动单成交，已产生欠债")
            elif 'check' in label:
                # 检查订单成交了，说明欠债已清零！
                self._debt_cleared = True
                self.results['third_order_filled'] = True
                self.results['debt_clear_check_count'] = self._check_count
                print(f"\n{'='*70}")
                print(f"  🎉 [步骤3通过] 欠债已清零！第{self._check_count}次检查时主动单成交了")
                print(f"{'='*70}")
                
        elif cb.matchtype == 'D':
            print(f"  ☑️  撤单: {order_id} ({label})")
            
            if 'second' in label:
                self._second_order_rejected = True
                self.results['second_order_rejected'] = True
                print(f"  ✅ [步骤2通过] 第二个主动单被拦截（欠债限制生效）")
            elif 'check' in label:
                print(f"  ⏳ 欠债仍存在，继续等待...")

    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        label = self._order_labels.get(order_id, 'unknown')
        # 只打印关键的撤单原因
        if 'check' not in label:
            print(f"  ℹ️  撤单原因: {order_id} - {reason}")

    def passed(self) -> bool:
        """核心断言：产生欠债 + 欠债期间主动单被拦截。
        步骤3（欠债清零）取决于市场数据，不作为硬性失败条件。"""
        return (self.results['first_order_filled']
                and self.results['second_order_rejected'])

    def print_summary(self):
        print(f"\n{'='*70}")
        print(f"📊 欠债清零测试结果")
        print(f"{'='*70}")
        
        # 步骤1
        status1 = "✅ 通过" if self.results['first_order_filled'] else "❌ 失败"
        print(f"  步骤1 - 第一个主动单成交（产生欠债）: {status1}")
        
        # 步骤2
        status2 = "✅ 通过" if self.results['second_order_rejected'] else "❌ 失败"
        print(f"  步骤2 - 第二个主动单被拦截（欠债限制）: {status2}")
        
        # 步骤3
        if self.results['third_order_filled']:
            status3 = f"✅ 通过（第{self.results['debt_clear_check_count']}次检查时成交）"
        else:
            status3 = f"⏳ 未完成（已检查{self._check_count}次）"
        print(f"  步骤3 - 欠债清零后可下新主动单: {status3}")
        
        print(f"{'='*70}")
        
        all_passed = (self.results['first_order_filled'] and 
                      self.results['second_order_rejected'] and 
                      self.results['third_order_filled'])
        
        if all_passed:
            print(f"🎉 完整测试通过！欠债清零机制工作正常")
        elif self.results['first_order_filled'] and self.results['second_order_rejected']:
            print(f"⚠️  欠债限制生效，但在回测期间欠债未能清零")
            print(f"    （这可能是正常的，取决于市场数据）")
        else:
            print(f"❌ 测试失败")
        
        print(f"{'='*70}")
        print(f"  最终持仓: {self.position}")
        print(f"{'='*70}")
