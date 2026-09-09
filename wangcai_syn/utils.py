"""
旺财回测平台 - 工具函数模块
提供数据转换、上海股票订单还原、下单撤单等工具函数
"""

import numpy as np
import pandas as pd
from typing import List, Dict, Tuple
from multiprocessing import Pool

_engine_refs = []
_mp_data_dict = {}

from wangcai_syn.wangcai_cpp import (
    SymbolData, MultiBacktestEngine,
    UserEvent, UserOrder, UserCancel,
    Direction, OrderType
)


def _normalize_adata_scalar(value) -> str:
    """把 AData/object 列统一为不带 bytes 包装的标量字符串。"""
    if value is None or (not isinstance(value, (bytes, bytearray)) and pd.isna(value)):
        return ''
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).decode('utf-8').strip()
    if isinstance(value, (int, np.integer)):
        return str(int(value))
    if isinstance(value, (float, np.floating)) and float(value).is_integer():
        return str(int(value))

    text = str(value).strip()
    if (len(text) >= 3 and text[0].lower() == 'b' and
            text[1] in ("'", '"') and text[-1] == text[1]):
        return text[2:-1]
    if len(text) >= 2 and text[0] in ("'", '"') and text[-1] == text[0]:
        return text[1:-1]
    return text


def revert_sh_order(order_df: pd.DataFrame, trade_df: pd.DataFrame, symbol: str) -> pd.DataFrame:
    """
    上海股票订单还原函数（numpy 向量化版本，比 pandas groupby 快 100-200 倍）
    
    上交所的逐笔委托数据不完整，需要从逐笔成交数据中还原出主动方订单
    
    Args:
        order_df: 逐笔委托数据 DataFrame (csord)
        trade_df: 逐笔成交数据 DataFrame (cstra)
        symbol: 股票代码
    
    Returns:
        pd.DataFrame: 还原后的订单数据，格式与原始 order_df 一致
    """
    trade_df = trade_df.copy()
    order_df = order_df.copy()
    
    if 'exectype' not in trade_df.columns:
        raise ValueError("上海订单还原要求 cstra 包含 exectype 列")
    trade_df['exectype'] = trade_df['exectype'].map(_normalize_adata_scalar)
    time_text = trade_df['time'].astype(str)

    # 只用真实成交还原，覆盖连续竞价和收盘集合竞价 (09:30 - 15:00)。
    # D0诊断修复：收盘集合竞价是单一价格撮合、无主动方概念，成交双方通常已揭示在 csord；
    # 无差别还原会与原始委托聚合导致量虚增（600050 收盘段偏差根因）。
    # 因此 14:57 后的收盘成交只还原 csord 中缺失的一方（数据覆盖不全的标的，如 ETF）。
    time_text = time_text.reset_index(drop=True)
    trade_reset = trade_df.reset_index(drop=True)
    close_mask = (time_text >= '0 days 14:57:00').values
    orig_ids = set(order_df['orderid'].values.astype('int64')) if len(order_df) else set()
    bid_ids = pd.to_numeric(trade_reset['bidorderid'], errors='coerce').fillna(0).astype('int64')
    ask_ids = pd.to_numeric(trade_reset['askorderid'], errors='coerce').fillna(0).astype('int64')
    active_is_new = (bid_ids > ask_ids).values
    active_ids = pd.Series(np.where(active_is_new, bid_ids, ask_ids))
    close_skip = close_mask & active_ids.isin(orig_ids).values
    keep = (~close_mask) | (~close_skip)
    df_cstrad_filtered = trade_reset[
        (time_text.values >= '0 days 09:30:00') &
        (time_text.values <= '0 days 15:00:00.999999') &
        (trade_reset['exectype'] == '1') &
        keep
    ]
    
    # bidorderid > askorderid：买方编号较新，需还原主动买单 bidorderid。
    bids_df = df_cstrad_filtered[
        (df_cstrad_filtered['askorderid'] != 0) & 
        (df_cstrad_filtered['bidorderid'] > df_cstrad_filtered['askorderid'])
    ][['date', 'time', 'sym', 'price', 'size', 'bidorderid', 'bizindex', 'channelno']].copy()
    bids_df.rename(columns={'bidorderid': 'orderid'}, inplace=True)
    bids_df['side'] = 1
    bids_df['ordertype'] = 0
    
    # bidorderid < askorderid：卖方编号较新，需还原主动卖单 askorderid。
    asks_df = df_cstrad_filtered[
        (df_cstrad_filtered['bidorderid'] != 0) & 
        (df_cstrad_filtered['bidorderid'] < df_cstrad_filtered['askorderid'])
    ][['date', 'time', 'sym', 'price', 'size', 'askorderid', 'bizindex', 'channelno']].copy()
    asks_df.rename(columns={'askorderid': 'orderid'}, inplace=True)
    asks_df['side'] = -1
    asks_df['ordertype'] = 0
    
    # 合并：从成交还原的订单 + 原始委托
    cols = [
        'date', 'time', 'sym', 'price', 'size', 'side', 'ordertype',
        'orderid', 'bizindex', 'channelno',
    ]
    orig = order_df[cols].copy()
    merged = pd.concat([bids_df[cols], asks_df[cols], orig], ignore_index=True)
    if merged.empty:
        return order_df.copy()

    # ---- numpy 向量化 groupby ----
    time_vals = merged['time'].astype(str).values
    date_vals = merged['date'].astype(str).values
    symbol_vals = merged['sym'].astype(str).values
    oids = merged['orderid'].values.astype(np.int64)
    channels = merged['channelno'].values.astype(np.int64)

    # 完整 AData 市场订单键：(date, sym, channelno, orderid)。
    original_positions = np.arange(len(merged), dtype=np.int64)
    sort_idx = np.lexsort(
        (original_positions, oids, channels, symbol_vals, date_vals)
    )
    oids_sorted = oids[sort_idx]
    channels_sorted = channels[sort_idx]
    dates_sorted = date_vals[sort_idx]
    symbols_sorted = symbol_vals[sort_idx]

    # 找每组的起始位置
    change = np.empty(len(oids_sorted), dtype=bool)
    change[0] = True
    change[1:] = (
        (oids_sorted[1:] != oids_sorted[:-1]) |
        (channels_sorted[1:] != channels_sorted[:-1]) |
        (dates_sorted[1:] != dates_sorted[:-1]) |
        (symbols_sorted[1:] != symbols_sorted[:-1])
    )
    group_starts = np.nonzero(change)[0]
    group_ids = oids_sorted[group_starts]

    # 提取排序后的数值数组
    prices = merged['price'].values.astype(np.float64)[sort_idx]
    sizes = merged['size'].values.astype(np.float64)[sort_idx]
    sides = merged['side'].values.astype(np.int64)[sort_idx]
    ordertypes = merged['ordertype'].values.astype(np.int64)[sort_idx]
    bizindexes = merged['bizindex'].values.astype(np.int64)[sort_idx]
    channelnos = channels_sorted
    times_sorted = time_vals[sort_idx]

    # reduceat 聚合：每组的 sum / max / min / first
    size_sum = np.add.reduceat(sizes, group_starts)
    price_max = np.maximum.reduceat(prices, group_starts)
    price_min = np.minimum.reduceat(prices, group_starts)
    side_first = sides[group_starts]
    ordertype_first = ordertypes[group_starts]
    bizindex_first = bizindexes[group_starts]
    channelno_first = channelnos[group_starts]
    time_first = times_sorted[group_starts]
    date_first = dates_sorted[group_starts]
    symbol_first = symbols_sorted[group_starts]

    # 买单用最大价格，卖单用最小价格
    price_final = np.where(side_first == 1, price_max, price_min)

    result = pd.DataFrame({
        'date': date_first, 'time': time_first, 'sym': symbol_first,
        'price': price_final, 'size': size_sum, 'side': side_first,
        'ordertype': ordertype_first, 'orderid': group_ids,
        'channelno': channelno_first, 'seqno': bizindex_first,
        'bizindex': bizindex_first, 'updatetime': time_first,
    })
    result.sort_values(['date', 'sym', 'bizindex'], inplace=True)
    result.reset_index(drop=True, inplace=True)

    return result


