import pandas as pd
from datetime import datetime

from config import tickerList

xtech_factor_path = f"/mlquant/factor/etf_con_factor/data_split1/2024_01.pqt"
new_factor_path = f"level2_factor/answer_path/603392.SH/factor16.csv"

origin_data = pd.read_parquet(xtech_factor_path)
def get_one_stock_answer(origin_data,code):
    start_time = datetime.now()
    new_factor = pd.read_csv(new_factor_path)

    changed_origin_data = origin_data.sort_values(by='date_time', ascending=True)
    changed_origin_data = changed_origin_data[changed_origin_data.code==code]
    changed_origin_data = changed_origin_data.loc[:, ['date_time','mid']]

    new_factor = new_factor.sort_values(by='date_time', ascending=True)
    new_factor = new_factor.loc[:, ['date_time','factor16']]

    result_answer = pd.merge(changed_origin_data,new_factor,on='date_time')

    result_answer.to_csv(f"utils/factor_answer_{code}.csv",index=False)
    print(datetime.now()-start_time)

if __name__=='__main__':
    for code in tickerList:
        get_one_stock_answer(origin_data,code)