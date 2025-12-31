"""
旺财回测平台 - 使用示例

安装:
    pip install wangcai_syn-0.1.1-cp312-cp312-linux_x86_64.whl

使用方法:
    1. 准备数据 (从数据库/本地文件/其他来源)
    2. 创建策略 (继承 Strategy 类)
    3. 调用 run_backtest() 运行回测
"""

import pandas as pd
import os
import adata
# 从 wangcai_syn 包导入
from wangcai_syn import run_backtest

# 导入自定义策略
from strategy_example import ExampleStrategy

def get_cstick_ad(date_str, sym):
    cstick = adata.get_data('cstick', date_str, date_str, [sym])
    cstick['datetime'] = cstick['date'] + pd.to_timedelta(cstick['time'])
    cstick.set_index('datetime', inplace=True, drop=False)
    aq_cols = ['datetime', 'sym', 'prevclose', 'open', 'high', 'low', 'close', 
            'volume', 'turnover', 'tradecount', 
            'bid1', 'bsize1', 'bid2', 'bsize2', 'bid3', 'bsize3', 'bid4', 'bsize4', 'bid5', 'bsize5', 
            'ask1', 'asize1', 'ask2', 'asize2', 'ask3', 'asize3', 'ask4', 'asize4', 'ask5', 'asize5',
            'avgbid', 'avgask', 'totalbsize', 'totalasize', 'iopv']

    return cstick[aq_cols]

def get_csord_ad(date_str, sym):
    csord = adata.get_data('csord', date_str, date_str, [sym])
    csord['datetime'] = csord['date'] + pd.to_timedelta(csord['time'])
    csord.set_index('datetime', inplace=True, drop=False)
    ord_cols = ["datetime",	"sym", "price", "size",	"side", "ordertype", "orderid", "channelno", "seqno", "bizindex"]
    return csord[ord_cols]

def get_cstra_ad(date_str, sym):
    cstra = adata.get_data('cstra', date_str, date_str, [sym])
    cstra['datetime'] = cstra['date'] + pd.to_timedelta(cstra['time'])
    cstra.set_index('datetime', inplace=True, drop=False)
    tra_cols = ["datetime", "sym", "price", "size", "bidorderid", "askorderid", 
                "tradeid", "exectype", "tradebsflag", "channelno", "bizindex"]
    return cstra[tra_cols]

def main():
    """
    示例主函数 - 演示如何使用回测平台
    
    用户需要自行准备数据（从数据库/本地文件/API等来源）
    """
    username = os.getenv('MY_USER_NAME') 
    password = os.getenv('MY_PASSWORD')
    adata.login(username, password)
    # ========== 1. 准备数据 ==========
    # 可以从任意来源加载：本地CSV、数据库、API等
    
    symbol = "000488.SZ"
    date = "2024-12-19"
    
    # 从本地CSV加载（示例），也可以从数据库获取
    print(f"📖 加载数据...")
    cstick_df = get_cstick_ad(date, symbol)
    order_df = get_csord_ad(date, symbol)
    trade_df = get_cstra_ad(date, symbol)
    
    print(f"   ✅ Tick数据: {len(cstick_df)} 行")
    print(f"   ✅ 委托数据: {len(order_df)} 行")
    print(f"   ✅ 成交数据: {len(trade_df)} 行")
    
    # 组织成字典格式: {symbol: (cstick_df, order_df, trade_df)}
    # 多合约回测：data = {symbol1: (...), symbol2: (...), ...}
    data = {
        symbol: (cstick_df, order_df, trade_df)
    }
    
    # ========== 2. 创建策略 ==========
    # 单策略模式
    strategy = ExampleStrategy(account="user1")
    
    # ========== 3. 运行回测 ==========
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"./output/{symbol}_{date}"
    )
    
    if success:
        print(f"\n🎉 回测成功!")
        return 0
    else:
        print(f"❌ 回测失败")
        return 1


if __name__ == "__main__":
    exit(main())
