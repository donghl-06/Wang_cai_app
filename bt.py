import sys
import pathlib
from typing import List, Dict, Optional, Tuple
from dataclasses import dataclass

sys.path.insert(0, str(pathlib.Path(__file__).parent / "wangcai_bt" / "src"))

from wangcai_bt import (
    BacktestEngine, Strategy,
    Direction, OrderType,
    Event, UserEvent, Execution, Snapshot,
    make_order_event, make_cancel_event,
)

START_TIME = "09:15:00"
START_TIME1 = "09:20:00"
START_TIME2 = "09:25:00"
AM_BEGIN = "09:30:00"
AM_END = "11:30:00"
PM_BEGIN = "13:00:00"
PM_CLOSINGPOS_BEGIN = "14:30:00"
PM_END1 = "14:57:00"
PM_END = "15:00:00"

SH_SIDE_BUY = 'B'
SH_SIDE_SELL = 'S'
SZ_SIDE_BUY = '1'
SZ_SIDE_SELL = '2'


@dataclass
class TDSug:
    """交易建议数据结构"""
    wait_event: int = 0
    instrument: str = ""
    order_localid: str = "0"
    order_type: int = 0
    exchange: int = 0
    price: int = 0
    volume: int = 0
    direction: int = 0


class PlaceOrdSigGen:
    """信号生成器 - 完整移植原逻辑"""
    def __init__(self, max_window: int = 8):
        self.max_window = max_window
        self.data_cache: List[Tuple[int, bool]] = []
        self.is_price_up: bool = True
        self.sig_count: int = 0
        
    def add_data(self, snap_shot: dict) -> Tuple[int, bool]:
        """
        添加数据并生成信号
        返回: (信号强度, 是否看涨)
        """
        # 获取买卖价
        ask_price = snap_shot.get('AskPrice', [0])[0] if 'AskPrice' in snap_shot else 0
        bid_price = snap_shot.get('BidPrice', [0])[0] if 'BidPrice' in snap_shot else 0
        
        # 如果没有买卖价数组，尝试从其他字段获取
        if ask_price == 0 and 'asks' in snap_shot:
            ask_price = snap_shot['asks'][0] if snap_shot['asks'] else 0
        if bid_price == 0 and 'bids' in snap_shot:
            bid_price = snap_shot['bids'][0] if snap_shot['bids'] else 0
            
        mid_price = self._get_midprice(ask_price, bid_price)
        
        if len(self.data_cache) == 0:
            self.data_cache.append((mid_price, False))
        else:
            pre_mid_price, _ = self.data_cache[-1]
            if mid_price > pre_mid_price:
                if self.is_price_up:
                    self.data_cache.append((mid_price, True))
                    self.sig_count += 1
                else:
                    self.data_cache.clear()
                    self.data_cache.append((mid_price, True))
                    self.sig_count = 1
                self.is_price_up = True
            elif mid_price < pre_mid_price:
                if not self.is_price_up:
                    self.data_cache.append((mid_price, True))
                    self.sig_count += 1
                else:
                    self.data_cache.clear()
                    self.data_cache.append((mid_price, True))
                    self.sig_count = 1
                self.is_price_up = False
            else:
                self.data_cache.append((mid_price, False))
                
        # 窗口管理
        if len(self.data_cache) > self.max_window:
            mid_price_0, is_sig_0 = self.data_cache[0]
            mid_price_1, _ = self.data_cache[1]
            if is_sig_0:
                if mid_price_0 != mid_price_1:
                    self.sig_count -= 1
                else:
                    self.data_cache[1] = (mid_price_1, is_sig_0)
            self.data_cache.pop(0)
            
        return self.sig_count, self.is_price_up
    
    def _get_midprice(self, ask_price: int, bid_price: int) -> int:
        """计算中间价"""
        if ask_price == 0:
            return bid_price
        elif bid_price == 0:
            return ask_price
        else:
            return int((ask_price + bid_price) / 2)


