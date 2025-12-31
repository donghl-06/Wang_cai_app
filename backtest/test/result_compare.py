import pandas as pd
import numpy as np
from pathlib import Path
import argparse
import multiprocessing as mp
from collections import defaultdict
import json
import csv
from datetime import datetime

def extract_stock_code_and_date(filename):
    """
    从文件名中提取股票代号和日期
    """
    filename = Path(filename).stem  

    parts = filename.split('_')
    if len(parts) >= 2:
        stock_code = parts[0]
        date_str = parts[1]
        
        if date_str.isdigit() and len(date_str) == 8:
            return stock_code, date_str
    
    return None, None

def find_matching_answer_file(result_file, answer_dir):
    """
    根据结果文件名找到对应的答案文件
    """
    stock_code, date_str = extract_stock_code_and_date(result_file)
    
    if not stock_code or not date_str:
        return None

    return Path(answer_dir,stock_code,f"{stock_code}_{date_str}_answer.csv")

def find_all_result_files(result_dir, patterns=None):
    """
    查找所有结果文件
    """
    if patterns is None:
        patterns = ["*.csv"]
    
    result_dir_path = Path(result_dir)
    result_files = []
    
    for pattern in patterns:
        result_files.extend(list(result_dir_path.glob(pattern)))
    
    return [str(f) for f in result_files if f.is_file()]

def compare_result_with_answer(result_file, answer_file, key_column, value_column):
    """
    比较计算结果文件和答案文件，统计相同值的百分比
    """
    try:
        result_df = pd.read_csv(result_file)
        answer_df = pd.read_csv(answer_file)
        
        if key_column not in result_df.columns or key_column not in answer_df.columns:
            raise ValueError(f"键列 '{key_column}' 在文件中不存在")
        
        if value_column not in result_df.columns or value_column not in answer_df.columns:
            raise ValueError(f"值列 '{value_column}' 在文件中不存在")
        
        # 确保键列按照顺序排列
        result_df = result_df.sort_values(by=key_column)
        answer_df = answer_df.sort_values(by=key_column)
        
        result_dict = dict(zip(result_df[key_column], result_df[value_column]))
        answer_dict = dict(zip(answer_df[key_column], answer_df[value_column]))
        
        # 获取所有key的交集
        common_keys = set(result_dict.keys()) & set(answer_dict.keys())
        result_only_keys = set(result_dict.keys()) - set(answer_dict.keys())
        answer_only_keys = set(answer_dict.keys()) - set(result_dict.keys())
        all_keys = list(set(result_dict.keys()) | set(answer_dict.keys()))
                

        # 对键进行排序以便有序输出
        common_keys = sorted(common_keys)
        result_only_keys = sorted(result_only_keys)
        answer_only_keys = sorted(answer_only_keys)
        
        if not common_keys:
            return {
                'result_file': result_file,
                'answer_file': answer_file,
                'stock_code': extract_stock_code_and_date(result_file)[0] or Path(result_file).stem,
                'date': extract_stock_code_and_date(result_file)[1] or 'unknown',
                'total_keys': len(all_keys),
                'common_keys': 0,
                'matching_keys': 0,
                'match_percentage': 0.0,
                'key_overlap_percentage': 0.0,
                'missing_in_result': len(answer_only_keys),
                'missing_in_answer': len(result_only_keys),
                'result_only_items': {k: result_dict[k] for k in result_only_keys},
                'answer_only_items': {k: answer_dict[k] for k in answer_only_keys},
                'mismatch_details': [],
                'status': 'no_common_keys'
            }
        
        # 统计匹配的key
        matching_keys = 0
        mismatch_details = []
        
        for key in common_keys:
            # 处理NaN值的特殊情况
            if (pd.isna(result_dict[key]) and pd.isna(answer_dict[key]))\
                or (pd.isna(result_dict[key]) and answer_dict[key]==0):
                matching_keys += 1
            elif not pd.isna(result_dict[key]) and not pd.isna(answer_dict[key]):
                # 尝试将值转换为浮点数进行比较（到小数点后一位进行比较）
                result_val = float(result_dict[key])
                answer_val = float(answer_dict[key])
                
                # 四舍五入到小数点后一位进行比较
                result_rounded = round(result_val, 2)
                answer_rounded = round(answer_val, 2)
                
                if result_rounded == answer_rounded:
                    matching_keys += 1
                else:
                    # 计算数值差异
                    diff = result_val - answer_val
                    abs_diff = abs(diff)
                    rel_diff = abs_diff / abs(answer_val) if answer_val != 0 else float('inf')
                    
                    mismatch_details.append({
                        'key': key,
                        'result_value': result_dict[key],
                        'answer_value': answer_dict[key],
                        'difference': diff,
                        'abs_difference': abs_diff,
                        'relative_difference': rel_diff
                    })
            else:
                mismatch_details.append({
                    'key': key,
                    'result_value': result_dict[key],
                    'answer_value': answer_dict[key],
                    'difference': 'NaN mismatch',
                    'abs_difference': 'NaN mismatch',
                    'relative_difference': 'NaN mismatch'
                })
        
        # 计算百分比
        match_percentage = (matching_keys / 4739) * 100 #TODO:除数改为一天所有的键值
        key_overlap_percentage = (len(common_keys) / len(all_keys)) * 100
        
        return {
            'result_file': result_file,
            'answer_file': answer_file,
            'stock_code': extract_stock_code_and_date(result_file)[0] or Path(result_file).stem,
            'date': extract_stock_code_and_date(result_file)[1] or 'unknown',
            'total_keys': len(all_keys),
            'common_keys': len(common_keys),
            'matching_keys': matching_keys,
            'match_percentage': match_percentage,
            'key_overlap_percentage': key_overlap_percentage,
            'missing_in_result': len(answer_only_keys),
            'missing_in_answer': len(result_only_keys),
            'mismatch_count': len(mismatch_details),
            'mismatch_details': sorted(mismatch_details, key=lambda x: x['key']),  # 按key排序
            'result_only_items': {k: result_dict[k] for k in result_only_keys},
            'answer_only_items': {k: answer_dict[k] for k in answer_only_keys},
            'status': 'success'
        }
        
    except Exception as e:
        return {
            'result_file': result_file,
            'answer_file': answer_file,
            'stock_code': extract_stock_code_and_date(result_file)[0] or Path(result_file).stem,
            'date': extract_stock_code_and_date(result_file)[1] or 'unknown',
            'error': str(e),
            'status': 'error'
        }

