"""
旺财回测平台 - 工具函数模块
提供数据转换、上海股票订单还原、下单撤单等工具函数
"""

import pandas as pd
from typing import List, Dict, Tuple

from wangcai_syn.wangcai_cpp import (
    SymbolData, MultiBacktestEngine,
    UserEvent, UserOrder, UserCancel,
    Direction, OrderType
)


def revert_sh_order(order_df: pd.DataFrame, trade_df: pd.DataFrame, symbol: str) -> pd.DataFrame:
    """
    上海股票订单还原函数
    
    上交所的逐笔委托数据不完整，需要从逐笔成交数据中还原出被动方订单
    
    Args:
        order_df: 逐笔委托数据 DataFrame (csord)
        trade_df: 逐笔成交数据 DataFrame (cstra)
        symbol: 股票代码
    
    Returns:
        pd.DataFrame: 还原后的订单数据，格式与原始 order_df 一致
    """
    trade_df = trade_df.copy()
    order_df = order_df.copy()
    
    # 过滤交易时间段数据 (09:30 - 14:57)
    df_cstrad_filtered = trade_df[
        (trade_df['time'] >= '0 days 09:30:00') & 
        (trade_df['time'] <= '0 days 14:57:00')
    ].copy()
    
    # 买单：bidorderid > askorderid 说明买单是主动方，askorderid是被动方（之前已挂单）
    bids_df = df_cstrad_filtered[
        (df_cstrad_filtered['askorderid'] != 0) & 
        (df_cstrad_filtered['bidorderid'] > df_cstrad_filtered['askorderid'])
    ][['date', 'time', 'sym', 'price', 'size', 'bidorderid', 'bizindex', 'channelno']].copy()
    
    # 卖单：bidorderid < askorderid 说明卖单是主动方，bidorderid是被动方（之前已挂单）
    asks_df = df_cstrad_filtered[
        (df_cstrad_filtered['bidorderid'] != 0) & 
        (df_cstrad_filtered['bidorderid'] < df_cstrad_filtered['askorderid'])
    ][['date', 'time', 'sym', 'price', 'size', 'askorderid', 'bizindex', 'channelno']].copy()
    
    # 标记买卖方向和订单类型
    bids_df['side'] = 1
    asks_df['side'] = -1
    bids_df['ordertype'] = 0
    asks_df['ordertype'] = 0
    
    # 重命名订单ID
    bids_df = bids_df.rename(columns={'bidorderid': 'orderid'})
    asks_df = asks_df.rename(columns={'askorderid': 'orderid'})
    
    # 合并买卖订单
    ask_bid_df = pd.concat([bids_df, asks_df], ignore_index=True)
    
    # 合并订单数据（原始订单 + 从成交还原的订单）
    csord_revert_df = pd.concat([
        ask_bid_df, 
        order_df[['date', 'time', 'sym', 'price', 'size', 'side', 'ordertype', 'orderid', 'bizindex', 'channelno']]
    ], ignore_index=True)
    
    # 按orderid分组聚合
    csord_revert_df = csord_revert_df.groupby(['orderid']).agg({
        'date': 'first',
        'time': 'min',
        'sym': 'first',
        'price': ['max', 'min'],
        'size': 'sum',
        'side': 'first',
        'ordertype': 'first',
        'channelno': 'first',
        'bizindex': 'first'
    }).reset_index()
    
    # 展平多级列名
    csord_revert_df.columns = ['orderid', 'date', 'time', 'sym', 'price_max', 'price_min', 
                                 'size', 'side', 'ordertype', 'channelno', 'bizindex']
    
    # 根据side选择价格：买单用最大价格，卖单用最小价格
    csord_revert_df['price'] = csord_revert_df['price_max'].where(
        csord_revert_df['side'] == 1, csord_revert_df['price_min']
    )
    
    # 删除临时列
    csord_revert_df = csord_revert_df.drop(columns=['price_max', 'price_min'])
    
    # 添加seqno和updatetime列
    csord_revert_df['seqno'] = csord_revert_df['bizindex']
    csord_revert_df['updatetime'] = csord_revert_df['time']
    
    # 按bizindex排序
    csord_revert_df = csord_revert_df.sort_values(by='bizindex').reset_index(drop=True)
    
    # 重新排列列的顺序
    csord_revert_df = csord_revert_df[[
        'date', 'time', 'sym', 'price', 'size', 'side', 
        'ordertype', 'orderid', 'channelno', 'seqno', 'bizindex', 'updatetime'
    ]]
    
    return csord_revert_df


