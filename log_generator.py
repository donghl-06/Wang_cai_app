import adata
adata.login("linzhuoyu", 'Lzy160115?')


date = "2025-12-15" 
sym = "002929.SZ"
base_name = f"{sym}_{date}"
cstick = adata.get_data("cstick", date, date, [sym])
csord = adata.get_data("csord", date, date, [sym])
cstra = adata.get_data("cstra", date, date, [sym])
csbar1d = adata.get_data("csbar_1d", date, date, [sym])

cstick.to_csv(f"cstick_{base_name}.csv", index=False)
csord.to_csv(f"csord_{base_name}.csv", index=False)
cstra.to_csv(f"cstra_{base_name}.csv", index=False)
csbar1d.to_csv(f"csbar1d_{base_name}.csv", index=False)