def generate_individual_report(comparison_result, output_dir):
    """
    为单个文件生成详细对比报告，按照股票代码创建子文件夹
    """
    stock_code = comparison_result['stock_code']
    stock_output_dir = Path(output_dir) / stock_code
    stock_output_dir.mkdir(parents=True, exist_ok=True)
    
    # 从文件名创建报告文件名
    result_filename = Path(comparison_result['result_file']).stem
    report_filename = f"{result_filename}_comparison_report.txt"
    report_path = stock_output_dir / report_filename
    
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write("=" * 80 + "\n")
        f.write("文件对比详细报告\n")
        f.write("=" * 80 + "\n\n")
        
        f.write(f"结果文件: {comparison_result['result_file']}\n")
        f.write(f"答案文件: {comparison_result['answer_file']}\n")
        f.write(f"股票代码: {comparison_result['stock_code']}\n")
        f.write(f"日期: {comparison_result['date']}\n")
        f.write(f"状态: {comparison_result['status']}\n\n")
        
        if comparison_result['status'] == 'error':
            f.write(f"错误信息: {comparison_result['error']}\n")
            return report_path
        
        f.write("总体统计:\n")
        f.write(f"  总键数: {comparison_result['total_keys']}\n")
        f.write(f"  共同键数: {comparison_result['common_keys']}\n")
        f.write(f"  匹配键数: {comparison_result['matching_keys']}\n")
        f.write(f"  匹配百分比: {comparison_result['match_percentage']:.2f}%\n")
        f.write(f"  键重叠百分比: {comparison_result['key_overlap_percentage']:.2f}%\n")
        f.write(f"  结果文件中缺失的键: {comparison_result['missing_in_answer']}\n")
        f.write(f"  答案文件中缺失的键: {comparison_result['missing_in_result']}\n")
        f.write(f"  值不匹配的键: {comparison_result['mismatch_count']}\n\n")
        
        # 输出结果文件特有的键值对（按键排序）
        if comparison_result['result_only_items']:
            f.write("结果文件特有的键值对:\n")
            for key in sorted(comparison_result['result_only_items'].keys()):
                f.write(f"  {key}: {comparison_result['result_only_items'][key]}\n")
            f.write("\n")
        
        # 输出答案文件特有的键值对（按键排序）
        if comparison_result['answer_only_items']:
            f.write("答案文件特有的键值对:\n")
            for key in sorted(comparison_result['answer_only_items'].keys()):
                f.write(f"  {key}: {comparison_result['answer_only_items'][key]}\n")
            f.write("\n")
        
        # 输出不匹配的键值对详情（已按key排序）
        if comparison_result['mismatch_details']:
            f.write("不匹配的键值对详情:\n")
            f.write("键\t结果值\t答案值\t绝对值差\t相对差值\n")
            f.write("-" * 80 + "\n")
            
            for detail in comparison_result['mismatch_details']:
                f.write(f"{detail['key']}\t{detail['result_value']}\t{detail['answer_value']}\t")
                
                if detail['abs_difference'] == 'NaN mismatch':
                    f.write("NaN mismatch\tNaN mismatch\n")
                elif detail['abs_difference'] is not None:
                    f.write(f"{detail['abs_difference']:.6f}\t")
                    if detail['relative_difference'] == float('inf'):
                        f.write("无限大(除数为0)\n")
                    else:
                        f.write(f"{detail['relative_difference']*100:.6f}%\n")
                else:
                    f.write("非数值\t非数值\n")
    
    return report_path

