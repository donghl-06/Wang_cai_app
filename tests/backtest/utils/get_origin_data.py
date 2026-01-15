import pandas as pd
from datetime import datetime

code = "600085.SH"
date = '2024-01-10'
data_path = f"/mlquant/data/etf_con_data/cstra/{code}.HDF5"

start_time = datetime.now()

origin_data = pd.read_hdf(data_path)
origin_data = origin_data[origin_data.sym==code]
origin_data = origin_data[origin_data.date==date]
# origin_data = origin_data[(origin_data.time>'10:45:33')&(origin_data.time<'10:47:00')]
origin_data = origin_data[origin_data.bidorderid<origin_data.askorderid]
origin_data = origin_data[(origin_data.bidorderid>0)&(origin_data.askorderid>0)]
origin_data = origin_data.loc[:, ['time','sym', 'size','price','bidorderid','askorderid']]
origin_data['contribution'] = origin_data['price'] *origin_data['size']
origin_data.to_csv(f"{code}_{date}_origin_data.csv",index=False)

print(datetime.now()-start_time)