def create_symbol_data(symbol: str, 
                       cstick_df: pd.DataFrame,
                       order_df: pd.DataFrame, 
                       trade_df: pd.DataFrame,
                       csbar1d_df: pd.DataFrame,
                       is_etf: bool = False) -> SymbolData:
    """
    从 DataFrame 创建 SymbolData 对象
    
    Args:
        symbol: 合约代码，如 "000488.SZ"
        cstick_df: Tick快照数据 DataFrame
        order_df: 逐笔委托数据 DataFrame
        trade_df: 逐笔成交数据 DataFrame
        csbar1d_df: 日线数据 DataFrame（包含涨跌停限制）
        is_etf: 是否为ETF（True=三位小数/tick=0.001元，False=两位小数/tick=0.01元）
    
    Returns:
        SymbolData: 回测引擎所需的数据对象
    """
    # 处理 bytes 对象和已经被字符串化的 "b'X'" 字段。
    trade_df = trade_df.copy()
    if 'exectype' in trade_df.columns:
        trade_df['exectype'] = trade_df['exectype'].map(_normalize_adata_scalar)
    if 'tradebsflag' in trade_df.columns:
        trade_df['tradebsflag'] = trade_df['tradebsflag'].map(_normalize_adata_scalar)

    order_df = order_df.copy()
    for column in ('side', 'ordertype'):
        if column in order_df.columns:
            order_df[column] = order_df[column].map(_normalize_adata_scalar)
    
    # 对于上海股票，进行订单还原
    if symbol.endswith('.SH'):
        order_df = revert_sh_order(order_df, trade_df, symbol)
        print(f"[{symbol}] 上海股票订单还原完成，还原后订单数: {len(order_df)}")
    
    # 转换为 CSV 字符串
    cstick_csv = cstick_df.to_csv(index=False)
    order_csv = order_df.to_csv(index=False)
    trade_csv = trade_df.to_csv(index=False)
    csbar1d_csv = csbar1d_df.to_csv(index=False)
    
    return SymbolData(symbol, cstick_csv, order_csv, trade_csv, csbar1d_csv, is_etf)