def batch_compare_all_files(result_dir, answer_dir, key_column, value_column, n_workers=1, individual_reports_dir=None):
    """
    批量比较所有文件，并可选择生成单个文件的详细报告
    """
    # 查找所有结果文件
    result_files = find_all_result_files(result_dir)
    
    if not result_files:
        print("无任何计算结果")
        return []
    
    print(f"共有 {len(result_files)} 个计算结果")
    
    file_pairs = []
    missing_answers = []
    
    for result_file in result_files:
        answer_file = find_matching_answer_file(result_file, answer_dir)
        if answer_file and answer_file.exists():
            file_pairs.append((result_file, answer_file))
        else:
            stock_code, date_str = extract_stock_code_and_date(result_file)
            missing_answers.append((result_file, stock_code, date_str))
    
    if missing_answers:
        print(f"警告: {len(missing_answers)} 个结果文件没有找到对应的答案文件:")
        for result_file, stock_code, date_str in missing_answers[:5]:
            print(f"  - {Path(result_file).name} (股票: {stock_code}, 日期: {date_str})")
        if len(missing_answers) > 5:
            print(f"  - ... 还有 {len(missing_answers) - 5} 个")
    
    if not file_pairs:
        print("未找到任何匹配的文件对")
        return []
    
    print(f"找到 {len(file_pairs)} 个匹配的文件对")
    
    if n_workers is None:
        n_workers = min(mp.cpu_count(), len(file_pairs))
    
    params = [(result, answer, key_column, value_column) for result, answer in file_pairs]
    with mp.Pool(n_workers) as pool:
        results = pool.starmap(compare_result_with_answer, params)
    
    # 生成单个文件的详细报告
    if individual_reports_dir:
        print(f"生成单个文件的详细报告到目录: {individual_reports_dir}")
        individual_report_paths = []
        for result in results:
            report_path = generate_individual_report(result, individual_reports_dir)
            individual_report_paths.append(report_path)
    
    return results