class BondMomentumStrategy(Strategy):
    """债券动量策略"""
    
    def __init__(self, strategy_id: str = "BondMomentum", 
                 max_window: int = 8,
                 ordbaseunit_volume: int = 100,
                 ordmaxunit_volume: int = 500):
        super().__init__()
        self._id = strategy_id
        self.local_ord_id = -1

        self.price_tick = 100
        # 策略参数
        self.max_window = max_window
        self.ordbaseunit_volume = ordbaseunit_volume
        self.ordmaxunit_volume = ordmaxunit_volume
        
        # 信号生成器
        self.sig_gen = PlaceOrdSigGen(self.max_window)
        
        # 持仓跟踪（相当于原策略的q_value_proxy）
        self.delta_pos = 0
        
        # 买卖方向标识
        self.side_buy = None
        self.side_sell = None
        self.instrument = None
        
        # 日志记录
        self.traded_orders = []
        self.placed_orders = []
        
        # 快照缓存（用于onTickEvent）
        self.last_snapshot = None
        
        print(f"\n{'='*60}")
        print(f"策略初始化: {self._id}")
        print(f"  信号窗口: {self.max_window}")
        print(f"  基础下单量: {self.ordbaseunit_volume}")
        print(f"  最大单笔量: {self.ordmaxunit_volume}")
        print(f"{'='*60}\n")
    def _align_price(self, price: int) -> int:
        """
        将价格对齐到最小变动单位（tick）
        确保价格是100的整数倍
        """
        if price <= 0:
            return 100  # 最小有效价格
        
        # 四舍五入到最近的tick
        aligned = round(price / self.price_tick) * self.price_tick
        
        # 确保至少是1个tick
        if aligned <= 0:
            aligned = self.price_tick
            
        return int(aligned)
    
    def _ensure_valid_price(self, price: float) -> int:
        """
        确保价格有效且对齐
        输入可能是float，输出必须是int且对齐到tick
        """
        # 如果是浮点数，先转换为厘
        if isinstance(price, float):
            price = int(price * 10000)  # 元转厘
        
        # 对齐到tick
        return self._align_price(price)
    
    def getStrategyId(self) -> str:
        return self._id
    
    def _init_instrument(self, symbol: str):
        """初始化合约相关信息"""
        if self.instrument is None:
            self.instrument = symbol
            # 判断交易所（SH/SZ）
            if symbol.endswith('SH'):
                self.side_buy = SH_SIDE_BUY
                self.side_sell = SH_SIDE_SELL
            else:
                self.side_buy = SZ_SIDE_BUY
                self.side_sell = SZ_SIDE_SELL
            print(f"[初始化] 合约: {symbol}, 交易所: {'上海' if symbol.endswith('SH') else '深圳'}")
    
    def _get_next_order_id(self) -> str:
        """生成下一个订单ID"""
        order_id = str(self.local_ord_id)
        self.local_ord_id -= 1
        return order_id
    
    def _get_time_int(self, datetime_str: str) -> int:
        """将时间字符串转换为整数格式（原策略使用）"""
        # "2024-12-19 09:30:00.000" -> 93000000
        if len(datetime_str) >= 19:
            time_str = datetime_str[11:19].replace(':', '')
            return int(time_str + '000')
        return 0
    
    def _get_time_str(self, datetime_str: str) -> str:
        """从datetime字符串中提取时间部分"""
        if len(datetime_str) >= 19:
            return datetime_str[11:19]
        return "00:00:00"
    
    def onOrderEvent(self, event: Event) -> List[UserEvent]:
        """处理订单事件 - 原策略没有此接口，我们在这里处理普通订单流"""
        # 原策略主要在snapshot中处理，这里暂时返回空
        return []
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        """处理行情快照 - 这是原策略的主要逻辑入口"""
        actions = []
        
        try:
            # 初始化合约信息
            if self.instrument is None and hasattr(snapshot, 'Instrument'):
                self._init_instrument(snapshot.Instrument)
            
            # 构造原策略需要的数据格式
            next_task = self._convert_snapshot_to_task(snapshot)
            if next_task is None:
                return actions
            
            time_str = self._get_time_str(snapshot.datetime)
            
            # 判断是否进入收盘平仓时段
            if time_str >= PM_CLOSINGPOS_BEGIN:
                # 收盘平仓逻辑
                actions.extend(self._handle_closing_position(next_task, snapshot))
            else:
                # 正常交易逻辑
                sig_count, is_price_up = self.sig_gen.add_data(next_task)
                
                # 确保价格有效且对齐
                ask_price = self._align_price(next_task['AskPrice'][0])
                bid_price = self._align_price(next_task['BidPrice'][0])
                spread = ask_price - bid_price
                
                # 调试信息
                if sig_count > 0:
                    print(f"[信号] sig_count: {sig_count}, is_price_up: {is_price_up}, "
                          f"spread: {spread/10000:.2f}元")
                
                # 信号触发条件
                if sig_count > 2 and spread < 30 * self.price_tick:  # 30分钱
                    order_id = self._get_next_order_id()
                    
                    if is_price_up:
                        # 看涨，买入（使用卖一价）
                        price = ask_price
                        volume = self.ordbaseunit_volume
                        direction = Direction.Buy
                    else:
                        # 看跌，卖出（使用买一价）
                        price = bid_price
                        volume = self.ordbaseunit_volume
                        direction = Direction.Sell
                    
                    # 再次确保价格对齐
                    price = self._align_price(price)
                    
                    if price > 0:
                        actions.append(make_order_event(
                            order_id=order_id,
                            symbol=snapshot.Instrument,
                            direction=direction,
                            order_type=OrderType.Limit,
                            price=price,
                            volume=volume,
                            strategy_id=self._id
                        ))
                        
                        print(f"[下单] {order_id}: {'买入' if direction == Direction.Buy else '卖出'} "
                              f"{volume}股 @ {price/10000:.4f}元 (对齐价格: {price}厘)")

                        
                        # 记录下单信息
                        self.placed_orders.append({
                            'time': snapshot.datetime,
                            'order_id': order_id,
                            'exchange': 0 if snapshot.Instrument.endswith('SH') else 1,
                            'ask1': next_task['AskPrice'][0],
                            'bid1': next_task['BidPrice'][0],
                            'delta_pos': self.delta_pos,
                            'price': price,
                            'volume': volume,
                            'direction': 'Buy' if direction == Direction.Buy else 'Sell'
                        })
                        
        except Exception as e:
            print(f"[ERROR] onTickEvent异常: {e}")
            import traceback
            traceback.print_exc()
            
        return actions
    
    def _convert_snapshot_to_task(self, snapshot: Snapshot) -> Optional[dict]:
        """将Snapshot转换为原策略使用的数据格式 - 确保价格对齐"""
        try:
            # 获取买卖价
            asks = []
            bids = []
            
            if hasattr(snapshot, 'asks') and snapshot.asks:
                # 确保所有价格都对齐
                asks = [self._align_price(p) for p in snapshot.asks]
            else:
                asks = [0] * 10
                
            if hasattr(snapshot, 'bids') and snapshot.bids:
                # 确保所有价格都对齐
                bids = [self._align_price(p) for p in snapshot.bids]
            else:
                bids = [0] * 10
            
            # 如果没有有效的买卖价，使用最新价
            if asks[0] == 0 and hasattr(snapshot, 'last_price'):
                last_price = self._align_price(snapshot.last_price)
                asks[0] = last_price + self.price_tick  # 加1个tick
                
            if bids[0] == 0 and hasattr(snapshot, 'last_price'):
                last_price = self._align_price(snapshot.last_price)
                bids[0] = max(last_price - self.price_tick, self.price_tick)  # 减1个tick，但不能为负
            
            task = {
                'Time': self._get_time_int(snapshot.datetime),
                'Instrument': snapshot.Instrument,
                'Exchange': 0 if snapshot.Instrument.endswith('SH') else 1,
                'AskPrice': asks,
                'BidPrice': bids,
            }
            
            return task
            
        except Exception as e:
            print(f"[ERROR] 转换snapshot失败: {e}")
            return None
    
    def _handle_closing_position(self, next_task: dict, snapshot: Snapshot) -> List[UserEvent]:
        """处理收盘平仓 - 确保价格对齐"""
        actions = []
        
        if self.delta_pos > 0:
            # 多头平仓
            need_close_volume = self.delta_pos
            while need_close_volume > 0:
                if need_close_volume > self.ordmaxunit_volume:
                    volume = self.ordmaxunit_volume
                    need_close_volume -= self.ordmaxunit_volume
                else:
                    volume = need_close_volume
                    need_close_volume = 0
                
                order_id = self._get_next_order_id()
                # 使用买一价卖出，确保对齐
                price = self._align_price(next_task['BidPrice'][0])
                
                if price > 0:
                    actions.append(make_order_event(
                        order_id=order_id,
                        symbol=snapshot.Instrument,
                        direction=Direction.Sell,
                        order_type=OrderType.Limit,
                        price=price,
                        volume=volume,
                        strategy_id=self._id
                    ))
                    
                    print(f"[收盘平仓] 卖出 {volume}股 @ {price/10000:.4f}元 (对齐价格: {price}厘)")
                    
                    # 记录下单
                    self.placed_orders.append({
                        'time': snapshot.datetime,
                        'order_id': order_id,
                        'exchange': next_task['Exchange'],
                        'ask1': next_task['AskPrice'][0],
                        'bid1': next_task['BidPrice'][0],
                        'delta_pos': self.delta_pos,
                        'price': price,
                        'volume': volume,
                        'direction': 'Sell'
                    })
                    
                if need_close_volume == 0:
                    break
                    
        elif self.delta_pos < 0:
            # 空头平仓
            need_close_volume = -self.delta_pos
            while need_close_volume > 0:
                if need_close_volume > self.ordmaxunit_volume:
                    volume = self.ordmaxunit_volume
                    need_close_volume -= self.ordmaxunit_volume
                else:
                    volume = need_close_volume
                    need_close_volume = 0
                
                order_id = self._get_next_order_id()
                # 使用卖一价买入，确保对齐
                price = self._align_price(next_task['AskPrice'][0])
                
                if price > 0:
                    actions.append(make_order_event(
                        order_id=order_id,
                        symbol=snapshot.Instrument,
                        direction=Direction.Buy,
                        order_type=OrderType.Limit,
                        price=price,
                        volume=volume,
                        strategy_id=self._id
                    ))
                    
                    print(f"[收盘平仓] 买入 {volume}股 @ {price/10000:.4f}元 (对齐价格: {price}厘)")
                    
                    # 记录下单
                    self.placed_orders.append({
                        'time': snapshot.datetime,
                        'order_id': order_id,
                        'exchange': next_task['Exchange'],
                        'ask1': next_task['AskPrice'][0],
                        'bid1': next_task['BidPrice'][0],
                        'delta_pos': self.delta_pos,
                        'price': price,
                        'volume': volume,
                        'direction': 'Buy'
                    })
                    
                if need_close_volume <= 0:
                    break
                    
        return actions
    
    def onTradeEvent(self, execution: Execution, datetime: str) -> List[UserEvent]:
        """处理成交事件"""
        # 原策略在__process_selftramsg_process中处理
        return []
    
    def onOrderFilled(self, order_id: str, price: int, volume: int) -> None:
        """订单成交通知 - 对应原策略的__process_selftramsg_process"""
        # 更新delta_pos
        for order in self.placed_orders:
            if order['order_id'] == order_id:
                if order['direction'] == 'Buy':
                    self.delta_pos += volume
                else:
                    self.delta_pos -= volume
                break
        
        print(f"[成交] 订单{order_id}: {volume}股 @ {price/10000:.3f}元, delta_pos: {self.delta_pos}")
        
        # 记录成交
        self.traded_orders.append({
            'localid': order_id,
            'direction': 'B' if self.delta_pos > 0 else 'S',
            'volume': volume,
            'price': price,
            'matchamount': price * volume / 10000,
            'deltapos': self.delta_pos,
            'matchtime': '',
            'matchtype': 'T'
        })
    
    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        """订单撤销通知"""
        print(f"[撤单] 订单{order_id}: {reason}")
    
    def onTradeCallback(self, callback) -> None:
        """统一交易回调"""
        pass
    
    def onOrderCallback(self, callback) -> None:
        """下单回调"""
        pass
    
    def print_summary(self):
        """打印策略统计"""
        print(f"\n{'='*60}")
        print(f"策略统计: {self._id}")
        print(f"{'='*60}")
        print(f"最终delta_pos: {self.delta_pos}")
        print(f"总下单数: {len(self.placed_orders)}")
        print(f"总成交数: {len(self.traded_orders)}")
        
        # 输出下单记录
        if self.placed_orders:
            print("\n下单记录（前10条）:")
            print("time,exchange,ask1,bid1,deltapos,price,volume,direction,ordlocalid")
            for order in self.placed_orders[:10]:
                print(f"{order['time']},{order['exchange']},{order['ask1']},"
                      f"{order['bid1']},{order['delta_pos']},{order['price']},"
                      f"{order['volume']},{order['direction']},{order['order_id']}")
        
        # 输出成交记录
        if self.traded_orders:
            print("\n成交记录（前10条）:")
            print("localid,direction,volume,price,matchamount,deltapos,matchtime,matchtype")
            for trade in self.traded_orders[:10]:
                print(f"{trade['localid']},{trade['direction']},{trade['volume']},"
                      f"{trade['price']},{trade['matchamount']},{trade['deltapos']},"
                      f"{trade['matchtime']},{trade['matchtype']}")
        
        print(f"{'='*60}\n")


