# import numpy as np
import sys
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT_DIR))

import pandas as pd
from typing import *
import multiprocessing
from core.conditions import Timeline
from utils.config import PM_END

# 然后使用绝对导入
from utils.shared_logger import logger,get_logger
from signal_gen.factor_calc import *

logger = get_logger("factor_calc")


class FactorUnit:
    def __init__(self,factor_names,factor_func:List,middle_value_names:Dict,factor_type:str,max_span=200,pop_condition:str='length'):
        """初始化因子计算单元（使用共同中间值的因子计算）
        Args:
            factor_func (List): 因子计算的函数（具体输出时候的操作）
            middle_value_name (Dict): 每个中间值/原始数据的名称和对应需要的计算数据的位置（键名+索引号）
            middle_value_name example:
            middle_value_name = {
                "name1":{keyname1:index1}, 索引号为None表示选取所有元素
                "name2":{keyname2:index2},
            }
            factor_type (str): 因子类型，使用的是什么类型的数据
            factor_func:因子计算的中间值聚合函数（包括参数）列表,负责具体的因子计算
            注意：每个因子计算函数包含两个部分的输入参数：计算数值和配置参数，这里填入的是输入参数
            factor_func example:
            factor_func = [
                (func1, {"args1":value1}),
                (func2, {"args2_1":value2_1, "args2_2":value2_2}),
                (func3, {"args3_1":value3_1, "args3_2":value3_2, "args3_3":value3_3})
            ]
            max_span (int, optional): 队列最大长度（元素个数）或者时间窗口大小（秒数）. Defaults to 200（长度单位）.
            pop_conditon (str, optional):使用队列长度还是时间窗口作为条件 . 默认为长度为标准'length'，可选项:'length'/'time'.
        """
        self.factor_names = factor_names
        self.max_span = max_span
        self.pop_condition = pop_condition
        self.middle_value_dict = {}     # 中间值存储区(某一个时刻的贡献值，如增加量，减少量等)
        self.output_buffer = {}     # 最新有效值（最新时刻的最新状态值，如累加值）
        self.middle_value_names = middle_value_names
        self.factor_func = factor_func
        self.factor_type = factor_type
        self.start_flag:bool = True
        self.latest_data = None #存储最新一条
        self.middle_dict_lock = threading.Lock()  # 用于线程安全访问中间值字典


    def set_inst(self,inst):
        self.inst = inst

    def set_timeline(self,timeline):
        self.timeline = timeline
    
    def set_trade_date(self,date):
        self.trade_date = date

    def update_data(self,data):
        try:
            #更新数据之后立即计算
            self.latest_data = data
            self.middle_value_calc()
        except StopIteration:
            pass
        except Exception as e:
            logger.error(f"update data has wrong by {e}")

    def _one_middle_value_calc_(self,key_name:Dict,operation:Callable,op_type:str='raw'):
        """针对一个中间值的计算过程
            根据给出的需要计算的数据和指定的操作，得出当前时刻的中间值
        Args:
            key_name (Dict): 在订阅的数据中，所要使用的键名称（可以是一组值）
            opertation (function): 对于收到的值的操作，传入一个函数，对于对应的输入值进行操作
        """
        #TODO：此处的数据方式是否可以进行优化
        try:
            tmp_result = []
            tmp_middle_value = 0
            # TODO：评估这里的用时
            for key,index in key_name.items():
                if index == None:
                    tmp_result.append(self.latest_data[key])
                elif key in self.latest_data and 0 <= index < len(self.latest_data[key]):
                    tmp_result.append(self.latest_data[key][index])
            
            if op_type == 'raw':
                tmp_middle_value = operation(*tmp_result) if operation else tmp_result
            elif op_type =='transfer':
                tmp_middle_value = operation(*tmp_result)
            elif op_type == 'accumulated':
                tmp_middle_value = operation(*tmp_result)
        except Exception as e:
            logger.error(f"one calc is wrong {e}")
        return tmp_middle_value
    
    def pop_value(self):
                #弹出不符合条件的最大长度的队列元素
        with self.middle_dict_lock:
            #以长度为标准pop
            if self.pop_condition=='length':
                if len(self.middle_value_dict) > self.max_span:
                    oldest_key = next(iter(self.middle_value_dict))
                    content = self.middle_value_dict.pop(oldest_key)
                    logger.info(content)
            #以时间长度为标准pop
            elif self.pop_condition=='time':
                if len(self.middle_value_dict):
                    oldest_key = next(iter(self.middle_value_dict))
                    time_range_start = self.timeline.get_last_trigger_time()
                    # 最旧的一条数据超出时间范围就弹出
                    while oldest_key <= time_range_start:
                        # 获取要pop的内容
                        content = self.middle_value_dict.pop(oldest_key)
                        # pop后将更改最新的中间值字典
                        for k,v in content.items():
                            self.output_buffer[k] = max(0,self.output_buffer[k]-v)
                            with open(f'factor/{self.inst}/calc_process_{self.inst}_{self.trade_date}.txt','a') as f:
                                print(f"clock:{self.timeline.get_newest_time()} last valid value{self.output_buffer}",file=f)
                                print(f"clock:{self.timeline.get_newest_time()} -{v}",file=f)
                            #TODO：操作类型改为可选，比如可以累加或减
                        if self.middle_value_dict:
                            oldest_key = next(iter(self.middle_value_dict))
                        else:
                            break
            else:
                logger.info('Unspported pop condition.Please change to length or time.')

    def middle_value_calc(self):
        """ 计算所有的中间值，
            中间值结果添加至中间值字典
            如果字典长度超过了最大长度，则弹出第一个添加的元素
        """
        #中间值计算流程
        try:
            self.timestamp = self.latest_data['Time']
            tmp_middle_dict = {}
            for middle_name,[middle_value,operation,op_type] in self.middle_value_names.items():
                tmp_middle_value = self._one_middle_value_calc_(middle_value,operation,op_type)
                tmp_middle_dict[middle_name] = tmp_middle_value

                if middle_name in self.output_buffer:
                    if op_type=='accumulated':
                        self.output_buffer[middle_name] += tmp_middle_value
                        with open(f'factor/{self.inst}/calc_process_{self.inst}_{self.trade_date}.txt','a') as f:
                            print(f"clock:{self.timeline.get_newest_time()} last valid value{self.output_buffer}, +{tmp_middle_value}",file=f)
                    else:
                        self.output_buffer[middle_name] = tmp_middle_value
                else:
                    self.output_buffer[middle_name] = tmp_middle_value

            #将新增中间值至缓冲区
            if self.timestamp in self.middle_value_dict:
                for k,v in tmp_middle_dict.items():
                    self.middle_value_dict[self.timestamp][k] += v
            else:
                self.middle_value_dict[self.timestamp] = tmp_middle_dict
        except Exception as e:
            logger.error(f"add middle value data has wrong {e}")

        self.pop_value()

        with open(f'factor/{self.inst}/calc_process_{self.inst}_{self.trade_date}.txt','a') as f:
            print(f"clock:{self.timeline.get_newest_time()} last valid value{self.output_buffer}",file=f)

    def calculate_factors(self):
        """计算因子值"""
        with self.middle_dict_lock:
            if not self.middle_value_dict and not self.output_buffer:
                return {}
            try:
                factor_result = {}
                for func, args in self.factor_func:
                    factor_name, result = func(self.output_buffer, args)
                    factor_result[factor_name] = result
                return factor_result
            except Exception as e:
                logger.error(f'Factor calculation error: {e}')
                return {}