def generate_detailed_report(comparison_results, output_dir):
    """
    生成总体详细报告
    """
    output_path = Path(output_dir)
    output_path.mkdir(exist_ok=True)
    
    timestamp = datetime.now().strftime("%Y%m%d")
    
    successful_results = [r for r in comparison_results if r.get('status') == 'success']
    error_results = [r for r in comparison_results if r.get('status') == 'error']
    no_common_results = [r for r in comparison_results if r.get('status') == 'no_common_keys']
    
    total_pairs = len(comparison_results)
    match_percentages = [r['match_percentage'] for r in successful_results]
    
    summary = {
        'report_time': datetime.now().isoformat(),
        'total_pairs': total_pairs,
        'successful_comparisons': len(successful_results),
        'failed_comparisons': len(error_results),
        'pairs_with_no_common_keys': len(no_common_results),
        'average_match_percentage': np.mean(match_percentages) if match_percentages else 0,
        'median_match_percentage': np.median(match_percentages) if match_percentages else 0,
        'min_match_percentage': np.min(match_percentages) if match_percentages else 0,
        'max_match_percentage': np.max(match_percentages) if match_percentages else 0,
        'std_match_percentage': np.std(match_percentages) if match_percentages else 0,
        'perfect_matches': sum(1 for r in successful_results if r['match_percentage'] == 100),
        'poor_matches': sum(1 for r in successful_results if r['match_percentage']<100),
    }
    
    # 按股票代码分组统计
    stock_stats = defaultdict(lambda: {
        'count': 0,
        'match_percentages': [],
        'perfect_matches': 0,
        'poor_matches': 0
    })
    
    for result in successful_results:
        stock_code = result['stock_code']
        stock_stats[stock_code]['count'] += 1
        stock_stats[stock_code]['match_percentages'].append(result['match_percentage'])
        if result['match_percentage'] == 100:
            stock_stats[stock_code]['perfect_matches'] += 1
        else:
            stock_stats[stock_code]['poor_matches'] += 1
    
    # 计算每只股票的平均匹配率
    for stock_code, stats in stock_stats.items():
        stats['average_match_percentage'] = np.mean(stats['match_percentages']) if stats['match_percentages'] else 0
    
    summary['stock_statistics'] = dict(stock_stats)
    
    # 生成CSV报告
    csv_report_path = output_path / f"comparison_report_{timestamp}.csv"
    with open(csv_report_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['股票代码', '日期', '结果文件', '答案文件', '匹配率(%)', '键重叠率(%)', 
                            '总键数', '共同键数', '匹配键数', '状态', '错误信息'])
        
        for result in comparison_results:
            writer.writerow([
                result.get('stock_code', 'N/A'),
                result.get('date', 'N/A'),
                Path(result['result_file']).name,
                Path(result['answer_file']).name if 'answer_file' in result else 'N/A',
                f"{result.get('match_percentage', 0):.2f}" if 'match_percentage' in result else 'N/A',
                f"{result.get('key_overlap_percentage', 0):.2f}" if 'key_overlap_percentage' in result else 'N/A',
                result.get('total_keys', 'N/A'),
                result.get('common_keys', 'N/A'),
                result.get('matching_keys', 'N/A'),
                result.get('status', 'N/A'),
                result.get('error', '')
            ])
    
    # 生成JSON格式的详细报告
    json_report_path = output_path / f"detailed_comparison_report_{timestamp}.json"
    with open(json_report_path, 'w', encoding='utf-8') as f:
        # 转换无法序列化的对象
        serializable_results = []
        for result in comparison_results:
            serializable_result = result.copy()
            # 将可能包含非序列化类型的值转换为字符串
            if 'result_only_items' in serializable_result:
                serializable_result['result_only_items'] = {k: str(v) for k, v in serializable_result['result_only_items'].items()}
            if 'answer_only_items' in serializable_result:
                serializable_result['answer_only_items'] = {k: str(v) for k, v in serializable_result['answer_only_items'].items()}
            serializable_result['answer_file'] = serializable_result['answer_file'].name
            serializable_results.append(serializable_result)
        
        json.dump({
            'summary': summary,
            'details': serializable_results
        }, f, ensure_ascii=False, indent=2)
    
    # 生成按股票分组的报告
    stock_report_path = output_path / f"stock_summary_report_{timestamp}.csv"
    with open(stock_report_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['股票代码', '文件数量', '平均匹配率(%)', '完美匹配数量', '优秀匹配数量', '较差匹配数量'])
        
        for stock_code, stats in stock_stats.items():
            writer.writerow([
                stock_code,
                stats['count'],
                f"{stats['average_match_percentage']:.2f}",
                stats['perfect_matches'],
                stats['poor_matches']
            ])
    
    return summary, csv_report_path, json_report_path, stock_report_path