def _prepare_symbol_csvs(symbol):
    """多进程 worker：读取 _mp_data_dict 中的数据，完成 SH 还原 + CSV 转换"""
    data_tuple = _mp_data_dict[symbol]
    if len(data_tuple) == 5:
        cstick_df, order_df, trade_df, csbar1d_df, is_etf = data_tuple
    else:
        cstick_df, order_df, trade_df, csbar1d_df = data_tuple
        is_etf = False

    trade_df = trade_df.copy()
    if 'exectype' in trade_df.columns:
        trade_df['exectype'] = trade_df['exectype'].map(_normalize_adata_scalar)
    if 'tradebsflag' in trade_df.columns:
        trade_df['tradebsflag'] = trade_df['tradebsflag'].map(_normalize_adata_scalar)

    order_df = order_df.copy()
    for column in ('side', 'ordertype'):
        if column in order_df.columns:
            order_df[column] = order_df[column].map(_normalize_adata_scalar)

    if symbol.endswith('.SH'):
        order_df = revert_sh_order(order_df, trade_df, symbol)
        print(f"[{symbol}] 上海股票订单还原完成，还原后订单数: {len(order_df)}")

    cstick_csv = cstick_df.to_csv(index=False)
    order_csv = order_df.to_csv(index=False)
    trade_csv = trade_df.to_csv(index=False)
    csbar1d_csv = csbar1d_df.to_csv(index=False)

    return (symbol, cstick_csv, order_csv, trade_csv, csbar1d_csv, is_etf)


