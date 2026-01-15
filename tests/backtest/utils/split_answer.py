import pandas as pd
import os
import argparse
from datetime import datetime
import multiprocessing as mp
from pathlib import Path
import numpy as np

def load_time_range(time_range_path):
    """加载time_range.csv文件，获取所有时间点"""
    time_range_df = pd.read_csv(time_range_path)
    return time_range_df['time'].unique()

def process_single_file(input_path, stock_code, time_range, date_column='timestamp', output_root='./'):
    """
    处理单个CSV文件，按日期分割并保存为多个文件，并与time_range对齐
    
    参数:
    input_path: 输入CSV文件路径
    stock_code: 股票代码
    time_range: 统一的时间范围数组
    date_column: 日期时间列的列名，默认为'timestamp'
    output_root: 输出根目录，默认为当前目录
    """
    try:
        # 构建输出目录路径
        output_dir = os.path.join(output_root, stock_code)
        
        # 确保输出目录存在
        if not os.path.exists(output_dir):
            os.makedirs(output_dir)
        
        # 读取CSV文件
        print(f"进程 {os.getpid()}: 正在处理文件: {input_path}")
        df = pd.read_csv(input_path)
        
        # 只保留需要的列
        if 'factor16' in df.columns:
            df = df[['date', 'factor16', 'time']]
        else:
            print(f"警告: 文件 {input_path} 中未找到factor16列，将保留所有列")
        
        # 将时间列转换为与time_range相同的格式
        df['time'] = df['time'] * 10
        
        # 按日期分组
        grouped = df.groupby('date')
        
        # 为每个日期创建单独的文件
        file_count = 0
        for date, group in grouped:
            # 格式化日期为YYYYMMDD
            date_str = str(date)
            
            # 创建完整的时间序列DataFrame
            full_time_df = pd.DataFrame({'time': time_range})
            
            # 合并原始数据
            merged_df = pd.merge(full_time_df, group[['factor16', 'time']], 
                                on='time', how='left')
            
            # 填充缺失值为0
            merged_df['factor16'] = merged_df['factor16'].fillna(0)
            
            # 构建输出文件名
            output_filename = f"{stock_code}_{date_str}_answer.csv"
            output_path = os.path.join(output_dir, output_filename)
            
            # 保存数据到CSV文件
            merged_df.to_csv(output_path, index=False)
            file_count += 1
        
        print(f"进程 {os.getpid()}: 完成处理 {input_path}, 生成 {file_count} 个文件")
        return True
        
    except Exception as e:
        print(f"进程 {os.getpid()}: 处理文件 {input_path} 时出错: {str(e)}")
        return False

def find_csv_files(root_path):
    """
    在根目录下递归查找所有的factor16.csv文件
    
    参数:
    root_path: 根目录路径
    
    返回:
    文件路径和对应股票代码的列表
    """
    csv_files = []
    root_path = Path(root_path)
    
    # 遍历根目录下的所有子目录
    for stock_dir in root_path.iterdir():
        if stock_dir.is_dir():
            csv_path = stock_dir / "factor16.csv"
            if csv_path.exists():
                # 从目录名获取股票代码
                stock_code = stock_dir.name
                csv_files.append((str(csv_path), stock_code))
    
    return csv_files

def process_files_parallel(csv_files, time_range_path, date_column='timestamp', output_root='./', max_workers=None):
    """
    使用进程池并行处理多个CSV文件
    
    参数:
    csv_files: 包含文件路径和股票代码的元组列表
    time_range_path: time_range.csv文件路径
    date_column: 日期时间列的列名
    output_root: 输出根目录
    max_workers: 最大工作进程数，默认为CPU核心数
    """
    if max_workers is None:
        max_workers = mp.cpu_count()
    
    # 加载统一的时间范围
    time_range = load_time_range(time_range_path)
    print(f"加载time_range.csv，共 {len(time_range)} 个时间点")
    
    print(f"使用 {max_workers} 个进程并行处理 {len(csv_files)} 个文件")
    
    # 准备参数列表
    params = [(path, code, time_range, date_column, output_root) for path, code in csv_files]
    
    # 创建进程池
    with mp.Pool(processes=max_workers) as pool:
        # 使用starmap并行处理
        results = pool.starmap(process_single_file, params)
    
    # 统计成功和失败的数量
    success_count = sum(results)
    failure_count = len(results) - success_count
    
    print(f"处理完成: 成功 {success_count} 个, 失败 {failure_count} 个")

if __name__ == "__main__":
    # 设置命令行参数解析
    parser = argparse.ArgumentParser(description='并行按日期分割股票CSV数据，并与time_range对齐')
    parser.add_argument('--root_path', default="level2_factor/answer_path", help='根目录路径，包含股票代码子文件夹')
    parser.add_argument('--time_range', default='level2_factor/time_range.csv', help='统一时间范围文件路径')
    parser.add_argument('--date_column', default='date_time', help='日期时间列的列名（默认为timestamp）')
    parser.add_argument('--output_root', default='level2_factor/split_path', help='输出根目录（默认为当前目录）')
    parser.add_argument('--workers', type=int, default=10, help='工作进程数（默认为CPU核心数）')
    
    # 解析参数
    args = parser.parse_args()
    
    start_time = datetime.now()
    
    # 查找所有需要处理的CSV文件
    csv_files = find_csv_files(args.root_path)
    
    if not csv_files:
        print(f"在 {args.root_path} 目录下未找到任何factor16.csv文件")
        exit(1)
    
    # 检查time_range文件是否存在
    if not os.path.exists(args.time_range):
        print(f"错误: time_range文件 {args.time_range} 不存在")
        exit(1)
    
    # 并行处理文件
    process_files_parallel(
        csv_files=csv_files,
        time_range_path=args.time_range,
        date_column=args.date_column,
        output_root=args.output_root,
        max_workers=args.workers
    )

    print(f"总耗时: {datetime.now() - start_time}")