import adata
adata.login("linzhuoyu", 'Lzy160115?')
#etf list
etf_list = [
    "510050.SH",
    "510300.SH",
    "510500.SH",
    "159915.SZ",
    "159919.SZ"
]
date = "2025-11-17" 
sym = "510050.SH"
base_name = f"{sym}_{date}"
cstick = adata.get_data("cstick", date, date, [sym])
csord = adata.get_data("csord", date, date, [sym])
cstra = adata.get_data("cstra", date, date, [sym])
csbar1d = adata.get_data("csbar_1d", date, date, [sym])

cstick.to_csv(f"cstick_{base_name}.csv", index=False)
csord.to_csv(f"csord_{base_name}.csv", index=False)
cstra.to_csv(f"cstra_{base_name}.csv", index=False)
csbar1d.to_csv(f"csbar1d_{base_name}.csv", index=False)