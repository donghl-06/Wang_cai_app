import glob
import re
import os

files = glob.glob('/mnt/team/research-share/huangchunyang/etfmm_oe/wangcai_log_v2/**/*.log', recursive=True)
total = 0
correct = 0

with open('/mnt/team/research-share/huangchunyang/etfmm_oe/comparison_log.csv', 'w', encoding='utf-8') as log_file:
    log_file.write("Predicted,Real,Correct,StockCode,Date,File\n")
    for file in files:
        date = os.path.basename(file).replace('.log', '')
        with open(file, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        for i, line in enumerate(lines, 1):
            if '集合竞价完成' in line:
                match = re.search(r'开盘价=([0-9.]+).*真实开盘价=([0-9.]+)', line)
                if match:
                    pred = float(match.group(1))
                    real = float(match.group(2))
                    is_correct = pred == real
                    total += 1
                    if is_correct:
                        correct += 1
                    
                    # Find stock code in subsequent lines
                    code = 'Unknown'
                    for j in range(i, len(lines)):
                        if '策略 ETFTRADER_' in lines[j]:
                            match_code = re.search(r'策略 ETFTRADER_([0-9]+\.[A-Z]+)', lines[j])
                            if match_code:
                                code = match_code.group(1)
                                break
                    
                    log_file.write(f"{pred},{real},{is_correct},{code},{date},{file}:{i}\n")

print(f"Total comparisons: {total}")
print(f"Correct predictions: {correct}")
print(f"Accuracy: {correct/total:.4f}" if total > 0 else "No data found")
print("Comparison log saved to comparison_log.csv")