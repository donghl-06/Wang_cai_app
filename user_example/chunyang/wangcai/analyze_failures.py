import glob
import re
import os

files = glob.glob('/mnt/team/research-share/huangchunyang/etfmm_oe/wangcai_log_v2/**/*.log', recursive=True)
total_failures = 0

with open('/mnt/team/research-share/huangchunyang/etfmm_oe/failure_log.csv', 'w', encoding='utf-8') as log_file:
    log_file.write("Date,StockCode,File\n")
    for file in files:
        date = os.path.basename(file).replace('.log', '')
        with open(file, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        for i, line in enumerate(lines, 1):
            if '用户订单处理失败: price out of limit up/down' in line:
                total_failures += 1
                
                # Find stock code in nearby lines (next 20 lines)
                code = 'Unknown'
                end = min(len(lines), i+20)
                for j in range(i, end):
                    match_code = re.search(r'ETFTRADER_([0-9]+\.[A-Z]+)', lines[j])
                    if match_code:
                        code = match_code.group(1)
                        break
                
                log_file.write(f"{date},{code},{file}:{i}\n")

print(f"Total failures: {total_failures}")
print("Failure log saved to failure_log.csv")