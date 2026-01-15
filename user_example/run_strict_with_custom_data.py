#!/usr/bin/env python3
"""
综合示例：严格主动单模式 + 自定义数据推送

演示同时使用两个功能：
1. strict_active_order_mode=True: 主动单受欠债限制
2. enable_custom_data=True: 根据自定义信号下单
"""

import os
import sys
from pathlib import Path

import pandas as pd

# 确保使用本地编译的包
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

import wangcai_syn
from wangcai_syn import run_backtest, Strategy, make_order, make_cancel, UserEvent

# 打印加载路径，确认本地包
print(f"📦 wangcai_syn: {wangcai_syn.__file__}")
print(f"📦 扩展模块: {wangcai_syn.wangcai_cpp.__file__}")


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../new_log"
OUTPUT_DIR = "./output"
# ============================


class StrictCustomStrategy(Strategy):
    """
    综合策略示例：
    - 使用自定义数据中的 signal 字段判断是否下单
    - 在严格主动单模式下运行，测试欠债限制
    """
    
    def __init__(self, account: str = "strict_custom"):
        super().__init__()
        self.account = account         # 账户标识
        self.order_count = 0           # 订单计数
        self.position = 0              # 当前持仓
        self.custom_data_count = 0     # 收到的自定义数据次数
        self.signal_orders = []        # 信号触发的订单记录
    
    def getStrategyId(self) -> str:
        """返回策略ID（必须实现）"""
        return self.account
        
    def onCustomEvent(self, data: dict):
        """
        收到自定义数据回调
        根据 signal 字段决定是否下单（主动单：穿越买一/卖一价格）
        """
        self.custom_data_count += 1
        
        datetime_str = data.get('datetime', '')
        signal = data.get('signal', '')
        target_price = data.get('target_price', 0)
        
        print(f"\n📌 [自定义数据] {datetime_str}")
        print(f"   信号: {signal}, 目标价: {target_price}")
        
        # 只在 signal == 'buy' 时下单
        if signal == 'buy' and target_price > 0:
            self.order_count += 1
            order_id = f"{self.account}_{5000 + self.order_count}"
            
            # 转换价格为厘 (1元 = 10000厘)
            price_li = int(target_price * 10000)
            
            # 下主动买单（价格会穿越卖一，触发严格模式的欠债检查）
            order = make_order(
                Broker='',
                Account=self.account,
                Exchange=1,  # 深圳
                Instrument=SYMBOL,
                OrderLocalID=order_id,
                OrderType=0,  # 限价
                Direction=1,  # 买入
                Price=price_li,
                Volume=100
            )
            
            self.signal_orders.append({
                'datetime': datetime_str,
                'signal': signal,
                'order_id': order_id,
                'price': target_price,
                'volume': 100
            })
            
            print(f"   ✅ 下主动买单: {order_id} 100@{target_price:.4f}")
            return [order]
        else:
            print(f"   ⏭️ 信号为 '{signal}'，不下单")
            return []
    
    def onOrderEvent(self, order):
        """市场原始订单事件（csord）- 不处理"""
        return []
    
    def onTradeEvent(self, trade):
        """市场原始成交事件（cstra）- 不处理"""
        return []
    
    def onTickEvent(self, snapshot):
        """Tick回调 - 不处理"""
        return []
    
    def onOrderCallback(self, cb):
        """用户订单回报回调"""
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  ✓ 下单确认: {cb.orderlocalid} {direction} {cb.volume}@{cb.price/10000:.4f}")
    
    def onTradeCallback(self, cb):
        """用户成交回报回调"""
        if cb.matchtype == 'T':  # T=成交
            if cb.direction == 'B':
                self.position += cb.volume
            else:
                self.position -= cb.volume
            direction = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f} 持仓={self.position}")
        elif cb.matchtype == 'D':  # D=撤单
            print(f"  ⚠️ 撤单: {cb.localid}")
    
    def print_summary(self):
        """打印测试结果"""
        print(f"\n{'='*70}")
        print(f"📊 综合测试结果（严格主动单 + 自定义数据）")
        print(f"{'='*70}")
        print(f"  收到自定义数据次数: {self.custom_data_count}")
        print(f"  信号触发下单次数: {len(self.signal_orders)}")
        print(f"  最终持仓: {self.position}")
        
        if self.signal_orders:
            print(f"\n  信号订单记录:")
            for i, rec in enumerate(self.signal_orders, 1):
                print(f"    [{i}] {rec['datetime']} {rec['order_id']} {rec['volume']}@{rec['price']:.4f} (signal={rec['signal']})")
        
        print(f"{'='*70}")


def load_data(symbol: str, date: str, data_dir: str):
    """加载回测所需的4个数据文件"""
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"✅ 数据加载完成: {symbol}")
    print(f"   Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def main():
    print(f"\n{'='*70}")
    print("🧪 综合测试：严格主动单模式 + 自定义数据推送")
    print(f"{'='*70}")
    print(f"  标的: {SYMBOL}")
    print(f"  日期: {DATE}")
    print(f"{'='*70}")

    # 1. 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }

    # 2. 构造自定义数据
    # 模拟一个信号序列：连续发出多个买入信号
    # 在严格主动单模式下，第一个主动单成交后会产生"欠债"
    # 后续的主动单会被拒绝，直到欠债清除
    custom_df = pd.DataFrame({
        "datetime": [
            "2025-11-17 09:31:00",  # 第1个买入信号
            "2025-11-17 09:31:05",  # 第2个买入信号（可能被欠债限制）
            "2025-11-17 09:32:00",  # 第3个买入信号
            "2025-11-17 09:35:00",  # 第4个买入信号（欠债可能已清除）
            "2025-11-17 10:00:00",  # 持有信号（不下单）
        ],
        "signal": ["buy", "buy", "buy", "buy", "hold"],
        "target_price": [44.50, 44.60, 44.55, 44.45, 0.0],
    })
    
    print(f"\n📋 自定义数据:")
    print(custom_df)

    # 3. 创建策略
    strategy = StrictCustomStrategy(account="strict_custom_test")

    # 4. 运行回测（同时启用两个功能）
    print(f"\n🚀 开始回测...")
    print(f"   ⚙️ 严格主动单模式: ✅ 开启")
    print(f"   ⚙️ 自定义数据推送: ✅ 开启")
    
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_strict_custom",
        strict_active_order_mode=True,   # 启用严格主动单模式
        custom_data=custom_df,           # 自定义数据
        enable_custom_data=True          # 启用自定义数据推送
    )
    
    if success:
        print("\n✅ 回测完成!")
        strategy.print_summary()
        
        print(f"\n💡 说明:")
        print(f"   - 在严格主动单模式下，主动单吃掉历史订单后会产生'欠债'")
        print(f"   - 在欠债未清除前，新的主动单会被拒绝")
        print(f"   - 观察订单状态可以验证欠债限制是否生效")
    else:
        print("\n❌ 回测失败")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())
