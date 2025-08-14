'''
Author: chenlisen
Date: 2025-08-13 06:26:20
LastEditTime: 2025-08-13 06:29:07
FilePath: /wangcai_cpp/get_data.py
'''
import aqdatac as aq

if __name__ == "__main__":
    sym, date = "600196.SH", "2024-02-01"
    aq.login("fuzheyuan", "fuzheyuan")
    order_df = aq.get_data("csord", date, date, sym)
    trade_df = aq.get_data("cstra", date, date, sym)
    tick_df = aq.get_data("cstick", date, date, sym)
    
    order_df.to_csv(f"logs/csord_{sym}_{date}.csv", index=False)
    trade_df.to_csv(f"logs/cstra_{sym}_{date}.csv", index=False)
    tick_df.to_csv(f"logs/cstick_{sym}_{date}.csv", index=False)