def create_multi_symbol_data(data_dict, n_workers=None, release_input: bool = False) -> List[SymbolData]:
    """
    从字典创建多个合约的 SymbolData
    
    Args:
        data_dict: 格式为以下两种之一：
            - 4元组（股票）: {symbol: (cstick_df, order_df, trade_df, csbar1d_df), ...}
            - 5元组（指定ETF）: {symbol: (cstick_df, order_df, trade_df, csbar1d_df, is_etf), ...}
        n_workers: 并行 worker 数，None 或 <=1 时串行处理
        release_input: 是否边转换边释放输入数据（默认关闭）
            开启后会**清空传入的 data_dict**，每个合约转换完成即从字典中移除，
            使 Python DataFrame 与 C++ SymbolData 不同时占用内存（几十个标的可省数 GB）。
            代价是调用方拿到的是一个空字典，无法复用同一份数据再跑一次。
            仅在数据用完即弃的大批量场景下开启。
    
    Returns:
        List[SymbolData]: SymbolData 对象列表
    """
    global _mp_data_dict

    if n_workers and n_workers > 1 and len(data_dict) > 1:
        _mp_data_dict = data_dict
        symbols = list(data_dict.keys())
        symbol_data_list = []
        with Pool(n_workers) as pool:
            for result in pool.imap_unordered(_prepare_symbol_csvs, symbols):
                sym, cstick_csv, order_csv, trade_csv, csbar1d_csv, is_etf = result
                sd = SymbolData(sym, cstick_csv, order_csv, trade_csv, csbar1d_csv, is_etf)
                symbol_data_list.append(sd)
                if release_input:
                    data_dict.pop(sym, None)
        _mp_data_dict = {}
        return symbol_data_list

    symbol_data_list = []
    for symbol in list(data_dict.keys()):
        data_tuple = data_dict[symbol]
        if len(data_tuple) == 5:
            cstick_df, order_df, trade_df, csbar1d_df, is_etf = data_tuple
        else:
            cstick_df, order_df, trade_df, csbar1d_df = data_tuple
            is_etf = False
        
        symbol_data = create_symbol_data(symbol, cstick_df, order_df, trade_df, csbar1d_df, is_etf)
        symbol_data_list.append(symbol_data)
        if release_input:
            # 转换完立刻丢弃，避免 DataFrame 与 SymbolData 同时驻留
            data_dict.pop(symbol, None)
            del data_tuple, cstick_df, order_df, trade_df, csbar1d_df
    return symbol_data_list


