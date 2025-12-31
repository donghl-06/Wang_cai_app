"""
运行接口验证测试
"""

import pandas as pd
from wangcai_syn import run_backtest
from strategy_test import TestStrategy


def main():
    # 加载数据
    symbol = "000488.SZ"
    data_path = "../test_bt/logs"
    date = "2024-12-19"
    
    print(f"📖 加载数据: {symbol}")
    cstick_df = pd.read_csv(f"{data_path}/cstick_{symbol}_{date}.csv")
    order_df = pd.read_csv(f"{data_path}/csord_{symbol}_{date}.csv")
    trade_df = pd.read_csv(f"{data_path}/cstra_{symbol}_{date}.csv")
    
    data = {symbol: (cstick_df, order_df, trade_df)}
    
    # 运行测试策略
    strategy = TestStrategy(account="test")
    run_backtest(data, strategy)


if __name__ == "__main__":
    main()

