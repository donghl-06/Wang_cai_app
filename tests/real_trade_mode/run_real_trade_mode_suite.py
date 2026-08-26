"""
运行真实成交替代模式（RT）覆盖测试套件

说明：
- 使用同一份真实行情数据，按场景分开跑，避免场景间互相干扰
- 每个场景都给出 PASS/FAIL 结论，最后汇总
"""

import os
import sys
from pathlib import Path
from typing import Dict, List, Tuple

import pandas as pd

from wangcai_syn import run_backtest
from test_real_trade_mode_suite import (
    RtStrictBridgeScenario,
    RtOffBaselineScenario,
    RtPassiveCancelBothSidesScenario,
    RtPassiveFillTimingScenario,
)


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../../new_log"
OUTPUT_DIR = "./output_suite"
# ============================


def load_data(symbol: str, date: str, data_dir: str):
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"数据加载完成: {symbol}")
    print(f"  Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def run_one_case(
    case_name: str,
    strategy,
    data,
    *,
    rt_mode: bool,
) -> Tuple[bool, List[str]]:
    print(f"\n{'=' * 80}")
    print(f"[CASE] {case_name}")
    print(f"  RT模式: {'开启' if rt_mode else '关闭'}")
    print(f"{'=' * 80}")

    ok = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{case_name}",
        real_trade_match_mode=rt_mode,
    )

    if not ok:
        return False, [f"[FAIL] run_backtest 返回 False: {case_name}"]

    passed, messages = strategy.validate()
    return passed, messages


def main():
    print("\n真实成交替代模式（RT）覆盖测试套件")
    print(f"标的: {SYMBOL}  日期: {DATE}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    # 场景定义：名称、策略、是否开启RT
    cases = [
        (
            "case_01_rt_strict_bridge",
            RtStrictBridgeScenario(account="rt_case01"),
            True,
        ),
        (
            "case_02_rt_off_baseline",
            RtOffBaselineScenario(account="rt_case02"),
            False,
        ),
        (
            "case_03_rt_passive_cancel_both_sides",
            RtPassiveCancelBothSidesScenario(account="rt_case03"),
            True,
        ),
        (
            "case_04_rt_passive_fill_timing",
            RtPassiveFillTimingScenario(account="rt_case04"),
            True,
        ),
    ]

    all_results: Dict[str, bool] = {}
    details: Dict[str, List[str]] = {}

    for case_name, strategy, rt_on in cases:
        passed, messages = run_one_case(case_name, strategy, data, rt_mode=rt_on)
        all_results[case_name] = passed
        details[case_name] = messages
        for msg in messages:
            print(" ", msg)

    print(f"\n{'=' * 80}")
    print("RT覆盖测试汇总")
    print(f"{'=' * 80}")
    fail_count = 0
    for case_name, passed in all_results.items():
        status = "PASS" if passed else "FAIL"
        print(f"  {status}  {case_name}")
        if not passed:
            fail_count += 1
            for msg in details[case_name]:
                print(f"    - {msg}")

    print(f"{'=' * 80}")
    if fail_count == 0:
        print("全部场景通过")
        return 0
    print(f"失败场景数: {fail_count}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