def run_backtest(data_dict, 
                 strategy, 
                 output_dir: str = None,
                 strict_active_order_mode: bool = False,
                 real_trade_match_mode: bool = False,
                 queue_info_enabled: bool = False,
                 custom_data: pd.DataFrame = None,
                 enable_custom_data: bool = False,
                 n_workers: int = None,
                 release_input: bool = False,
                 realtime_tick_interval_ms: int = 0,
                 event_snapshot_enabled: bool = False) -> bool:
    """
    运行回测（支持单合约/多合约，单策略，股票/ETF）
    
    Args:
        data_dict: 格式为以下两种之一：
            - 4元组（股票，两位小数）: {symbol: (cstick_df, order_df, trade_df, csbar1d_df), ...}
            - 5元组（指定ETF/股票）: {symbol: (cstick_df, order_df, trade_df, csbar1d_df, is_etf), ...}
              其中 is_etf=True 表示ETF（三位小数），is_etf=False 表示股票（两位小数）
        strategy: 策略实例（单个策略）
        output_dir: 输出目录，默认不保存
        strict_active_order_mode: 是否启用严格主动单模式（默认关闭）
            开启后，虚拟主动单只能成交市场实际提供的量，剩余部分撤单，
            且在欠债未还清前禁止下新的主动单
        real_trade_match_mode: 是否启用真实成交替代模式（默认关闭）
            开启后，主动单自动使用严格模式，被动单只能在同价位有真实成交时
            按真实成交量成交（排队位置决定是否轮到你，量用完就没有了）
            注意：此模式与 strict_active_order_mode 互斥，开启此模式会自动启用严格主动单
        queue_info_enabled: 是否启用下单回调队列信息（默认关闭）
            开启后，onOrderCallback 会额外返回：
            - queue_ahead_count: 前方历史订单数
            - queue_ahead_volume: 前方历史订单总量
            - prev_order_ids: 前方最近3个历史订单ID（真实orderid）
        custom_data: 用户自定义数据 DataFrame（默认None）
            必须包含 'datetime' 列，格式如 '2025-11-17 09:35:00'
            其他列可自定义，会作为 dict 传递给策略的 onCustomEvent
        enable_custom_data: 是否启用自定义数据推送功能（默认关闭）
            需设为 True 才会启用自定义数据推送
        release_input: 是否在转换过程中释放输入数据（默认关闭）
            开启后会**清空传入的 data_dict**，让 Python DataFrame 不与 C++ SymbolData
            同时占用内存（几十个标的可省数 GB）；代价是回测结束后 data_dict 为空，
            同一份数据无法再跑第二次。仅在数据用完即弃的大批量场景下开启。
        realtime_tick_interval_ms: 实时合成 Tick 推送间隔（毫秒，默认 0=关闭）
            需显式传入间隔（如 10/50/100）才开启。开启后，引擎每跨过一个间隔
            网格边界，在当前市场事件处理完成后从内部订单簿合成一个十档 Snapshot，
            触发策略的 onRealTimeTickEvent 回调。字段结构与真实 3 秒 Tick 一致：
            - 十档/最新价/涨跌停/委托总量来自订单簿实时状态
            - Volume/Turnover/NumTrades/High/Low 由引擎按重建的历史成交逐笔累计，
              单调不减；与官方快照的数值出入来自时间戳口径不同（官方 tick 有
              独立时间戳），属正常现象，不做对齐
            注意：回测时间只随市场事件前进，无事件的空白区间（如午休）不补发。
        event_snapshot_enabled: 事件驱动快照开关（默认 False）
            开启后每个市场事件（逐笔委托/逐笔成交含撤单）的全部处理
            ——撮合 + 策略响应产生的下单/撤单——结束后，从内部订单簿合成
            一个十档 Snapshot 推送策略一次（触发 onEventSnapshot 回调），
            推送次数 == 市场事件数；tick 事件不触发。快照口径与
            onRealTimeTickEvent 一致（十档只含历史订单/公开订单簿）。
    
    Returns:
        bool: 回测是否成功
    
    Example:
        # 单合约（股票）
        >>> data = {"688516.SH": (cstick_df, order_df, trade_df, csbar1d_df)}
        >>> run_backtest(data, strategy)
        
        # 多合约（股票）
        >>> data = {
        ...     "000488.SZ": (cstick_df1, order_df1, trade_df1, csbar1d_df1),
        ...     "688516.SH": (cstick_df2, order_df2, trade_df2, csbar1d_df2),
        ... }
        >>> run_backtest(data, strategy)
        
        # ETF 回测（三位小数）
        >>> data = {"159001.SZ": (cstick_df, order_df, trade_df, csbar1d_df, True)}
        >>> run_backtest(data, strategy)
        
        # 混合 ETF 和股票
        >>> data = {
        ...     "159001.SZ": (cstick_df1, order_df1, trade_df1, csbar1d_df1, True),   # ETF
        ...     "300827.SZ": (cstick_df2, order_df2, trade_df2, csbar1d_df2, False),  # 股票
        ... }
        >>> run_backtest(data, strategy)
        
        # 启用严格主动单模式
        >>> run_backtest(data, strategy, strict_active_order_mode=True)
        
        # 使用自定义数据
        >>> custom_df = pd.DataFrame({
        ...     'datetime': ['2025-11-17 09:35:00', '2025-11-17 10:00:00'],
        ...     'signal': [1, -1]
        ... })
        >>> run_backtest(data, strategy, custom_data=custom_df, enable_custom_data=True)
    """
    try:
        print(f"🚀 开始回测")
        print(f"   合约数量: {len(data_dict)}")
        for symbol, data_tuple in data_dict.items():
            # 支持 4 元组（股票）和 5 元组（ETF/股票）
            if len(data_tuple) == 5:
                cstick_df, order_df, trade_df, csbar1d_df, is_etf = data_tuple
            else:
                cstick_df, order_df, trade_df, csbar1d_df = data_tuple
                is_etf = False
            etf_flag = " [ETF]" if is_etf else ""
            print(f"   {symbol}{etf_flag}: Tick {len(cstick_df)}行, 委托 {len(order_df)}行, 成交 {len(trade_df)}行, 日线 {len(csbar1d_df)}行")
        
        if strict_active_order_mode:
            print(f"   ⚙️ 严格主动单模式: {'✅ 开启' if strict_active_order_mode else '❌ 关闭'}")
        
        if real_trade_match_mode:
            print(f"   ⚙️ 真实成交替代模式: ✅ 开启（主动单=严格模式，被动单=真实成交池）")
        
        if queue_info_enabled:
            print(f"   ⚙️ 队列信息回调: ✅ 开启（仅下单回调追加字段）")
        
        if enable_custom_data:
            print(f"   ⚙️ 自定义数据推送: {'✅ 开启' if enable_custom_data else '❌ 关闭'}")
        
        if realtime_tick_interval_ms and realtime_tick_interval_ms > 0:
            print(f"   ⚙️ 实时合成Tick: ✅ 开启（间隔 {realtime_tick_interval_ms}ms，触发 onRealTimeTickEvent）")

        if event_snapshot_enabled:
            print(f"   ⚙️ 事件驱动快照: ✅ 开启（每个市场事件推送一次，触发 onEventSnapshot）")
        
        # 创建 SymbolData 列表
        worker_label = f"（{n_workers} 进程）" if n_workers and n_workers > 1 else ""
        print(f"\n🔄 转换数据格式{worker_label}...")
        symbol_data_list = create_multi_symbol_data(data_dict, n_workers=n_workers,
                                                    release_input=release_input)

        if release_input:
            # 释放 Python DataFrame 内存，避免与 C++ SymbolData 双重占用
            import gc
            data_dict.clear()
            gc.collect()
        
        # 创建回测引擎
        print(f"⚙️ 初始化回测引擎...")
        engine = MultiBacktestEngine(symbol_data_list)
        
        # 设置真实成交替代模式（如果启用）—— 此模式包含严格主动单模式
        if real_trade_match_mode and hasattr(engine, 'setRealTradeMatchMode'):
            engine.setRealTradeMatchMode(True)
            if hasattr(engine, 'isRealTradeMatchMode'):
                print(f"   ✅ 真实成交替代模式已启用，当前状态: {engine.isRealTradeMatchMode()}")
            else:
                print(f"   ✅ 真实成交替代模式已启用")
        elif strict_active_order_mode and hasattr(engine, 'setStrictActiveOrderMode'):
            # 仅启用严格主动单模式（不启用真实成交替代）
            if hasattr(engine, 'isStrictActiveOrderMode'):
                print(f"   🔎 严格主动单模式(设置前): {engine.isStrictActiveOrderMode()}")
            engine.setStrictActiveOrderMode(True)
            if hasattr(engine, 'isStrictActiveOrderMode'):
                print(f"   ✅ 严格主动单模式已启用，当前状态: {engine.isStrictActiveOrderMode()}")
            else:
                print(f"   ✅ 严格主动单模式已启用")
        
        # 设置下单回调队列信息（可选）
        if queue_info_enabled and hasattr(engine, 'setQueueInfoEnabled'):
            engine.setQueueInfoEnabled(True)
            if hasattr(engine, 'isQueueInfoEnabled'):
                print(f"   ✅ 队列信息回调已启用，当前状态: {engine.isQueueInfoEnabled()}")
            else:
                print(f"   ✅ 队列信息回调已启用")
        
        # 设置实时合成 Tick 间隔（可选）
        if realtime_tick_interval_ms and realtime_tick_interval_ms > 0 \
                and hasattr(engine, 'setRealTimeTickInterval'):
            engine.setRealTimeTickInterval(int(realtime_tick_interval_ms))
            print(f"   ✅ 实时合成Tick已启用，间隔 {engine.getRealTimeTickInterval()}ms")

        # 设置事件驱动快照（可选）
        if event_snapshot_enabled and hasattr(engine, 'setEventSnapshotEnabled'):
            engine.setEventSnapshotEnabled(True)
            print(f"   ✅ 事件驱动快照已启用")
        
        # 设置自定义数据推送功能（如果启用）
        if enable_custom_data:
            if custom_data is None:
                raise ValueError("已启用自定义数据推送，但 custom_data 为空")
            
            # 校验必须有 datetime 列
            if 'datetime' not in custom_data.columns:
                raise ValueError("自定义数据必须包含 'datetime' 列")
            
            # 将 DataFrame 转换为时间戳列表和字典列表
            datetimes = custom_data['datetime'].astype(str).tolist()
            data_list = custom_data.to_dict(orient='records')
            
            # 设置自定义事件时间戳
            if hasattr(engine, 'loadCustomEventTimes'):
                engine.loadCustomEventTimes(datetimes)
            
            # 启用自定义数据功能
            if hasattr(engine, 'setCustomDataEnabled'):
                engine.setCustomDataEnabled(True)
            
            # 将数据列表设置到策略中
            if hasattr(strategy, 'setCustomDataList'):
                strategy.setCustomDataList(data_list)
            
            print(f"   ✅ 自定义数据已加载，共 {len(data_list)} 条记录")
        
        # 注册策略
        engine.registerStrategy(strategy)
        
        # 运行回测
        print(f"▶️ 运行回测...")
        engine.run()
        
        print(f"\n✅ 回测完成!")
        
        # 保存记录（如果指定了输出目录）
        if output_dir and hasattr(strategy, 'save_records'):
            strategy.save_records(output_dir)
        
        if hasattr(strategy, 'print_summary'):
            strategy.print_summary()
        
        _engine_refs.append(engine)
        return True
        
    except Exception as e:
        print(f"❌ 回测失败: {e}")
        import traceback
        traceback.print_exc()
        return False


