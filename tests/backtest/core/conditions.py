"""
    分别管理两个条件的触发,
        条件一：快照数据到达
        条件二：当前时刻的逐笔数据的下一个时刻的数据的到达
    两个条件同时满足的时候触发cond,此时进行因子的输出
"""
import threading
from multiprocessing import Value
from utils.config import AM_FIRST_TICK
from utils.shared_logger import logger


class Timeline:
    """记录当前的时间进度
    """
    def __init__(
            self,
            init_time = AM_FIRST_TICK,
            interval = 3000 
        ):
        self.interval = interval
        """因子输出的时间间隔,1000对应1s,默认3s"""
        self.time_lock = threading.Lock()
        """对于时间修改的锁"""
        self.newest_time = Value('i',init_time)
        """当前票对应的最新一条逐笔数据的时间(交易所时间)"""
        self.last_trigger_time = Value('i',init_time)
        """上一次的触发时间"""
        self.trigger_time = Value('i',init_time + 3000)
        """当前应该被触发的时间点，但是这个时间点到达的时候不进行因子输出，需要当大于这个时间的下一个逐笔数据到达的时候才能触发"""
        self.tick_condtion = threading.Condition()
        """绑定逐笔成交数据的Condition,表示对应当前已经到来的逐笔成交数据是否应该输出因子"""
        self.stop_event = Value('b',False)
        """停止事件"""
        self.output_flag_lock = threading.Lock()
        """输出因子标志锁"""
        self.output_factor_flag = Value('b',False)
        """输出因子的标识"""

    def set_newest_time(self,newest_time):
        with self.time_lock:
            self.newest_time.value = newest_time

    def get_newest_time(self):
        return self.newest_time.value

    def get_trigger_time(self):
        return self.trigger_time.value
    
    def get_last_trigger_time(self):
        return self.last_trigger_time.value

    def change_output_flag(self,state=True):
        with self.output_flag_lock:
            self.output_factor_flag.value = state

    def add_trigger_time(self):
        if self.trigger_time.value == 112957000:
            self.trigger_time.value = 130000000
            self.last_trigger_time.value = 112957000
        else:
            self.last_trigger_time.value = self.trigger_time.value
            hours = self.trigger_time.value // 10000000
            minutes = (self.trigger_time.value // 100000) % 100
            seconds = (self.trigger_time.value // 1000) % 100
            fractions = self.trigger_time.value % 1000

            fractions += self.interval

            carry_s = fractions // 1000
            fractions %= 60
            seconds += carry_s

            carry_m = seconds // 60
            seconds %= 60
            minutes += carry_m

            carry_h = minutes // 60
            minutes %= 60
            hours += carry_h

            self.trigger_time.value = hours * 10000000 + minutes * 100000 + seconds * 1000 + fractions
    
    def sub_trigger_time(self,trigger_time):
        if trigger_time.value==130003000:
            return 130000000
        elif trigger_time.value==130000000:
            return 112957000
        hours = trigger_time // 10000000
        minutes = (trigger_time // 100000) % 100
        seconds = (trigger_time // 1000) % 100
        fractions = trigger_time % 1000
        fractions -= self.interval

        while fractions < 0:
            fractions += 1000
            seconds -= 1
            
            while seconds < 0:
                seconds += 60
                minutes -= 1
                
                while minutes < 0:
                    minutes += 60
                    hours -= 1
                    
                    if hours < 0:
                        hours = 0
                        minutes = 0
                        seconds = 0
                        fractions = 0
                        break 
        return hours * 10000000 + minutes * 100000 + seconds * 1000 + fractions
    def to_milliseconds(self,time_int):
        hours = time_int // 10000000
        minutes = (time_int // 100000) % 100
        seconds = (time_int // 1000) % 100
        milliseconds = time_int % 1000
        total_ms = hours * 3600000 + minutes * 60000 + seconds * 1000 + milliseconds
        return total_ms

    def sub_two_time(self,now_time, trigger_time):
        if trigger_time<=113000000 and now_time >= 130000000:
            trigger_time = 130000000
        total_ms1 = self.to_milliseconds(now_time)
        total_ms2 = self.to_milliseconds(trigger_time)
        return total_ms1 - total_ms2
    
    
    # def run(self):
    #     while not self.stop_event.value:
    #         if self.output_factor_flag:
    #             with self.tick_condtion:
    #                 #TODO:不在这里进行条件控制
    #                 if self.output_factor_flag.value:
    #                     self.change_output_flag(False)
    #                     logger.info(f"{self.get_newest_time()} {self.get_trigger_time()} condition triggered.")
    #                     self.tick_condtion.notify_all()