def main():
    """主函数"""
    SYMBOL = "000063.SZ"
    DATE = "2024-12-19"
    DATA_PATH = "../logs"
    
    print(f"运行债券动量策略")
    print(f"合约: {SYMBOL}")
    print(f"日期: {DATE}")
    print(f"数据路径: {DATA_PATH}\n")
    
    try:
        # 创建回测引擎
        engine = BacktestEngine(symbol=SYMBOL, date=DATE, data_path=DATA_PATH)
        
        # 创建策略（可以调整参数）
        strategy = BondMomentumStrategy(
            strategy_id="BondMomentum",
            max_window=8,              # 信号窗口大小
            ordbaseunit_volume=100,     # 基础下单量
            ordmaxunit_volume=500       # 最大单笔下单量
        )
        
        # 注册策略
        engine.registerStrategy(strategy)
        
        # 运行回测
        print("开始运行回测...\n")
        engine.run()
        
        # 获取结果
        positions = engine.getPositions()
        total_pnl = engine.getTotalPnL()
        
        # 打印结果
        print(f"\n{'='*60}")
        print("回测结果")
        print(f"{'='*60}")
        
        if positions:
            for key, pos in positions.items():
                print(f"持仓 {key}:")
                print(f"  数量: {pos.quantity} 股")
                print(f"  平均成本: {pos.avg_cost:.4f} 元")
                print(f"  已实现盈亏: {pos.realized_pnl:.2f} 元")
        
        print(f"\n总盈亏: {total_pnl:.2f} 元")
        print(f"{'='*60}\n")
        
        # 打印策略统计
        strategy.print_summary()
        
    except Exception as e:
        print(f"回测失败: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())