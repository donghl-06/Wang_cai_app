import aqdatac
import pandas as pd
import os

def login_aqdatac():
    """登录aqdatac"""
    username = os.getenv('MY_USER_NAME')
    password = os.getenv('MY_PASSWORD')
    
    if not username or not password:
        raise ValueError("请设置环境变量 MY_USER_NAME 和 MY_PASSWORD")
    aqdatac.login(username, password)
    print("登录成功")

def revert(sym, date):
    """
    订单还原函数
    
    参数:
        sym (str): 股票代码，如 '600050.SH'
        date (str): 日期，如 '2025-02-17'
    
    返回:
        pandas.DataFrame: 还原后的订单数据，以datetime为索引
    """
    # 获取数据
    df_cstra = aqdatac.get_data("cstra", date, date, sym)
    df_csord = aqdatac.get_data("csord", date, date, sym)
    print(f"获取{sym}数据成功，获取到的成交数据有{len(df_cstra)}条，订单数据有{len(df_csord)}条")
    
    # 过滤交易时间段数据
    df_cstrad_filtered = df_cstra[
        (df_cstra['datetime'].dt.time >= pd.to_datetime('09:30').time()) & 
        (df_cstra['datetime'].dt.time <= pd.to_datetime('14:57').time())
    ].copy()
    
    # 去掉撤单成交，保留首次撮合的成交
    bids_df = df_cstrad_filtered[
        (df_cstrad_filtered['askorderid'] != 0) & 
        (df_cstrad_filtered['bidorderid'] > df_cstrad_filtered['askorderid'])
    ][['datetime', 'sym', 'price', 'size', 'bidorderid', 'bizindex', 'channelno']].copy()
    
    asks_df = df_cstrad_filtered[
        (df_cstrad_filtered['bidorderid'] != 0) & 
        (df_cstrad_filtered['bidorderid'] < df_cstrad_filtered['askorderid'])
    ][['datetime', 'sym', 'price', 'size', 'askorderid', 'bizindex', 'channelno']].copy()
    
    # 标记买卖方向
    bids_df['side'] = 1
    asks_df['side'] = -1
    
    # 标记订单类型
    bids_df['ordertype'] = 0
    asks_df['ordertype'] = 0
    
    # 重命名订单ID
    bids_df = bids_df.rename(columns={'bidorderid': 'orderid'})
    asks_df = asks_df.rename(columns={'askorderid': 'orderid'})
    
    # 合并买卖订单
    ask_bid_df = pd.concat([bids_df, asks_df], ignore_index=True)
    
    # 合并订单数据
    csord_revert_df = pd.concat([
        ask_bid_df, 
        df_csord[['datetime', 'sym', 'price', 'size', 'side', 'ordertype', 'orderid', 'bizindex', 'channelno']]
    ], ignore_index=True)
    
    # 按orderid分组聚合 - 先分别计算最大和最小价格
    csord_revert_df = csord_revert_df.groupby(['orderid']).agg({
        'datetime': 'min', 
        'sym': 'first',
        'price': ['max', 'min'],  # 同时计算最大和最小价格
        'size': 'sum',
        'side': 'first',
        'ordertype': 'first',
        'channelno': 'first',
        'bizindex': 'first'
    }).reset_index()
    
    # 展平多级列名
    csord_revert_df.columns = ['orderid', 'date', 'time', 'sym', 'price_max', 'price_min', 
                                 'size', 'side', 'ordertype', 'channelno', 'bizindex','updatetime']
    
    # 根据side选择合适的价格：买单(side=1)用最大价格，卖单(side=-1)用最小价格（向量化操作）
    csord_revert_df['price'] = csord_revert_df['price_max'].where(
        csord_revert_df['side'] == 1, csord_revert_df['price_min']
    )
    
    # 删除临时列
    csord_revert_df = csord_revert_df.drop(columns=['price_max', 'price_min'])
    
    # 添加seqno列，值等于bizindex
    csord_revert_df['seqno'] = csord_revert_df['bizindex']
    
    # 按bizindex排序（只需要一次）
    csord_revert_df = csord_revert_df.sort_values(by='bizindex').reset_index(drop=True)
    
    # 重新排列列的顺序
    csord_revert_df = csord_revert_df[[
        'date', 'time', 'sym', 'price', 'size', 'side', 
        'ordertype', 'orderid', 'channelno', 'seqno', 'bizindex',
        'updatetime'
    ]]
    
    # 设置datetime为索引
    csord_revert_df = csord_revert_df.set_index('datetime')
    return csord_revert_df

# 使用示例
if __name__ == "__main__":
    # 登录
    login_aqdatac()
    # 执行订单还原
    result = revert('600050.SH', '2025-02-17')
    print(result.head())