class FactorEngine:
    def __init__(
            self,
            inst:str,
            trade_date:str,
            snapshot_q:queue.Queue,
            trade_detail_q:queue.Queue,
            order_detail_q:queue.Queue,
            trade_order_detail_q:queue.Queue,
            factor_q:queue.Queue,
            timeline:Timeline,
            end_time:int = PM_END,
        ):
        
        self.inst = inst
        self.trade_date = trade_date
        self.last_output_time = 0
        self.factor_list = []
        self.snapshot_q = snapshot_q[0] if isinstance(snapshot_q,tuple) else snapshot_q
        self.trade_detail_q = trade_detail_q[0] if isinstance(trade_detail_q,tuple) else trade_detail_q
        self.order_detail_q = order_detail_q[0] if isinstance(order_detail_q,tuple) else order_detail_q
        self.trade_order_detail_q = trade_order_detail_q[0] if isinstance(trade_order_detail_q,tuple) else trade_order_detail_q
        self.factor_q = factor_q[0] if isinstance(factor_q,tuple) else factor_q
        
        self.snapshot_compute_units = []
        self.order_compute_units = []
        self.trade_compute_units = []
        self.trade_order_compute_units = []
        self.stop_event = threading.Event()

        self.cond =  timeline.tick_condtion#第一个条件是snapshot到来，第二个条件是当前时刻的下一个时刻的逐笔数据到达
        self.latest_timestamp = 0
        self.timeline = timeline
        self.end_time = end_time

    def init_threads(self, daemon:bool=False):
        """初始化所有计算线程"""
        threads = [
            # threading.Thread(target=self._process_queue_, args=(self.snapshot_q, self.snapshot_compute_units)),
            threading.Thread(target=self._process_queue_, args=(self.trade_detail_q, self.trade_compute_units)),
            # threading.Thread(target=self._process_queue_, args=(self.order_detail_q, self.order_compute_units)),
            # threading.Thread(target=self._process_queue_, args=(self.trade_order_detail_q, self.trade_order_compute_units)),
            # threading.Thread(target=self._collect_factors_)
        ]

        for t in threads:
            t.daemon = daemon
            t.start()


    def register(self,unit:FactorUnit):
        #unit register init
        unit.set_inst(self.inst['Instrument'])
        unit.set_timeline(self.timeline)
        unit.set_trade_date(self.trade_date)

        self.factor_list += unit.factor_names
        if unit.factor_type == 'snapshot':
            self.snapshot_compute_units.append(unit)
        elif unit.factor_type == 'order':
            self.order_compute_units.append(unit)
        elif unit.factor_type == 'trade':
            self.trade_compute_units.append(unit)
        elif unit.factor_type == 'trade_order':
            self.trade_order_compute_units.append(unit)
        logger.info(f"{self.inst} {unit.factor_names} register successfully!")
    
    def stop(self):
        """停止所有计算线程"""
        self.stop_event.set()
        with self.cond:
            self.cond.notify_all()  # 唤醒等待的线程
        logger.info("Factor engine stopped")


    def _process_queue_(self, data_q: multiprocessing.Queue, units: List[FactorUnit]):
        """处理数据队列并更新因子单元"""
        while not self.stop_event.is_set():
            try:
                new_data = data_q.get(True, 0.01)
                if new_data['Time'] > self.timeline.get_newest_time():
                    self.timeline.set_newest_time(new_data['Time'])
                now_time,trigger_time = new_data['Time'],self.timeline.get_trigger_time()
                time_diff = self.timeline.sub_two_time(now_time,trigger_time)

                while time_diff > self.timeline.interval:
                    logger.info(f"time diff is {time_diff} need to add.")
                    self._collect_factors_(self.timeline.get_trigger_time(),add_time_flag=False)
                    time_diff = self.timeline.sub_two_time(now_time,self.timeline.get_trigger_time())
                    self.timeline.add_trigger_time()

                    if self.timeline.get_trigger_time() in range(11295700000,130000000):
                        continue
                    #没有新数据，就要在下一条数据到达的时候进行之前数据的更新，主要是要把中间值字典的数据内容删除掉
                    for unit in self.trade_compute_units:
                        unit.pop_value()

                if (now_time > self.timeline.get_trigger_time()):
                    self._collect_factors_(self.timeline.get_trigger_time())
                
                for unit in units:
                    unit.update_data(new_data)
            except queue.Empty:
                continue

    def _collect_factors_(self,output_time,add_time_flag=True):
        """收集并整合所有因子结果"""
        logger.info(f"{self.timeline.get_newest_time()} {self.timeline.get_trigger_time()} condition triggered.")
        all_factors = dict.fromkeys(self.factor_list,None)

        # 计算快照因子
        ##TODO:更加高效的方式进行因子数据的输出操作
        # for unit in self.snapshot_compute_units:
        #     all_factors.update(unit.calculate_factors())
        
        # 计算成交因子
        for unit in self.trade_compute_units:
            all_factors.update(unit.calculate_factors())
        
        # # 计算委托因子
        # for unit in self.order_compute_units:
        #     all_factors.update(unit.calculate_factors())
        
        # # 计算委托成交组合因子
        # for unit in self.trade_order_compute_units:
        #     all_factors.update(unit.calculate_factors())
        
        #TODO:timestamp的标识应该是当前这个触发信号发出的时间？（时间标志仅作实验使用，实际运算不需要这个键）

        all_factors['time'] = output_time
        with open(f"factor/{self.inst['Instrument']}/calc_process_{self.inst['Instrument']}_{self.trade_date}.txt",'a') as f:
            print(f"clock:{self.timeline.get_newest_time()} {all_factors}",file=f)
        if add_time_flag:
            self.timeline.add_trigger_time()
        factor_df = pd.DataFrame([all_factors])
        logger.info(all_factors)
        self.factor_q.put(factor_df)
        if self.latest_timestamp > self.end_time:
            self.stop()