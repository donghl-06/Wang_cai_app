
import aqdatac
import pandas as pd
from datetime import datetime
import os

if __name__ == "__main__":
    # 定义要处理的股票代码和日期
    # "002466.SZ", "2023-12-11"
    # 股票号: 000709.SZ 集合竞价成交价格：0, 成交量：0, 日期：2022-01-05
    stocks_dates = [("002466.SZ", "2023-12-11")]

    # 从环境变量中获取用户名和密码
    username = os.getenv('MY_USER_NAME')
    password = os.getenv('MY_PASSWORD')
    start_time_login = datetime.now()
    aqdatac.login(username, password)
    end_time_login = datetime.now()
    print(f"登录耗时: {end_time_login - start_time_login}")
    
    # 处理数据
    for sym, date_str in stocks_dates:
        current_date = datetime.strptime(date_str, '%Y-%m-%d')

        # 获取数据
        start_time_data = datetime.now()
        cstr_data: pd.DataFrame = aqdatac.get_data('cstra', start_date=current_date, end_date=current_date, sym_list=sym)
        csord_data: pd.DataFrame = aqdatac.get_data('csord', start_date=current_date, end_date=current_date, sym_list=sym)
        cstick_data: pd.DataFrame = aqdatac.get_data('cstick', start_date=current_date, end_date=current_date, sym_list=sym)
        # 将tradebsflag和exectype从bytes格式转换为int格式
        cstr_data['tradebsflag'] = cstr_data['tradebsflag'].apply(lambda x: int(x) if isinstance(x, (int, float)) else int(x.decode()) if isinstance(x, bytes) and x.strip() else 0)
        cstr_data['exectype'] = cstr_data['exectype'].apply(lambda x: int(x) if isinstance(x, (int, float)) else int(x.decode()) if isinstance(x, bytes) and x.strip() else 0)
        print(cstr_data)
        # 储存成csv 
        csord_data.to_csv(f"csord_{sym}_{date_str}.csv", index=False)
        cstr_data.to_csv(f"cstra_{sym}_{date_str}.csv", index=False)
        cstick_data.to_csv(f"cstick_{sym}_{date_str}.csv", index=False)
