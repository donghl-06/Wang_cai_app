import pandas as pd

# Files
files = [
    'failure_log.csv',
    'comparison_log.csv',
    'result/etftrader_analysis/passive/buy_stock/merged_buy_stock_log_with_factors.csv',
    'result/etftrader_analysis_wangcai/merged_passive_log_with_factors.csv',
    'result/etftrader_analysis_wangcai/merged_passive_mid_opponent_log_with_factors.csv',
    'result/etftrader_analysis_wangcai/merged_passive_mid_own_log_with_factors.csv',
    'result/etftrader_analysis_wangcai/merged_passive_opponent_log_with_factors.csv'
]

# Step 1: Deduplicate all in memory
dfs = {}
for f in files:
    df = pd.read_csv(f)
    print(f'Before deduplication {f}: {df.shape}')
    df.drop_duplicates(inplace=True)
    print(f'After deduplication {f}: {df.shape}')
    dfs[f] = df

# Step 2: Filter comparison_log.csv to Correct == False
df_comp = dfs['comparison_log.csv']
print('Sample Correct values before filtering:', df_comp['Correct'].value_counts())
df_comp_filtered = df_comp[df_comp['Correct'] == False]
print(f'After filtering comparison_log.csv: {df_comp_filtered.shape}')
df_comp_filtered.to_csv('comparison_log_filtered.csv', index=False)
print('Saved filtered comparison_log.csv as comparison_log_filtered.csv')

# Step 3 and 4: Get erroneous stock-date pairs from filtered comparison_log.csv, failure_log.csv, and alignment_failure_log.csv
erroneous = set((row['StockCode'], row['Date']) for _, row in df_comp_filtered.iterrows())

# Add from failure_log.csv
df_fail = dfs['failure_log.csv']
erroneous.update((row['StockCode'], row['Date']) for _, row in df_fail.iterrows())

# Add from alignment_failure_log.csv
df_align = pd.read_csv('alignment_failure_log.csv')
df_align.drop_duplicates(inplace=True)
erroneous.update((row['StockCode'], row['Date']) for _, row in df_align.iterrows())

print(f'Number of erroneous stock-date pairs: {len(erroneous)}')

# Filter merged_buy_stock_log_with_factors.csv
for strategy in ["passive", "passive_mid_opponent", "passive_mid_own", "passive_opponent"]:
    df_buy = pd.read_csv(f'result/etftrader_analysis/{strategy}/buy_stock/merged_buy_stock_log_with_factors.csv')
    print(f'Before filtering merged_buy_stock_log_with_factors.csv: {df_buy.shape}')
    df_buy_filtered = df_buy[~df_buy.apply(lambda row: (row['stock_code'], row['date']) in erroneous, axis=1)]
    print(f'After filtering merged_buy_stock_log_with_factors.csv: {df_buy_filtered.shape}')
    df_buy_filtered.to_csv(f'result/etftrader_analysis/{strategy}/buy_stock/merged_buy_stock_log_with_factors_filtered.csv', index=False)
    print('Saved filtered merged_buy_stock_log_with_factors.csv as merged_buy_stock_log_with_factors_filtered.csv')

# Filter merged_passive_log.csv
df_passive = dfs['result/etftrader_analysis_wangcai/merged_passive_log_with_factors.csv']
print(f'Before filtering merged_passive_log.csv: {df_passive.shape}')
df_passive_filtered = df_passive[~df_passive.apply(lambda row: (row['stock_code'], row['date']) in erroneous, axis=1)]
print(f'After filtering merged_passive_log.csv: {df_passive_filtered.shape}')
df_passive_filtered.to_csv('result/etftrader_analysis_wangcai/merged_passive_log_with_factors_filtered.csv', index=False)
print('Saved filtered merged_passive_log.csv as merged_passive_log_with_factors_filtered.csv')

# Filter merged_passive_mid_opponent_log_with_factors.csv
df_mid_opponent = dfs['result/etftrader_analysis_wangcai/merged_passive_mid_opponent_log_with_factors.csv']
print(f'Before filtering merged_passive_mid_opponent_log_with_factors.csv: {df_mid_opponent.shape}')
df_mid_opponent_filtered = df_mid_opponent[~df_mid_opponent.apply(lambda row: (row['stock_code'], row['date']) in erroneous, axis=1)]
print(f'After filtering merged_passive_mid_opponent_log_with_factors.csv: {df_mid_opponent_filtered.shape}')
df_mid_opponent_filtered.to_csv('result/etftrader_analysis_wangcai/merged_passive_mid_opponent_log_with_factors_filtered.csv', index=False)
print('Saved filtered merged_passive_mid_opponent_log_with_factors.csv as merged_passive_mid_opponent_log_with_factors_filtered.csv')

# Filter merged_passive_mid_own_log_with_factors.csv
df_mid_own = dfs['result/etftrader_analysis_wangcai/merged_passive_mid_own_log_with_factors.csv']
print(f'Before filtering merged_passive_mid_own_log_with_factors.csv: {df_mid_own.shape}')
df_mid_own_filtered = df_mid_own[~df_mid_own.apply(lambda row: (row['stock_code'], row['date']) in erroneous, axis=1)]
print(f'After filtering merged_passive_mid_own_log_with_factors.csv: {df_mid_own_filtered.shape}')
df_mid_own_filtered.to_csv('result/etftrader_analysis_wangcai/merged_passive_mid_own_log_with_factors_filtered.csv', index=False)
print('Saved filtered merged_passive_mid_own_log_with_factors.csv as merged_passive_mid_own_log_with_factors_filtered.csv')

# Filter merged_passive_opponent_log_with_factors.csv
df_opponent = dfs['result/etftrader_analysis_wangcai/merged_passive_opponent_log_with_factors.csv']
print(f'Before filtering merged_passive_opponent_log_with_factors.csv: {df_opponent.shape}')
df_opponent_filtered = df_opponent[~df_opponent.apply(lambda row: (row['stock_code'], row['date']) in erroneous, axis=1)]
print(f'After filtering merged_passive_opponent_log_with_factors.csv: {df_opponent_filtered.shape}')
df_opponent_filtered.to_csv('result/etftrader_analysis_wangcai/merged_passive_opponent_log_with_factors_filtered.csv', index=False)
print('Saved filtered merged_passive_opponent_log_with_factors.csv as merged_passive_opponent_log_with_factors_filtered.csv')