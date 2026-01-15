# shared_logger.py - 多进程安全的全局日志模块
import os
import sys
import logging
import logging.config
import logging.handlers
import atexit
import multiprocessing
from datetime import datetime
from typing import Optional

class AutoLogger:
    _initialized = False
    _log_queue = None
    _listener = None
    _experiment_name = ""
    
    @classmethod
    def setup(cls, log_dir="logs", level="INFO", log_to_console=True, experiment_name: Optional[str] = None):
        if cls._initialized:
            return logging.getLogger()
        
        # 创建日志目录
        os.makedirs(log_dir, exist_ok=True)
        
        # 获取主脚本名称
        main_script = os.path.basename(sys.argv[0]) if sys.argv else "interactive"
        
        # 设置实验名称
        if experiment_name:
            cls._experiment_name = experiment_name
        else:
            # 只在主进程中提示输入
            if multiprocessing.parent_process() is None:  # 主进程
                cls._experiment_name = input("输入试验名称：") or "unnamed"
            else:  # 子进程使用默认名称
                cls._experiment_name = "child_process"
        
        # 创建带时间戳的日志文件名
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        script_base = main_script.split('.')[0]
        log_file = f"{log_dir}/{script_base}/{script_base}_{timestamp}_{cls._experiment_name}.log"
        os.makedirs(f"{log_dir}/{script_base}", exist_ok=True)

        # 创建多进程安全的队列
        cls._log_queue = multiprocessing.Queue(-1)
        
        # 日志配置
        formatter = logging.Formatter(
            f'%(asctime)s | %(processName)-10s | {cls._experiment_name} | %(levelname)-8s | %(message)s',
            datefmt='%Y-%m-%d %H:%M:%S'
        )
        
        # 文件处理器
        file_handler = logging.FileHandler(log_file, encoding='utf-8')
        file_handler.setFormatter(formatter)
        file_handler.setLevel(level)
        
        # 控制台处理器
        console_handler = logging.StreamHandler()
        console_handler.setFormatter(formatter)
        console_handler.setLevel(level)
        
        # 创建队列监听器
        handlers = [file_handler]
        if log_to_console:
            handlers.append(console_handler)
            
        cls._listener = logging.handlers.QueueListener(
            cls._log_queue, *handlers, respect_handler_level=True
        )
        cls._listener.start()
        
        # 配置根记录器
        root_logger = logging.getLogger()
        root_logger.setLevel(level)
        
        # 添加队列处理器
        queue_handler = logging.handlers.QueueHandler(cls._log_queue)
        root_logger.addHandler(queue_handler)
        
        cls._initialized = True
        
        # 记录启动信息
        root_logger.info("🚀 日志系统初始化完成")
        root_logger.info(f"📋 命令行参数: {' '.join(sys.argv)}")
        root_logger.info(f"📁 日志文件: {log_file}")
        root_logger.info(f"👤 实验名称: {cls._experiment_name}")
        
        # 注册退出处理
        atexit.register(cls._exit_log)
        
        return root_logger
    
    @classmethod
    def get_logger(cls, name=None):
        """获取配置好的日志记录器"""
        if not cls._initialized:
            # 子进程可能未初始化，自动配置
            return cls.setup(experiment_name="auto_child")
        
        logger = logging.getLogger(name)
        
        # 确保子进程中的记录器使用队列处理器
        if not any(isinstance(h, logging.handlers.QueueHandler) for h in logger.handlers):
            queue_handler = logging.handlers.QueueHandler(cls._log_queue)
            logger.addHandler(queue_handler)
            logger.propagate = False  # 防止重复记录
        
        return logger
    
    @classmethod
    def _exit_log(cls):
        if cls._initialized:
            logger = logging.getLogger()
            logger.info("🛑 程序正常退出")
            
            # 停止监听器并清空队列
            if cls._listener:
                cls._listener.stop()
            
            # 清空队列
            if cls._log_queue:
                while not cls._log_queue.empty():
                    try:
                        cls._log_queue.get_nowait()
                    except:
                        pass
                cls._log_queue.close()
                cls._log_queue.join_thread()

# 全局访问点
def setup_logger(**kwargs):
    return AutoLogger.setup(**kwargs)

def get_logger(name=None):
    return AutoLogger.get_logger(name)

# 自动初始化（仅在主进程中）
if multiprocessing.parent_process() is None:
    logger = setup_logger()
else:
    logger = get_logger()