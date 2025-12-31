"""
    所有的因子计算逻辑（输出逻辑）
"""
import pprint
import time
import queue
from functools import partial
import threading
import multiprocessing
from time import sleep
from datetime import datetime
from typing import Dict, List, Callable
from signal_gen.factor_utils import get_nth_last_element
from utils.shared_logger import logger

###############################level1 测试因子->当前时刻因子###############################
def mid(snapshot_buffer,config:Dict) -> Dict:
    ask1,bid1 = snapshot_buffer['askPrice1'][0],snapshot_buffer['bidPrice1'][0]
    return 'mid',(ask1+bid1)/2/10000

def asize1_delt(snapshot_buffer,config:Dict) -> Dict:
    '''
        所需中间值，askvolume
        'asize1_delt',          # 卖一档挂单量单期变化（当前值-前值)
    '''
    if len(snapshot_buffer) < 2:
        return 'asize1_delt',None
    cur_key = next(reversed(snapshot_buffer))
    last_key = get_nth_last_element(snapshot_buffer,2)
    pre_askvolume,cur_askvolume = snapshot_buffer[last_key]['askvolume'][0][0],snapshot_buffer[cur_key]['askvolume'][0][0]
    delta_t = config['delta_t']
    return 'asize1_delt',(cur_askvolume - pre_askvolume)/delta_t

def bsize1_delt(snapshot_buffer,config:Dict) -> Dict:
    '''
        所需中间值，bidvolume
        'bsize1_delt',          # 买一档挂单量单期变化（当前值-前值)
    '''
    if len(snapshot_buffer) < 2:
        return 'bsize1_delt',None
    cur_key = next(reversed(snapshot_buffer))
    last_key = get_nth_last_element(snapshot_buffer,2)
    pre_bidvolume,cur_bidvolume = snapshot_buffer[last_key]['bidvolume'][0][0],snapshot_buffer[cur_key]['bidvolume'][0][0]
    delta_t=config['delta_t']
    return 'bsize1_delt',(cur_bidvolume - pre_bidvolume)/delta_t

def imb(snapshot_buffer, config:Dict):
    '''
    Order Book Imbalance, 指定深度买卖订单量求和后再滚动平均，计算订单薄不平衡指标,量化盘口买卖双方压力
    输入参数：
    - df: 行情快照表。
    - depth(档位):1、5、10
    - slot:1 #大于1则ic 很低 实际选择值时也只是用1

    所需中间值：
    - askvolume
    - bidvolume
    '''
    depth = config['depth']
    slot = config['slot']
    try:
        ask_sizes = [d.get('askvolume') for _,d in snapshot_buffer.items()][0]
        bid_sizes = [d.get('bidvolume') for _,d in snapshot_buffer.items()][0]

        ask_qtys = sum([sum(x[:depth]) for x in ask_sizes])
        bid_qtys = sum([sum(x[:depth]) for x in bid_sizes])
        signal = -(ask_qtys - bid_qtys)/(ask_qtys + bid_qtys)

        #TODO: intime calculation normalization how to do it?
        return f'imb@depth={depth}@slot={slot}',signal
    except:
        return f'imb@depth={depth}@slot={slot}',None 

############################### level2 factors###############################
def factor16(last_valid_dict:Dict,config:Dict) -> Dict:
    """最近3s主动卖出的成交股票金额

    Args:
        last_valid_dict (Dict): 中间值字典
        config (Dict): 相关配置文件

    Returns:
        ("因子名称"，因子结果)
    """
    result = 0.0
    try:
        result = last_valid_dict['active_sell_size']
        return "factor16" , result/10000
    except Exception as e:
        logger.info(f"factor16 calc wrong {e}")
        return "factor16" , result

def factor19(last_valid_dict,config:Dict) -> Dict:
    """最近3s买入撤单的数量Volume
    Args:
        last_valid_dict (Dict): 中间值字典
        config (Dict): 相关配置文件

    Returns:
        ("因子名称"，因子结果)
    """
    try:
        result = last_valid_dict['buy_cancel_volume']
        return "factor19",result
    except Exception as e:
        logger.info(f"factor19 calc wrong {e}")
        return "factor19",None
    
def factor20(last_valid_dict,config:Dict) -> Dict:
    """最近3s卖出撤单的数量Volume
    Args:
        last_valid_dict (Dict): 中间值字典
        config (Dict): 相关配置文件

    Returns:
        ("因子名称"，因子结果)
    """
    try:
        result = last_valid_dict['sell_cancel_volume']
        return "factor20",result
    except Exception as e:
        logger.info(f"factor20 calc wrong {e}")
        return "factor20",None

