"""
ETF Trader 被动单回测脚本

基于 bt_passive.py 的下单方法，实现 ETF Trader 的被动单回测。

主要特点：
1. 读取 ETF Trader 母单数据（5分钟的订单）
2. 将每个母单拆分成 5 个 1 分钟的子订单
3. 使用 bt_passive.py 的被动单策略进行回测
4. 每个子订单等待 1 分钟，如果不成交则用市价单补单
5. 支持按天运行，日志按天保存
6. 支持多种被动单策略选择

支持的策略：
- passive_own: 本方最优价（默认）
- passive_opponent: 对手最优价
- passive_mid_own: 中间价挂单(如果有，没有的话用本方最优)
- passive_mid_opponent: 中间价挂单(如果有，没有的话用对手最优)
- passive_own_second: 本方第二档挂单
- passive_own_third: 本方第三档挂单

使用方法：
    # 运行指定日期的回测（使用默认策略 passive_own）
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --trade_type all
    
    # 使用对手最优价策略
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --strategy passive_opponent
    
    # 使用中间价策略（回退本方）
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --strategy passive_mid_own
    
    # 运行所有日期的回测
    python run_etf_trader_wangcai_backtest.py --trade_type all
    
    # 只回测买入股票
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --trade_type buy_stock
    
    # 只回测卖出ETF
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --trade_type sell_etf
    
    # 指定日期和股票代码
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --sym 000001.SZ
    
    # 跳过已有结果的日期
    python run_etf_trader_wangcai_backtest.py --skip_existing
    
    # 保存详细日志文件（默认不保存）
    python run_etf_trader_wangcai_backtest.py --date 2025-12-15 --save_detailed_logs
"""

import os
import sys
import argparse
import pandas as pd
import adata
from datetime import datetime, timedelta
from typing import List, Dict

# 从 wangcai_syn 包导入
from wangcai_syn import run_backtest, Strategy
import wangcai_syn
print(f"wangcai syn version: {wangcai_syn.__version__}")
# 添加 wangcai 目录到路径，以便导入 bt_passive
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'wangcai'))
from bt_passive import PassiveOrderStrategy


def parse_args():
    """解析命令行参数"""
    parser = argparse.ArgumentParser(description='ETF Trader Passive Backtest Runner')
    parser.add_argument('--output_dir', type=str, default='output_etftrader_passive',
                        help='输出目录')
    parser.add_argument('--data_file', type=str, 
                        default='data/etf_trader_detail-20251116-20251216.csv',
                        help='交易明细数据文件路径')
    parser.add_argument('--date', type=str, default=None,
                        help='指定回测日期，格式为 YYYY-MM-DD 或 YYYYMMDD。如果不指定，则运行所有日期的回测')
    parser.add_argument('--sym', type=str, default=None,
                        help='指定股票代码（例如 000001.SZ）。如果不指定，则处理所有股票')
    parser.add_argument('--wait_time', type=int, default=60000,
                        help='被动单等待时间（毫秒），默认 60000 = 1分钟')
    parser.add_argument('--trade_type', type=str, default='all',
                        choices=['all', 'buy_stock', 'sell_etf'],
                        help='交易类型: all=全部, buy_stock=只买入成分股, sell_etf=只卖出ETF')
    parser.add_argument('--strategy', type=str, default='passive_own',
                        choices=['passive_own', 'passive_opponent', 'passive_mid_own', 
                                'passive_mid_opponent', 'passive_own_second', 'passive_own_third'],
                        help='被动单策略: passive_own=本方最优价, passive_opponent=对手最优价, '
                             'passive_mid_own=中间价(回退本方), passive_mid_opponent=中间价(回退对手), '
                             'passive_own_second=本方第二档, passive_own_third=本方第三档')
    parser.add_argument('--save_detailed_logs', action='store_true',
                        help='保存详细的日志文件（order_callbacks.csv, trade_callbacks.csv, cancel_callbacks.csv, position_updates.csv, tick_events.csv）')
    parser.add_argument('--skip_existing', action='store_true',
                        help='跳过已有结果文件的实验')
    return parser.parse_args()


def load_trader_data(data_file):
    """
    加载交易明细数据
    
    Args:
        data_file: 交易明细CSV文件路径
        
    Returns:
        DataFrame: 交易明细数据
    """
    df = pd.read_csv(data_file)
    # 转换tradingday为日期格式
    df['tradingday'] = pd.to_datetime(df['tradingday'], format='%Y%m%d').dt.strftime('%Y-%m-%d')
    return df


