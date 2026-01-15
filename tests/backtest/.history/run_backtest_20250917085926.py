#!/usr/bin/env python
import itertools
import shutil
import os,sys

os.chdir(sys.path[0])
sys.path.append('../.')
from datetime import datetime
import multiprocessing
from threading import BoundedSemaphore

import utils.config
from utils.config import tickerDict,tickerList,trade_date_list_202401,multi_ticker_list,trade_date_list_202201
from utils.config import AM_FIRST_TICK, AM_END, PM_BEGIN, PM_END
from utils.shared_logger import logger,setup_logger

from signal_gen.factor_engine import *
from core.conditions import Timeline
from signal_gen import factor_calc
from signal_gen.middle_calc import *
from signal_gen.factor_def import trade_unit,level1_unit,mid_unit

sys.path.insert(0, '/home/linzhuoyu/project/wangcai_huice/wangcai_cpp/build')

from wangcai_cpp import (
    BacktestEngine, Strategy,
    Direction, OrderType,
    Event, UserEvent, Execution, Snapshot,
    OrderDetail, TradeDetail,  # 新增：原始市场数据结构
    make_order_event, make_cancel_event,
    TradeCallback, OrderCallback, MultiBacktestEngine
)

from strategy.factor_strategy import FactorStrategy


def worker(args_list):
    inst = args_list[0]
    date = args_list[1]

    #定义所有队列和信号量
    snapshot_q = multiprocessing.Queue()
    tradetail_q = multiprocessing.Queue()
    orddetail_q = multiprocessing.Queue()
    trade_order_q = multiprocessing.Queue()
    factor_q = multiprocessing.Queue()
    inst_dict = {'Instrument':inst}
    logger.info(f"The application {inst} {date} start successfully")

    timeline = Timeline()
    logger.info(f"inst:{inst} process is start!")

    strategy = FactorStrategy('factor_calc',inst_dict,date,snapshot_q,tradetail_q,orddetail_q,trade_order_q,factor_q,unit_list,timeline)
    strategy.init_work_td()

    date = f'{date[:4]}-{date[4:6]}-{date[6:]}'
    engine = MultiBacktestEngine([inst],date,'data')
    engine.registerStrategy(strategy)
    engine.run()

    logger.info(f"The application {inst} {date} end successfully")


if __name__ == '__main__':
    logger.info("main process start")
    unit_list = [trade_unit]

    ticker_list = [utils.config.code]
    if not os.path.exists('factor'):
        os.makedirs('factor',exist_ok=True)
        os.makedirs('factor/result',exist_ok=True)
        for ticker in ticker_list:
            if not os.path.exists(f'factor/{ticker}/'):
                os.makedirs(f'factor/{ticker}/')
    # for debug
    code,date = '600085.SH','20240102'
    if os.path.exists(f'factor/{code}/calc_process_{code}_{date}.txt'):
        os.remove(f'factor/{code}/calc_process_{code}_{date}.txt')
    if os.path.exists(f'factor/result/{code}_{date}_result.csv'):
        os.remove(f'factor/result/{code}_{date}_result.csv')
    if not os.path.exists(f'factor/{code}'):
        os.makedirs(f'factor/{code}')
    worker((code,date))
    
    # if os.path.exists('factor'):
    #     shutil.rmtree('factor')
    #     os.makedirs('factor',exist_ok=True)
    #     os.makedirs('factor/result',exist_ok=True)
    #     for ticker in multi_ticker_list+tickerList:
    #         if not os.path.exists(f'factor/{ticker}/'):
    #             os.makedirs(f'factor/{ticker}/')

    # args_list = list(itertools.product(tickerList, trade_date_list_202401))
    # args_list_2022 = list(itertools.product(multi_ticker_list, trade_date_list_202201))
    # args_list += args_list_2022
    # with multiprocessing.Pool(40) as pool:
    #     res = pool.imap_unordered(worker, args_list)
    #     res = list(res)
    #     sys.exit(0)