# ========== 下单/撤单接口 ==========

def make_order(Broker: str,
               Account: str,
               Exchange: int,
               Instrument: str,
               OrderLocalID: str,
               OrderType: int,
               Direction: int,
               Price: int,
               Volume: int) -> UserEvent:
    """
    创建下单事件
    
    Args:
        Broker: 券商代码（可为空）
        Account: 账户标识
        Exchange: 交易所 (0=上海, 1=深圳)
        Instrument: 合约代码，如 "000488.SZ"
        OrderLocalID: 订单本地ID
        OrderType: 订单类型 (0=限价单)
        Direction: 方向 (1=买入, 2=卖出)
        Price: 价格（厘，1元=10000厘）
        Volume: 数量
    
    Returns:
        UserEvent: 下单事件
    
    Example:
        >>> order = make_order(
        ...     Broker='',
        ...     Account='user1',
        ...     Exchange=1,
        ...     Instrument='000488.SZ',
        ...     OrderLocalID='ORDER_001',
        ...     OrderType=0,
        ...     Direction=1,  # 买入
        ...     Price=80000,  # 8元
        ...     Volume=100
        ... )
    """
    from wangcai_syn.wangcai_cpp import Direction as Dir, OrderType as OT
    
    o = UserOrder()
    o.order_id = OrderLocalID
    o.symbol = Instrument
    o.direction = Dir.Buy if Direction == 1 else Dir.Sell
    o.order_type = OT.Limit if OrderType == 0 else OT.Market
    o.price = Price
    o.volume = Volume
    o.strategy_id = Account  # Account 作为内部 strategy_id
    
    return UserEvent(o)