def get_trading_dates(trader_df):
    """
    获取所有交易日期
    
    Args:
        trader_df: 交易明细DataFrame
        
    Returns:
        list: 交易日期列表
    """
    return sorted(trader_df['tradingday'].unique().tolist())


def split_parent_order_to_plan(parent_order, num_orders=5, interval_minutes=1, wait_time_minutes=1):
    """
    将母单拆分成多个子订单计划
    
    Args:
        parent_order: 母单数据（dict），包含 instrument, direction, volume, start_time 等信息
        num_orders: 拆分的子订单数量，默认5个
        interval_minutes: 每个子订单的间隔时间（分钟），默认1分钟
        wait_time_minutes: 每个子订单的等待时间（分钟），默认1分钟
        
    Returns:
        list: 子订单计划列表
    """
    plans = []
    total_volume = parent_order['volume']
    volume_per_order = int(total_volume / num_orders)  # 向下取整
    remaining_volume = total_volume - volume_per_order * num_orders
    
    start_time = parent_order['start_time']
    
    for i in range(num_orders):
        # 最后一个订单加上剩余的量
        if i == num_orders - 1:
            volume = volume_per_order + remaining_volume
        else:
            volume = volume_per_order
        
        order_dt = start_time + timedelta(minutes=i * interval_minutes)
        end_dt = order_dt + timedelta(minutes=wait_time_minutes)
        
        plan = {
            'stock_code': parent_order['instrument'],
            'direction': parent_order['direction'],
            'filled_volume': volume,
            'filled_price': 0.0,  # 价格由策略动态决定
            'order_dt': order_dt,
            'end_dt': end_dt,
        }
        plans.append(plan)
    
    return plans


def create_plans_from_trader_data(trader_df, date_str, trade_type='all'):
    """
    从交易明细数据创建订单计划
    
    Args:
        trader_df: 交易明细DataFrame
        date_str: 日期字符串 'YYYY-MM-DD'
        trade_type: 交易类型 'all', 'buy_stock', 'sell_etf'
        
    Returns:
        DataFrame: 订单计划
    """
    # 筛选指定日期的数据
    day_df = trader_df[trader_df['tradingday'] == date_str].copy()
    
    # 根据 trade_type 筛选
    if trade_type == 'buy_stock':
        day_df = day_df[day_df['direction'] == 'BUY'].copy()
    elif trade_type == 'sell_etf':
        day_df = day_df[day_df['direction'] == 'SELL'].copy()
    
    all_plans = []
    
    for idx, row in day_df.iterrows():
        direction = row['direction']
        instrument = row['instrument']
        volume = row['volume']
        
        # 确定开始时间和拆单参数
        if direction == 'BUY':
            # 买入成分股：9:35-9:40，5个1分钟订单
            start_time = datetime.strptime(f"{date_str} 09:35:00", "%Y-%m-%d %H:%M:%S")
            num_orders = 5
            interval_minutes = 1
            wait_time_minutes = 1
        else:  # SELL
            # 卖出ETF：13:00-13:30，5个5分钟订单，但这里改成1分钟间隔
            start_time = datetime.strptime(f"{date_str} 13:00:00", "%Y-%m-%d %H:%M:%S")
            num_orders = 5
            interval_minutes = 1  # 改成1分钟间隔
            wait_time_minutes = 1
        
        # 构造母单
        parent_order = {
            'instrument': instrument,
            'direction': f"OrderSide.{direction}",
            'volume': volume,
            'start_time': start_time,
        }
        
        # 拆分成子订单计划
        plans = split_parent_order_to_plan(
            parent_order, 
            num_orders=num_orders, 
            interval_minutes=interval_minutes,
            wait_time_minutes=wait_time_minutes
        )
        
        all_plans.extend(plans)
    
    # 转换为DataFrame
    if len(all_plans) == 0:
        return pd.DataFrame()
    
    plan_df = pd.DataFrame(all_plans)
    plan_df = plan_df.sort_values('order_dt').reset_index(drop=True)
    
    return plan_df


def get_cstick_ad(date_str, symbol):
    """从adata获取tick数据"""
    cstick = adata.get_data('cstick', date_str, date_str, [symbol])
    return cstick


def get_csord_ad(date_str, symbol):
    """从adata获取逐笔委托数据"""
    csord = adata.get_data('csord', date_str, date_str, [symbol])
    return csord


