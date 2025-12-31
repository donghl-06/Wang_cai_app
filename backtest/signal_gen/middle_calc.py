"""
    中间值函数的计算逻辑
"""
# 判断主动买入/卖出的函数
def calc_active_buy_amount(bid_order_seq: int, ask_order_seq: int, 
                        trade_volume: int, trade_price: float) :
    """计算主动买入金额"""
    buy_amt = 0.0
    if bid_order_seq > ask_order_seq:  # 主动买入
        buy_amt = trade_volume * trade_price
    elif bid_order_seq < ask_order_seq:  # 主动卖出
        buy_amt = 0.0
    else:  
        buy_amt = 0.0
    return buy_amt

def calc_passive_buy_amount(trade_volume: int, trade_price: float,
                        bid_order_seq: int, ask_order_seq: int):
    """计算主动卖出金额"""
    sell_amt = 0.0
    if bid_order_seq > ask_order_seq:  # 主动买入
        sell_amt = 0.0
    elif bid_order_seq < ask_order_seq:  # 主动卖出
        sell_amt = trade_volume * trade_price
    else:  # 中性交易
        sell_amt = 0.0
    return sell_amt

#最近3s的统计指标
def calc_buyin_cancel(trade_volume: int, trade_price: float,side:str,orderkind:str,
                        ):
    """计算累加买入的撤单量
    """
    cancel_volume = 0.0
    if orderkind == 'D' and side == 'B':
        cancel_volume = trade_volume
    return cancel_volume


def calc_sellout_cancel(trade_volume: int,trade_price: float,side:str,orderkind:str,
                        ):
    """计算累加买入的撤单量
    """
    cancel_volume = 0.0
    if orderkind == 'D' and side == 'S':
        cancel_volume = trade_volume
    return cancel_volume

def calc_active_sell_size(trade_volume: int, trade_price: float,
                        bid_order_seq: int, ask_order_seq: int):
    """主动卖出的成交股票金额
    """
    active_sell_size = 0.0
    if trade_price > 0 and bid_order_seq < ask_order_seq:
        active_sell_size = trade_volume * trade_price
    return active_sell_size