def make_cancel(Broker: str,
                Account: str,
                Exchange: int,
                Instrument: str,
                CancelOrderLocalID: str,
                OrderLocalID: str) -> UserEvent:
    """
    创建撤单事件
    
    Args:
        Broker: 券商代码（可为空）
        Account: 账户标识
        Exchange: 交易所 (0=上海, 1=深圳)
        Instrument: 合约代码
        CancelOrderLocalID: 撤单请求ID
        OrderLocalID: 要撤销的订单ID
    
    Returns:
        UserEvent: 撤单事件
    
    Example:
        >>> cancel = make_cancel(
        ...     Broker='',
        ...     Account='user1',
        ...     Exchange=1,
        ...     Instrument='000488.SZ',
        ...     CancelOrderLocalID='CANCEL_001',
        ...     OrderLocalID='ORDER_001'
        ... )
    """
    c = UserCancel(OrderLocalID, Account)
    return UserEvent(c)


# ========== 价格工具函数 ==========

def convert_price_to_li(price_value) -> int:
    """
    转换价格到系统内部单位（厘）
    
    Args:
        price_value: 价格值（元）
        
    Returns:
        int: 以厘为单位的价格
    """
    if isinstance(price_value, (int, float)):
        if price_value < 1000:
            return int(price_value * 10000)
        else:
            return int(price_value)
    return 50000


def align_price(price: int) -> int:
    """
    价格对齐到100厘（分）
    """
    if price <= 0:
        return 100
    return round(price / 100) * 100


def format_price(price_li: int) -> str:
    """
    格式化价格显示（从厘转元）
    """
    return f"{price_li / 10000:.4f}"