def get_cstra_ad(date_str, symbol):
    """从adata获取逐笔成交数据"""
    cstra = adata.get_data('cstra', date_str, date_str, [symbol])
    return cstra

def get_csbar1d_ad(date_str, symbol):
    """从adata获取日线数据"""
    csbar1d = adata.get_data('csbar_1d', date_str, date_str, [symbol])
    return csbar1d

def run_backtest_for_date(date_str, trader_df, args):
    """
    运行单个日期的回测
    
    Args:
        date_str: 日期字符串 'YYYY-MM-DD'
        trader_df: 交易明细DataFrame
        args: 命令行参数
        
    Returns:
        bool: 是否成功处理
    """
    print(f"\n{'='*80}")
    print(f"Processing date: {date_str}")
    print(f"Strategy: {args.strategy}")
    print(f"{'='*80}")
    
    # 设置输出目录
    output_dir = os.path.join(args.output_dir, date_str)
    os.makedirs(output_dir, exist_ok=True)
    
    # 检查是否已经完成
    result_file = os.path.join(output_dir, f"{args.strategy}_log.csv")
    if args.skip_existing and os.path.exists(result_file):
        print(f"{date_str} already processed, skipping")
        return False
    
    # 创建订单计划
    print(f"\n📝 创建订单计划...")
    plan_df = create_plans_from_trader_data(trader_df, date_str, args.trade_type)
    
    if len(plan_df) == 0:
        print(f"No plans for {date_str}, skipping")
        return False
    
    # 如果指定了股票代码，筛选该股票的计划
    if args.sym:
        plan_df = plan_df[plan_df['stock_code'] == args.sym]
        if len(plan_df) == 0:
            print(f"No plans for {args.sym} on {date_str}, skipping")
            return False
        print(f"Filtered to {args.sym}: {len(plan_df)} plans")
    else:
        print(f"Created {len(plan_df)} plans")
    
    # 保存计划到临时文件
    temp_plan_file = os.path.join(output_dir, "plan_temp.csv")
    plan_df.to_csv(temp_plan_file, index=False)
    print(f"Plan saved to: {temp_plan_file}")
    
    # 获取所有需要的股票/ETF代码
    symbols = plan_df['stock_code'].unique().tolist()
    print(f"\n📊 Will process {len(symbols)} symbols")
    
    # 为每个股票/ETF创建独立的策略并运行回测
    print(f"\n🚀 Running backtest...")
    
    all_instruction_logs = []
    
    for symbol in symbols:
        print(f"\n{'='*60}")
        print(f"Processing {symbol}")
        print(f"{'='*60}")
        
        # 筛选该股票的计划
        symbol_plan_df = plan_df[plan_df['stock_code'] == symbol].copy()
        
        if symbol == '600105.SH' or symbol == "002929.SZ":
            print(f"symbol_plan_df for {symbol}: {len(symbol_plan_df)} rows")
        else:
            continue 
        
        if len(symbol_plan_df) == 0:
            if symbol == '600105.SH' or symbol == "002929.SZ":
                print(f"Skipping {symbol} due to empty plan")
            continue
        
        # 保存该股票的计划
        # symbol_plan_file = os.path.join(output_dir, f"plan_{symbol}.csv")
        # symbol_plan_df.to_csv(symbol_plan_file, index=False)
        
        if symbol == '600105.SH':
            print(f"Saved plan file for 600105.SH: (no longer saved)")
        
        # ========== 加载该股票的数据 ==========
        print(f"  Loading data for {symbol}...")
        cstick_df = get_cstick_ad(date_str, symbol)
        csord_df = get_csord_ad(date_str, symbol)
        cstra_df = get_cstra_ad(date_str, symbol)
        csbar = get_csbar1d_ad(date_str, symbol)
        
        if cstick_df is None or csord_df is None or cstra_df is None:
            print(f"  ⚠️ Skipping {symbol} due to missing data")
            if symbol == '600105.SH':
                print(f"  Data check for 600105.SH: cstick={cstick_df is not None}, csord={csord_df is not None}, cstra={cstra_df is not None}")
            continue
        
        print(f"  ✅ {symbol}: {len(cstick_df)} ticks, {len(csord_df)} orders, {len(cstra_df)} trades")
        
        # 创建策略
        strategy = PassiveOrderStrategy(
            account=f"ETFTRADER_{symbol}",
            symbol=symbol,
            date=date_str,
            plan_df=symbol_plan_df,
            strategy=args.strategy
        )
        
        # 准备该股票的数据
        symbol_data_dict = {symbol: (cstick_df, csord_df, cstra_df, csbar)}
        
        # 运行回测
        print(f"  Running backtest...")
        success = run_backtest(
            data_dict=symbol_data_dict,
            strategy=strategy,
            output_dir=output_dir
        )
        
        if success:
            print(f"  ✅ {symbol} backtest completed!")
            
            # 保存记录（根据参数决定是否保存详细日志）
            strategy.save_records(output_dir, args.save_detailed_logs)
            
            # 收集指令日志
            if hasattr(strategy, 'instruction_logs'):
                for log in strategy.instruction_logs.values():
                    all_instruction_logs.append(log)
        else:
            print(f"  ❌ {symbol} backtest failed")
        
        # ========== 清除数据以释放内存 ==========
        del cstick_df, csord_df, cstra_df, symbol_data_dict, strategy
        import gc
        gc.collect()
        print(f"  Memory cleaned")
    
    # 合并所有指令日志并保存
    if len(all_instruction_logs) > 0:
        combined_log_df = pd.DataFrame(all_instruction_logs)
        combined_log_df = combined_log_df.sort_values('order_time').reset_index(drop=True)
        combined_log_df.to_csv(result_file, index=False, encoding='utf-8')
        print(f"\n✅ Combined instruction log saved to: {result_file}")
        
        # 打印统计信息
        print(f"\n{'='*60}")
        print(f"Summary for {date_str}:")
        print(f"{'='*60}")
        print(f"Total orders: {len(combined_log_df)}")
        
        if 'execution_type' in combined_log_df.columns:
            passive_filled = combined_log_df['execution_type'] == 'PASSIVE'
            market_filled = combined_log_df['execution_type'] == 'MARKET'
            print(f"Passive filled: {passive_filled.sum()} ({passive_filled.sum()/len(combined_log_df)*100:.1f}%)")
            print(f"Market filled: {market_filled.sum()} ({market_filled.sum()/len(combined_log_df)*100:.1f}%)")
        
        if 'final_price' in combined_log_df.columns:
            avg_price = combined_log_df['final_price'].mean()
            print(f"Average fill price: {avg_price:.4f}")
        
        print(f"{'='*60}")
    
    return True


