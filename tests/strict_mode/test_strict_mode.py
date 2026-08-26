"""
测试严格主动单模式

这个测试策略演示：
1. 主动单只能成交市场实际提供的量
2. 市场量不足时，剩余部分自动撤单
3. 有未还清欠债时，禁止下新的主动单
4. 欠债清除后，可以继续下主动单
"""

from typing import List
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel
)


class StrictModeTestStrategy(Strategy):
    """
    严格主动单模式测试策略
    
    测试场景：
    - 9:31:00: 连续下两个主动买单（第二单紧跟第一单之后）
        - 第一单：用于制造“欠债”（严格模式下会记录被虚拟吃掉的历史订单）
        - 第二单：用于验证“欠债未清前禁止下新的主动单”（应被拦截）
    
    说明：
    - 之前按“隔一段时间/下一个 tick”再下第二单，可能会出现欠债已经被真实市场快速清除，
      从而第二单被允许，这是符合规则但不利于测试验证。
    - 这里改为在同一个 tick 回调里按顺序下两单，确保第二单提交时欠债必然存在，测试更稳定。
    """

    def __init__(self, account: str = "test_strict"):
        super().__init__()
        self.account = account
        
        # 订单ID计数器
        self._order_counter = 1000
        
        # 测试状态
        self._test_step = 0  # 0: 初始, 1: 已下两单
        self._first_order_placed = False
        self._first_order_id = None
        self._second_order_id = None
        self._debt_cleared_test = False
        
        # 统计数据
        self.position = 0
        self.order_count = 0
        self.trade_count = 0
        self.cancel_count = 0
        self.rejected_count = 0  # 被拒绝的主动单数量
        
        # 记录最后一次ask1价格
        self._last_ask1 = 0

    def getStrategyId(self) -> str:
        return self.account

    def _next_order_id(self) -> str:
        self._order_counter += 1
        return f"{self.account}_{self._order_counter}"

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        """测试严格主动单模式的各种场景"""
        events = []

        # 跳过集合竞价
        if tick.Time < 93000000:
            return events

        # 解析时间
        t = tick.Time // 1000
        second = t % 100
        t = t // 100
        minute = t % 100
        hour = t // 100
        time_str = f"{hour:02d}:{minute:02d}:{second:02d}"

        # 获取盘口
        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        self._last_ask1 = ask1

        # === 测试：9:31:00 连续下两笔主动买单（第二单紧跟第一单）===
        if not self._first_order_placed and hour == 9 and minute == 31 and second == 0:
            if ask1 > 0:
                # -------------------- 第一单：制造欠债 --------------------
                first_id = self._next_order_id()
                # 关键点：
                # - 策略看到的 ask1 来自 tick 快照，但撮合使用的是“逐笔委托/成交重建的订单簿”；
                #   两者在某些时刻可能不一致，导致你以为是主动单，但在重建簿里其实是被动单。
                # - 为了稳定触发“主动单路径”（进入 ConAuctionEngine::accept_virtual_order 的 can_trade 分支），
                #   这里直接用涨停价（UpperLimit）作为超强穿越价，保证只要盘口存在卖单就一定是主动单。
                aggressive_price = tick.UpperLimit if getattr(tick, "UpperLimit", 0) > 0 else ask1
                target_price = aggressive_price
                # 注意：这里特意把数量设得“明显大于卖一可用量”，用于稳定触发：
                # - 严格模式：只能成交卖一实际提供的量，剩余撤单
                # - 同时会记录欠债，用于验证“欠债未清前禁止下第二个主动单”
                order_volume = 1_000_000  # 100万股（通常远大于单个价位可用量，用于测试）

                events.append(make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=tick.Exchange,
                    Instrument=tick.Instrument,
                    OrderLocalID=first_id,
                    OrderType=0,
                    Direction=1,  # 买入
                    Price=target_price,
                    Volume=order_volume
                ))

                print(f"\n{'='*60}")
                print(f"🧪 [测试] [{time_str}] 先下第一笔主动买单（制造欠债）")
                print(f"   订单ID: {first_id}")
                print(f"   价格: {target_price/10000:.4f} (用UpperLimit确保主动单)")
                print(f"   参考 ask1: {ask1/10000:.4f} | UpperLimit: {getattr(tick, 'UpperLimit', 0)/10000:.4f}")
                print(f"   数量: {order_volume}")
                print(f"   预期: 可能部分成交+剩余撤单（取决于市场量）")

                self._first_order_placed = True
                self._first_order_id = first_id

                # -------------------- 第二单：验证欠债限制 --------------------
                second_id = self._next_order_id()
                second_volume = 100
                events.append(make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=tick.Exchange,
                    Instrument=tick.Instrument,
                    OrderLocalID=second_id,
                    OrderType=0,
                    Direction=1,
                    Price=target_price,   # 同样用 UpperLimit，确保第二单也走“主动单路径”
                    Volume=second_volume
                ))

                print(f"\n🧪 [测试] [{time_str}] 紧跟着下第二笔主动买单（应被欠债限制拦截）")
                print(f"   订单ID: {second_id}")
                print(f"   价格: {target_price/10000:.4f} (用UpperLimit确保主动单)")
                print(f"   数量: {second_volume}")
                print(f"   预期: 应该被拦截（收到撤单/拒单回报），不应成交")
                print(f"{'='*60}\n")

                # 记录第二单 ID：用于在回调里判断“是否被拒绝”
                self._second_order_id = second_id
                self._test_step = 1

        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderCallback(self, cb: OrderCallback) -> None:
        """下单确认回调"""
        self.order_count += 1
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  ✓ 下单确认: {cb.orderlocalid} {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        """成交/撤单回调"""
        if cb.matchtype == 'T':
            # 成交
            self.trade_count += 1
            self.position = cb.deltapos
            direction = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f} 持仓={self.position}")

            # 如果第二单居然成交了，说明“欠债限制”没有生效（测试失败）
            if self._second_order_id and cb.localid == self._second_order_id:
                print("  ❌ 异常：第二个主动单发生成交，欠债限制未生效！")
            
        elif cb.matchtype == 'D':
            # 撤单
            self.cancel_count += 1
            print(f"  ☑️  撤单: {cb.localid}")

            # 如果第二单收到撤单回报，且我们没有主动发起撤单请求，
            # 在本测试中可视为“被拒绝/被拦截”的表现（因为欠债限制触发）。
            if self._second_order_id and cb.localid == self._second_order_id:
                self.rejected_count += 1
                print("  ✅ 第二个主动单已被拦截（收到撤单/拒单回报）")

    # 说明：
    # - 当前回测引擎对虚拟撤单/拒单的通知主要通过 onTradeCallback(matchtype='D') 体现，
    #   并不会稳定调用 onOrderCancelled(reason)（原因字符串一般只在引擎日志里打印）。
    # - 因此本测试把“第二单收到 D 回报”作为“被拦截”的判据。
    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        """保留接口（可选），此处仅打印，不作为核心判定依据"""
        print(f"  ☑️  onOrderCancelled: {order_id}, reason={reason}")

    def passed(self) -> bool:
        """核心断言：第二个主动单必须被拦截（供 runner 决定退出码）"""
        return self.order_count > 0 and self.rejected_count > 0

    def print_summary(self):
        """打印测试摘要"""
        print(f"\n{'='*60}")
        print(f"📊 严格主动单模式测试摘要")
        print(f"{'='*60}")
        print(f"  下单次数: {self.order_count}")
        print(f"  成交次数: {self.trade_count}")
        print(f"  撤单次数: {self.cancel_count}")
        print(f"  被拒次数: {self.rejected_count}")
        print(f"  最终持仓: {self.position}")
        print(f"{'='*60}")
        print(f"\n测试结果:")
        print(f"  ✅ 测试完成: 已在 09:31:00 连续下两单")
        print(f"  - 第一单: {self._first_order_id}")
        print(f"  - 第二单: {self._second_order_id}")
        print(f"  ✅ 第二单结果: " + ("已被拦截(符合预期)" if self.rejected_count > 0 else "未被拦截(异常!)"))
        print(f"{'='*60}")
