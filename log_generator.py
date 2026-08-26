import os
import sys
import argparse
import adata
import akshare as ak

adata.login(os.environ["ADATA_USERNAME"], os.environ["ADATA_PASSWORD"])


def parse_args():
    parser = argparse.ArgumentParser(description="A股日志数据导出工具")
    parser.add_argument("--date", default="2026-02-27", help="交易日期 (YYYY-MM-DD)")
    parser.add_argument("--market", choices=["all", "sh", "sz"], default="all",
                        help="市场: all=全部, sh=仅上海, sz=仅深圳")
    parser.add_argument("--limit", type=int, default=0,
                        help="最多下载几只（0=不限制）")
    parser.add_argument("--skip-existing", action="store_true",
                        help="跳过已存在的股票")
    return parser.parse_args()


def main():
    args = parse_args()
    date = args.date
    out_dir = "./adata_logs"
    os.makedirs(out_dir, exist_ok=True)

    df_stocks = ak.stock_info_a_code_name()
    sym_list = []
    for code in df_stocks["code"]:
        if code.startswith("6"):
            if args.market in ("all", "sh"):
                sym_list.append(f"{code}.SH")
        elif code.startswith("0") or code.startswith("3"):
            if args.market in ("all", "sz"):
                sym_list.append(f"{code}.SZ")

    if args.limit > 0:
        sym_list = sym_list[:args.limit]

    print(f"共 {len(sym_list)} 只股票（market={args.market}），开始导出 {date} 的数据...")

    success, fail = 0, 0
    for i, sym in enumerate(sym_list):
        base_name = f"{sym}_{date}"
        if args.skip_existing and os.path.exists(f"{out_dir}/cstick_{base_name}.csv"):
            print(f"[{i+1}/{len(sym_list)}] {sym} 已存在，跳过")
            continue

        print(f"[{i+1}/{len(sym_list)}] 正在处理 {sym} ...", end=" ", flush=True)
        try:
            cstick = adata.get_data("cstick", date, date, [sym])
            csord = adata.get_data("csord", date, date, [sym])
            cstra = adata.get_data("cstra", date, date, [sym])
            csbar1d = adata.get_data("csbar_1d", date, date, [sym])

            cstick.to_csv(f"{out_dir}/cstick_{base_name}.csv", index=False)
            csord.to_csv(f"{out_dir}/csord_{base_name}.csv", index=False)
            cstra.to_csv(f"{out_dir}/cstra_{base_name}.csv", index=False)
            csbar1d.to_csv(f"{out_dir}/csbar1d_{base_name}.csv", index=False)
            print("✅")
            success += 1
        except Exception as e:
            print(f"❌ {e}")
            fail += 1

    print(f"\n完成！成功 {success} 只，失败 {fail} 只")


if __name__ == "__main__":
    main()