def run_all_dates(trader_df, args):
    """
    运行所有日期的回测
    
    Args:
        trader_df: 交易明细DataFrame
        args: 命令行参数
    """
    # 获取所有日期
    date_list = get_trading_dates(trader_df)
    print(f"\n{'='*80}")
    print(f"Found {len(date_list)} dates to process")
    print(f"Date range: {date_list[0]} to {date_list[-1]}")
    print(f"{'='*80}\n")
    
    # 遍历每个日期
    success_count = 0
    for date_str in date_list:
        try:
            if run_backtest_for_date(date_str, trader_df, args):
                success_count += 1
        except Exception as e:
            print(f"❌ Error processing {date_str}: {e}")
            import traceback
            traceback.print_exc()
            continue
    
    print(f"\n{'='*80}")
    print(f"Completed {success_count}/{len(date_list)} dates successfully")
    print(f"{'='*80}\n")


def main():
    """主函数"""
    args = parse_args()
    
    # 登录 adata
    username = os.getenv('MY_USER_NAME')
    password = os.getenv('MY_PASSWORD')
    if username and password:
        adata.login(username, password)
        print("✅ Logged in to adata")
    else:
        print("⚠️ MY_USER_NAME or MY_PASSWORD not set, adata login skipped")
    
    # 加载交易明细数据
    print(f"\n📖 Loading trader data from {args.data_file}...")
    trader_df = load_trader_data(args.data_file)
    print(f"Loaded {len(trader_df)} records")
    
    # 创建输出目录
    if not os.path.exists(args.output_dir):
        os.makedirs(args.output_dir)
    
    # 运行回测
    if args.date:
        # 标准化日期格式
        date_str = args.date
        if "-" not in date_str:
            date_str = f"{date_str[:4]}-{date_str[4:6]}-{date_str[6:]}"
        print(f"\nRunning backtest for specific date: {date_str}")
        run_backtest_for_date(date_str, trader_df, args)
    else:
        print("\nRunning backtest for all dates")
        run_all_dates(trader_df, args)
    
    print("\n🎉 All done!")


if __name__ == "__main__":
    main()