def print_summary_report(summary, comparison_results):
    """
    打印汇总报告
    """
    print("=" * 80)
    print("批量测试汇总报告")
    print("=" * 80)
    print(f"测试时间: {summary['report_time']}")
    print(f"总文件对数量: {summary['total_pairs']}")
    print(f"成功比较: {summary['successful_comparisons']} ({summary['successful_comparisons']/summary['total_pairs']*100:.1f}%)")
    print(f"比较失败: {summary['failed_comparisons']} ({summary['failed_comparisons']/summary['total_pairs']*100:.1f}%)")
    print(f"无共同key的文件对: {summary['pairs_with_no_common_keys']} ({summary['pairs_with_no_common_keys']/summary['total_pairs']*100:.1f}%)")
    print("\n匹配统计:")
    print(f"平均匹配百分比: {summary['average_match_percentage']:.2f}%")
    print(f"中位数匹配百分比: {summary['median_match_percentage']:.2f}%")
    print(f"最小匹配百分比: {summary['min_match_percentage']:.2f}%")
    print(f"最大匹配百分比: {summary['max_match_percentage']:.2f}%")
    print(f"标准差: {summary['std_match_percentage']:.2f}%")
    print(f"完美匹配(100%): {summary['perfect_matches']} 个文件")
    print(f"较差匹配: {summary['poor_matches']} 个文件")
    
    successful_results = [r for r in comparison_results if r.get('status') == 'success']
    if successful_results:
        sorted_results = sorted(successful_results, key=lambda x: x['match_percentage'], reverse=True)
        
        print("\n📈 最佳匹配前5名:")
        for i, result in enumerate(sorted_results[:5], 1):
            print(f"  {i}. {result['stock_code']}_{result['date']}: {result['match_percentage']:.2f}%")
        
        print("\n📉 最差匹配前5名:")
        for i, result in enumerate(sorted_results[-5:], 1):
            print(f"  {i}. {result['stock_code']}_{result['date']}: {result['match_percentage']:.2f}%")
    
    print("=" * 80)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--result_dir', default="factor/result/")
    parser.add_argument('--answer_dir', default="level2_factor/split_path")
    parser.add_argument('--key_column', default='time')
    parser.add_argument('--value_column', default='factor16')
    parser.add_argument('--output_dir', default='test/reports')
    parser.add_argument('--individual_reports_dir', default='test/reports/individual_reports')
    parser.add_argument('--workers', type=int, default=10)
    
    args = parser.parse_args()
    
    print(f"结果文件路径: {args.result_dir}")
    print(f"答案文件路径: {args.answer_dir}")
    print(f"输出目录: {args.output_dir}")
    print(f"单个报告目录: {args.individual_reports_dir}")
    
    # 批量比较所有文件
    results = batch_compare_all_files(
        args.result_dir, 
        args.answer_dir, 
        args.key_column, 
        args.value_column, 
        args.workers,
        args.individual_reports_dir
    )
    
    if not results:
        print("没有结果可处理")
        return
    
    summary, csv_report, json_report, stock_report = generate_detailed_report(results, args.output_dir)
    print_summary_report(summary, results)

if __name__ == "__main__":
    main()
