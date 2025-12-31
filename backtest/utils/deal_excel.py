import pandas as pd
import numpy as np

# 读取CSV文件
input_csv = 'factor/active_trade_data.csv'  # 替换为你的CSV文件路径
output_excel = 'utils/active_trade_data.xlsx'  # 输出Excel文件路径

# 读取CSV文件
df = pd.read_csv(input_csv)

# 打印原始数据类型
print("原始数据类型:")
print(df.dtypes)
print("\n前几行数据:")
print(df.head())

# 确保time列是正确格式
df['time'] = df['time'].astype(int)

# 处理数值列转换
for column in df.columns:
    if column != 'time':  # 跳过time列
        print(f"\n处理列: {column}")
        
        # 检查列中是否包含非数值数据
        non_numeric_count = sum(pd.to_numeric(df[column], errors='coerce').isna())
        if non_numeric_count > 0:
            print(f"  警告: 列 '{column}' 包含 {non_numeric_count} 个非数值值")
            
            # 尝试清理数据（移除空格、特殊字符等）
            df[column] = df[column].astype(str).str.strip()
            df[column] = df[column].str.replace(r'[^\d.-]', '', regex=True)
            
            # 检查清理后的情况
            non_numeric_count_after = sum(pd.to_numeric(df[column], errors='coerce').isna())
            print(f"  清理后非数值值数量: {non_numeric_count_after}")
        
        # 转换为数值类型，然后转换为整数
        try:
            df[column] = pd.to_numeric(df[column], errors='coerce')
            # 使用Int64类型支持NaN值
            df[column] = df[column].astype('Int64')
            print(f"  成功将列 '{column}' 转换为整数类型")
        except Exception as e:
            print(f"  错误: 无法将列 '{column}' 转换为整数类型: {e}")
            # 如果转换失败，尝试找出问题值
            problem_values = df[column][pd.to_numeric(df[column], errors='coerce').isna()]
            if len(problem_values) > 0:
                print(f"  问题值示例: {problem_values.head().tolist()}")

# 计算contribution列
try:
    df['contribution'] = df['price'] * df['vol']
    print("成功计算contribution列")
except Exception as e:
    print(f"计算contribution列时出错: {e}")

# 计算每个time点内的累计贡献值
df['cumulative_contribution'] = 0  # 初始化累计列

# 按time分组并计算累计值
current_time = None
cumulative_sum = 0

for index, row in df.iterrows():
    if current_time != row['time']:
        # time变化，重置累计值
        cumulative_sum = 0
        current_time = row['time']
    
    # 累加当前行的贡献值
    if pd.notna(row['contribution']):
        cumulative_sum += row['contribution']
    
    # 更新累计值列
    df.at[index, 'cumulative_contribution'] = cumulative_sum

print("成功计算每个time点内的累计贡献值")

# 找出time变化的行索引
change_points = []
for i in range(1, len(df)):
    if df.iloc[i]['time'] != df.iloc[i-1]['time']:
        change_points.append(i)

# 从后往前插入空行，避免索引问题
for pos in sorted(change_points, reverse=True):
    # 创建空行（与df列数相同，值全为NaN）
    empty_row = pd.DataFrame({col: [np.nan] for col in df.columns})
    # 在变化点插入空行
    df = pd.concat([df.iloc[:pos], empty_row, df.iloc[pos:]]).reset_index(drop=True)

# 打印转换后的数据类型
print("\n转换后数据类型:")
print(df.dtypes)

# 保存为Excel文件
try:
    df.to_excel(output_excel, index=False)
    print(f"处理完成！结果已保存到: {output_excel}")
    
    # 打印处理结果的摘要
    print("\n处理结果摘要:")
    print(f"总行数: {len(df)}")
    print(f"不同time点数量: {df['time'].nunique()}")
    print(f"累计贡献值范围: {df['cumulative_contribution'].min()} - {df['cumulative_contribution'].max()}")
    
except Exception as e:
    print(f"保存Excel文件时出错: {e}")