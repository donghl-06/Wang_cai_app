"""
严格主动单模式 - 全面测试套件

测试场景覆盖：
1. 主动买单成交 + 记录欠债
2. 有欠债时禁止新的主动买单
3. 有欠债时禁止新的主动卖单
4. 被动买单不受欠债限制（可以正常挂单）
5. 被动卖单不受欠债限制（可以正常挂单）
6. 连续多个主动单全部被拦截
7. 主动卖单成交 + 记录欠债
"""

from typing import List
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel
)


class ComprehensiveStrictModeTest(Strategy):
    """
    严格主动单模式全面测试策略
    
    时间线：
    - 09:31:00: 测试1 - 主动买单成交（制造欠债）
    - 09:31:00: 测试2 - 有欠债时下主动买单（应被拦截）
    - 09:31:00: 测试3 - 有欠债时下主动卖单（应被拦截）
    - 09:31:03: 测试4 - 有欠债时下被动买单（应能挂单）
    - 09:31:06: 测试5 - 有欠债时下被动卖单（应能挂单）
    - 09:35:00: 测试6 - 主动卖单成交测试
    - 10:00:00: 测试7 - 欠债可能已清除，再次下主动单
    """

    def __init__(self, account: str = "comprehensive_test", strict_mode_expected: bool = True):
        super().__init__()
        self.account = account
        
        # 期望的引擎模式：True=严格主动单模式开启（主动单应被拦截），
        # False=严格模式关闭（主动单应正常成交）。断言方向随之取反。
        self.strict_mode_expected = strict_mode_expected
        
        # 订单ID计数器
        self._order_counter = 2000
        
        # 测试状态跟踪
        self._tests_completed = set()
        
        # 测试结果统计
        self.results = {
            'test1_active_buy_filled': False,          # 主动买单成交
            'test2_active_buy_rejected': False,        # 有欠债时主动买单被拦截
            'test2_active_buy_filled': False,          # 有欠债时主动买单成交
            'test3_active_sell_rejected': False,       # 有欠债时主动卖单被拦截
            'test3_active_sell_filled': False,         # 有欠债时主动卖单成交
            'test4_passive_buy_accepted': False,       # 被动买单可以挂单
            'test5_passive_sell_accepted': False,      # 被动卖单可以挂单
            'test6_active_sell_filled': False,         # 主动卖单成交
            'test7_after_debt_cleared': None,          # 欠债清除后能下主动单
        }
        
        # 订单ID映射
        self._order_ids = {}
        
        # 持仓跟踪
        self.position = 0
        
        # 盘口缓存
        self._last_tick = None

    def getStrategyId(self) -> str:
        return self.account

    def _next_order_id(self, label: str) -> str:
        self._order_counter += 1
        order_id = f"{self.account}_{self._order_counter}"
        self._order_ids[order_id] = label
        return order_id

    def _get_time_str(self, tick_time: int) -> str:
        t = tick_time // 1000
        second = t % 100
        t = t // 100
        minute = t % 100
        hour = t // 100
        return f"{hour:02d}:{minute:02d}:{second:02d}"

    def _check_time(self, tick_time: int, hour: int, minute: int, second: int) -> bool:
        t = tick_time // 1000
        s = t % 100
        t = t // 100
        m = t % 100
        h = t // 100
        return h == hour and m == minute and s == second

    def _debt_expectation(self, when_strict: str, when_loose: str) -> str:
        return when_strict if self.strict_mode_expected else when_loose

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events = []
        
        # 跳过集合竞价
        if tick.Time < 93000000:
            return events
        
        self._last_tick = tick
        time_str = self._get_time_str(tick.Time)
        
        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        upper_limit = getattr(tick, 'UpperLimit', 0)
        lower_limit = getattr(tick, 'LowerLimit', 0)
        
        # ==================== 测试1: 主动买单成交（制造欠债）====================
        if 'test1' not in self._tests_completed and self._check_time(tick.Time, 9, 31, 0):
            if ask1 > 0 and upper_limit > 0:
                order_id = self._next_order_id('test1_active_buy')
                price = upper_limit  # 用涨停价确保是主动单
                volume = 500_000     # 50万股
                
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=order_id, OrderType=0,
                    Direction=1, Price=price, Volume=volume
                ))
                
                print(f"\n{'='*70}")
                print(f"📌 [测试1] [{time_str}] 主动买单成交测试（制造欠债）")
                print(f"   订单ID: {order_id}")
                print(f"   价格: {price/10000:.4f} (涨停价, 确保主动单)")
                print(f"   数量: {volume}")
                print(f"   预期: 成交（可能部分成交），记录欠债")
                print(f"{'='*70}")
                
                self._tests_completed.add('test1')
                
                # ==================== 测试2: 有欠债时下主动买单 ====================
                order_id2 = self._next_order_id('test2_active_buy_with_debt')
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=order_id2, OrderType=0,
                    Direction=1, Price=upper_limit, Volume=100
                ))
                
                print(f"\n📌 [测试2] [{time_str}] 有欠债时下主动买单")
                print(f"   订单ID: {order_id2}")
                print(f"   价格: {upper_limit/10000:.4f} (涨停价, 主动单)")
                print(f"   预期: {self._debt_expectation('❌ 应被拒绝（存在欠债）', '✅ 应正常成交（严格模式关闭）')}")
                
                self._tests_completed.add('test2')
                
                # ==================== 测试3: 有欠债时下主动卖单 ====================
                if lower_limit > 0:
                    order_id3 = self._next_order_id('test3_active_sell_with_debt')
                    events.append(make_order(
                        Broker='', Account=self.account,
                        Exchange=tick.Exchange, Instrument=tick.Instrument,
                        OrderLocalID=order_id3, OrderType=0,
                        Direction=2, Price=lower_limit, Volume=100  # 跌停价卖出
                    ))
                    
                    print(f"\n📌 [测试3] [{time_str}] 有欠债时下主动卖单")
                    print(f"   订单ID: {order_id3}")
                    print(f"   价格: {lower_limit/10000:.4f} (跌停价, 主动卖)")
                    print(f"   预期: {self._debt_expectation('❌ 应被拒绝（存在欠债）', '✅ 应正常成交（严格模式关闭）')}")
                    
                    self._tests_completed.add('test3')
        
        # ==================== 测试4: 有欠债时下被动买单 ====================
        if 'test4' not in self._tests_completed and self._check_time(tick.Time, 9, 31, 3):
            if bid1 > 0 and lower_limit > 0:
                # 被动买单：价格 < bestAsk，不会立即成交
                passive_price = lower_limit  # 跌停价买入 = 一定是被动单
                order_id4 = self._next_order_id('test4_passive_buy')
                
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=order_id4, OrderType=0,
                    Direction=1, Price=passive_price, Volume=100
                ))
                
                print(f"\n📌 [测试4] [{time_str}] 有欠债时下被动买单")
                print(f"   订单ID: {order_id4}")
                print(f"   价格: {passive_price/10000:.4f} (跌停价, 被动单)")
                print(f"   预期: ✅ 应能挂单（被动单不受欠债限制）")
                
                self._tests_completed.add('test4')
        
        # ==================== 测试5: 有欠债时下被动卖单 ====================
        if 'test5' not in self._tests_completed and self._check_time(tick.Time, 9, 31, 6):
            if ask1 > 0 and upper_limit > 0:
                # 被动卖单：价格 > bestBid，不会立即成交
                passive_price = upper_limit  # 涨停价卖出 = 一定是被动单
                order_id5 = self._next_order_id('test5_passive_sell')
                
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=order_id5, OrderType=0,
                    Direction=2, Price=passive_price, Volume=100
                ))
                
                print(f"\n📌 [测试5] [{time_str}] 有欠债时下被动卖单")
                print(f"   订单ID: {order_id5}")
                print(f"   价格: {passive_price/10000:.4f} (涨停价, 被动卖)")
                print(f"   预期: ✅ 应能挂单（被动单不受欠债限制）")
                
                self._tests_completed.add('test5')
        
        # ==================== 测试6: 尝试第三个主动单（应仍被拦截）====================
        if 'test6' not in self._tests_completed and self._check_time(tick.Time, 9, 32, 0):
            if upper_limit > 0:
                order_id6 = self._next_order_id('test6_third_active')
                
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=order_id6, OrderType=0,
                    Direction=1, Price=upper_limit, Volume=100
                ))
                
                print(f"\n📌 [测试6] [{time_str}] 再次尝试主动买单")
                print(f"   订单ID: {order_id6}")
                print(f"   预期: {self._debt_expectation('❌ 应被拒绝（欠债可能仍未清除）', '✅ 应正常成交（严格模式关闭）')}")
                
                self._tests_completed.add('test6')
        
        # ==================== 测试7: 10分钟后尝试（欠债可能已清除）====================
        if 'test7' not in self._tests_completed and self._check_time(tick.Time, 10, 0, 0):
            if upper_limit > 0:
                order_id7 = self._next_order_id('test7_after_wait')
                
                events.append(make_order(
                    Broker='', Account=self.account,
                    Exchange=tick.Exchange, Instrument=tick.Instrument,
                    OrderLocalID=order_id7, OrderType=0,
                    Direction=1, Price=upper_limit, Volume=100
                ))
                
                print(f"\n📌 [测试7] [{time_str}] 10分钟后再次尝试主动单")
                print(f"   订单ID: {order_id7}")
                print(f"   预期: 取决于欠债是否已被真实市场消耗")
                
                self._tests_completed.add('test7')
        
        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderCallback(self, cb: OrderCallback) -> None:
        """下单确认"""
        order_id = cb.orderlocalid
        label = self._order_ids.get(order_id, 'unknown')
        direction = "买" if cb.direction == 1 else "卖"
        
        # 被动单收到下单确认说明挂单成功
        if 'passive' in label:
            if 'test4' in label:
                self.results['test4_passive_buy_accepted'] = True
                print(f"  ✅ [测试4通过] 被动买单挂单成功: {order_id}")
            elif 'test5' in label:
                self.results['test5_passive_sell_accepted'] = True
                print(f"  ✅ [测试5通过] 被动卖单挂单成功: {order_id}")
        else:
            print(f"  📋 下单确认: {order_id} ({label}) {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        """成交/撤单回调"""
        order_id = cb.localid
        label = self._order_ids.get(order_id, 'unknown')
        
        if cb.matchtype == 'T':
            # 成交
            self.position = cb.deltapos
            direction = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {order_id} ({label}) {direction} {cb.volume}@{cb.price/10000:.4f} 持仓={self.position}")
            
            # 记录测试结果
            if 'test1' in label:
                self.results['test1_active_buy_filled'] = True
                print(f"  ✅ [测试1通过] 主动买单成交成功")
            elif 'test6_active_sell' in label:
                self.results['test6_active_sell_filled'] = True
            elif 'test7' in label:
                self.results['test7_after_debt_cleared'] = 'filled'
                print(f"  ℹ️  [测试7] 主动单成交，说明欠债已清除")
            elif 'test2' in label or 'test3' in label:
                # 严格模式开启时这些不应该成交；关闭时成交才是正确行为
                if 'test2' in label:
                    self.results['test2_active_buy_filled'] = True
                else:
                    self.results['test3_active_sell_filled'] = True
                if self.strict_mode_expected:
                    print(f"  ❌ [测试失败] {label} 不应该成交但成交了！")
                else:
                    print(f"  ✅ [通过] {label} 严格模式关闭，主动单未被拦截")
                
        elif cb.matchtype == 'D':
            # 撤单/拒绝
            print(f"  ☑️  撤单: {order_id} ({label})")
            
            if 'test2' in label:
                self.results['test2_active_buy_rejected'] = True
                if self.strict_mode_expected:
                    print(f"  ✅ [测试2通过] 有欠债时主动买单被拦截")
                else:
                    print(f"  ❌ [测试失败] 严格模式关闭，主动买单不应被拦截")
            elif 'test3' in label:
                self.results['test3_active_sell_rejected'] = True
                if self.strict_mode_expected:
                    print(f"  ✅ [测试3通过] 有欠债时主动卖单被拦截")
                else:
                    print(f"  ❌ [测试失败] 严格模式关闭，主动卖单不应被拦截")
            elif 'test6' in label:
                print(f"  ℹ️  [测试6] 第三个主动单被拦截（欠债仍存在）")
            elif 'test7' in label:
                self.results['test7_after_debt_cleared'] = 'rejected'
                print(f"  ℹ️  [测试7] 主动单被拦截，说明欠债仍未清除")

    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        label = self._order_ids.get(order_id, 'unknown')
        print(f"  ℹ️  撤单原因: {order_id} ({label}) - {reason}")

    def _expected_checks(self):
        """返回 [(描述, 是否通过)]，test2/test3 的断言方向随期望模式取反"""
        if self.strict_mode_expected:
            test2_desc = "有欠债时主动买单被拦截"
            test2_ok = self.results['test2_active_buy_rejected']
            test3_desc = "有欠债时主动卖单被拦截"
            test3_ok = self.results['test3_active_sell_rejected']
        else:
            test2_desc = "严格模式关闭时主动买单正常成交"
            test2_ok = (self.results['test2_active_buy_filled']
                        and not self.results['test2_active_buy_rejected'])
            test3_desc = "严格模式关闭时主动卖单正常成交"
            test3_ok = (self.results['test3_active_sell_filled']
                        and not self.results['test3_active_sell_rejected'])
        
        return [
            ("测试1 - 主动买单成交（制造欠债）", self.results['test1_active_buy_filled']),
            (f"测试2 - {test2_desc}", test2_ok),
            (f"测试3 - {test3_desc}", test3_ok),
            ("测试4 - 被动买单不受欠债限制", self.results['test4_passive_buy_accepted']),
            ("测试5 - 被动卖单不受欠债限制", self.results['test5_passive_sell_accepted']),
        ]

    def passed(self) -> bool:
        """核心断言是否全部通过（供 runner 决定退出码）"""
        return all(ok for _, ok in self._expected_checks())

    def print_summary(self):
        """打印测试结果汇总"""
        mode_label = "开启" if self.strict_mode_expected else "关闭"
        print(f"\n{'='*70}")
        print(f"📊 严格主动单模式（期望{mode_label}） - 全面测试结果")
        print(f"{'='*70}")
        
        checks = self._expected_checks()
        for desc, ok in checks:
            print(f"  {desc}: {'✅ 通过' if ok else '❌ 失败'}")
        all_passed = all(ok for _, ok in checks)
        
        # 测试7: 等待后
        if self.results['test7_after_debt_cleared'] == 'filled':
            print(f"  测试7 - 10分钟后主动单: ✅ 成交（欠债已清除）")
        elif self.results['test7_after_debt_cleared'] == 'rejected':
            print(f"  测试7 - 10分钟后主动单: ⚠️  被拦截（欠债仍存在）")
        else:
            print(f"  测试7 - 10分钟后主动单: ⏳ 未执行")
        
        print(f"{'='*70}")
        
        if all_passed:
            print(f"🎉 核心测试全部通过！严格主动单模式工作正常")
        else:
            print(f"⚠️  有测试未通过，请检查上述详情")
        
        print(f"{'='*70}")
        print(f"  最终持仓: {self.position}")
        print(f"{'='*70}")