def create_symbol_data(symbol: str, 
                       cstick_df: pd.DataFrame,
                       order_df: pd.DataFrame, 
                       trade_df: pd.DataFrame,
                       csbar1d_df: pd.DataFrame) -> SymbolData:
    """
    从 DataFrame 创建 SymbolData 对象
    
    Args:
        symbol: 合约代码，如 "000488.SZ"
        cstick_df: Tick快照数据 DataFrame
        order_df: 逐笔委托数据 DataFrame
        trade_df: 逐笔成交数据 DataFrame
        csbar1d_df: 日线数据 DataFrame（包含涨跌停限制）
    
    Returns:
        SymbolData: 回测引擎所需的数据对象
    """
    # 处理可能的 bytes 类型字段
    trade_df = trade_df.copy()
    if 'exectype' in trade_df.columns:
        trade_df['exectype'] = trade_df['exectype'].apply(
            lambda x: x.decode('utf-8') if isinstance(x, bytes) else x)
    if 'tradebsflag' in trade_df.columns:
        trade_df['tradebsflag'] = trade_df['tradebsflag'].apply(
            lambda x: x.decode('utf-8') if isinstance(x, bytes) else x)
    
    # 对于上海股票，进行订单还原
    if symbol.endswith('.SH'):
        order_df = revert_sh_order(order_df, trade_df, symbol)
        print(f"[{symbol}] 上海股票订单还原完成，还原后订单数: {len(order_df)}")
    
    # 转换为 CSV 字符串
    cstick_csv = cstick_df.to_csv(index=False)
    order_csv = order_df.to_csv(index=False)
    trade_csv = trade_df.to_csv(index=False)
    csbar1d_csv = csbar1d_df.to_csv(index=False)
    
    return SymbolData(symbol, cstick_csv, order_csv, trade_csv, csbar1d_csv)


def create_multi_symbol_data(data_dict: Dict[str, Tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame]]) -> List[SymbolData]:
    """
    从字典创建多个合约的 SymbolData
    
    Args:
        data_dict: 格式为 {symbol: (cstick_df, order_df, trade_df, csbar1d_df), ...}
    
    Returns:
        List[SymbolData]: SymbolData 对象列表
    
    Example:
        >>> data = {
        ...     "000488.SZ": (cstick_df1, order_df1, trade_df1, csbar1d_df1),
        ...     "688516.SH": (cstick_df2, order_df2, trade_df2, csbar1d_df2),
        ... }
        >>> symbol_data_list = create_multi_symbol_data(data)
    """
    symbol_data_list = []
    for symbol, (cstick_df, order_df, trade_df, csbar1d_df) in data_dict.items():
        symbol_data = create_symbol_data(symbol, cstick_df, order_df, trade_df, csbar1d_df)
        symbol_data_list.append(symbol_data)
    return symbol_data_list


def run_backtest(data_dict: Dict[str, Tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame]], 
                 strategy, 
                 output_dir: str = None,
                 strict_active_order_mode: bool = False,
                 custom_data: pd.DataFrame = None,
                 enable_custom_data: bool = False) -> bool:
    """
    运行回测（支持单合约/多合约，单策略）
    
    Args:
        data_dict: 格式为 {symbol: (cstick_df, order_df, trade_df, csbar1d_df), ...}
        strategy: 策略实例（单个策略）
        output_dir: 输出目录，默认不保存
        strict_active_order_mode: 是否启用严格主动单模式（默认关闭）
            开启后，虚拟主动单只能成交市场实际提供的量，剩余部分撤单，
            且在欠债未还清前禁止下新的主动单
        custom_data: 用户自定义数据 DataFrame（默认None）
            必须包含 'datetime' 列，格式如 '2025-11-17 09:35:00'
            其他列可自定义，会作为 dict 传递给策略的 onCustomEvent
        enable_custom_data: 是否启用自定义数据推送功能（默认关闭）
            需设为 True 才会启用自定义数据推送
    
    Returns:
        bool: 回测是否成功
    
    Example:
        # 单合约
        >>> data = {"688516.SH": (cstick_df, order_df, trade_df, csbar1d_df)}
        >>> run_backtest(data, strategy)
        
        # 多合约
        >>> data = {
        ...     "000488.SZ": (cstick_df1, order_df1, trade_df1, csbar1d_df1),
        ...     "688516.SH": (cstick_df2, order_df2, trade_df2, csbar1d_df2),
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
        for symbol, (cstick_df, order_df, trade_df, csbar1d_df) in data_dict.items():
            print(f"   {symbol}: Tick {len(cstick_df)}行, 委托 {len(order_df)}行, 成交 {len(trade_df)}行, 日线 {len(csbar1d_df)}行")
        
        if strict_active_order_mode:
            print(f"   ⚙️ 严格主动单模式: {'✅ 开启' if strict_active_order_mode else '❌ 关闭'}")
        
        if enable_custom_data:
            print(f"   ⚙️ 自定义数据推送: {'✅ 开启' if enable_custom_data else '❌ 关闭'}")
        
        # 创建 SymbolData 列表
        print(f"\n🔄 转换数据格式...")
        symbol_data_list = create_multi_symbol_data(data_dict)
        
        # 创建回测引擎
        print(f"⚙️ 初始化回测引擎...")
        engine = MultiBacktestEngine(symbol_data_list)
        
        # 设置严格主动单模式（如果启用）
        if strict_active_order_mode and hasattr(engine, 'setStrictActiveOrderMode'):
            # 这里额外打印一次状态，便于确认"是否真的启用了 C++ 侧的严格模式"
            # （避免因为加载到旧 .so 或绑定未生效导致误判）
            if hasattr(engine, 'isStrictActiveOrderMode'):
                print(f"   🔎 严格主动单模式(设置前): {engine.isStrictActiveOrderMode()}")
            engine.setStrictActiveOrderMode(True)
            if hasattr(engine, 'isStrictActiveOrderMode'):
                print(f"   ✅ 严格主动单模式已启用，当前状态: {engine.isStrictActiveOrderMode()}")
            else:
                print(f"   ✅ 严格主动单模式已启用")
        
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
