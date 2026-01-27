"""
旺财回测平台 - ETF 混合回测测试

测试 ETF（三位小数，tick=10厘）+ 股票（两位小数，tick=100厘）混合回测

使用方法:
    python run_etf_test.py
"""

import pandas as pd
from pathlib import Path
import wangcai_syn
from wangcai_syn import Strategy


# ========== 配置区 ==========
ETF_SYMBOL = "510050.SH"      # 上证50ETF（三位小数）
STOCK_SYMBOL = "300827.SZ"    # 股票（两位小数）
DATE = "2025-11-17"
DATA_DIR = "../new_log"
# ============================


class ETFTestStrategy(Strategy):
    """ETF + 股票 混合测试策略"""
    
    def __init__(self, etf_symbols: set = None):
        super().__init__()
        self.tick_count = {}
        self.first_tick_logged = {}
        self.price_info = {}
        # ETF symbol 列表，用于判断价格精度
        self.etf_symbols = etf_symbols or set()
        
    def getStrategyId(self) -> str:
        return "ETFTestStrategy"
    
    def onTickEvent(self, tick):
        symbol = tick.Instrument
        
        # 统计 tick 数量
        self.tick_count[symbol] = self.tick_count.get(symbol, 0) + 1
        count = self.tick_count[symbol]
        
        # 输出前100个tick的详细价格 last_price不为0的
        if count <= 100 and tick.bids[0] > 0:
            is_etf = symbol in self.etf_symbols
            print(f"\n📊 [{symbol}] Tick #{count}:")
            print(f"   时间: {tick.datetime}")
            print(f"   bid1: {tick.bids[0]} 厘 ={tick.bids[0]/10000} 元")
            print(f"   类型: {'ETF' if is_etf else '股票'}")
        
        # 记录第一个有效价格用于汇总
        if symbol not in self.first_tick_logged:
            price_li = tick.last_price if tick.last_price > 0 else tick.bids[0]
            if price_li == 0:
                return []  # 还没有有效价格，等待下一个 tick
            self.first_tick_logged[symbol] = True
            
            is_etf = symbol in self.etf_symbols
            precision = "ETF (三位小数, tick=10厘)" if is_etf else "股票 (两位小数, tick=100厘)"
            
            self.price_info[symbol] = {
                'first_price_li': price_li,
                'first_price_yuan': price_li / 10000.0,
                'precision': precision,
                'datetime': tick.datetime,
            }
        return []  # 必须返回列表
    
    def onOrderEvent(self, order):
        return []  # 必须返回列表
    
    def onTradeEvent(self, trade):
        return []  # 必须返回列表
    
    def print_summary(self):
        print(f"\n{'='*60}")
        print(f"ETF + 股票 混合回测测试结果")
        print(f"{'='*60}")
        
        for symbol, count in self.tick_count.items():
            info = self.price_info.get(symbol, {})
            print(f"\n📈 {symbol}:")
            print(f"   Tick 数量: {count}")
            if info:
                print(f"   首个价格: {info['first_price_yuan']:.4f} 元")
                print(f"   价格精度: {info['precision']}")
        
        print(f"\n{'='*60}")


def load_data(symbol: str, date: str, data_dir: str):
    """加载回测数据"""
    path = Path(data_dir)
    
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    
    print(f"✅ 数据加载: {symbol}")
    print(f"   Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    
    return cstick, csord, cstra, csbar1d


def main():
    print("🚀 旺财回测平台 - ETF 混合回测测试")
    print(f"   ETF: {ETF_SYMBOL} @ {DATE}")
    print(f"   股票: {STOCK_SYMBOL} @ {DATE}")
    
    # 1. 加载数据
    print(f"\n📖 加载数据...")
    etf_data = load_data(ETF_SYMBOL, DATE, DATA_DIR)
    stock_data = load_data(STOCK_SYMBOL, DATE, DATA_DIR)
    
    # 2. 组织数据格式（5元组，包含 is_etf 标志）
    #    ETF: is_etf=True  -> tick=10厘（三位小数）
    #    股票: is_etf=False -> tick=100厘（两位小数）
    data = {
        ETF_SYMBOL: (*etf_data, True),      # ETF
        STOCK_SYMBOL: (*stock_data, False),  # 股票
    }
    
    # 3. 创建策略（传入 ETF symbol 集合）
    strategy = ETFTestStrategy(etf_symbols={ETF_SYMBOL})
    
    # 4. 运行回测
    print(f"\n🚀 开始混合回测...")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
    )
    
    # 5. 输出结果
    if success:
        print(f"\n🎉 回测完成!")
        strategy.print_summary()
    else:
        print(f"\n❌ 回测失败")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())
