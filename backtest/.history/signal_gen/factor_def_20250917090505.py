"""所有因子计算单元的逻辑
"""
from signal_gen.factor_calc import *
from signal_gen.middle_calc import *
from signal_gen import factor_calc
from signal_gen.factor_engine import FactorUnit


level1_unit = FactorUnit(
    factor_names = [
        'asize1_delt',
        'bsize1_delt',
        'imb@depth=1@slot=1',
        'imb@depth=5@slot=1',
        'imb@depth=10@slot=1'],
    factor_func=[
        (factor_calc.asize1_delt,{"delta_t":1}),
        (factor_calc.bsize1_delt,{"delta_t":1}),
        (factor_calc.imb,{"depth":1,"slot":1}),
        (factor_calc.imb,{"depth":5,"slot":1}),
        (factor_calc.imb,{"depth":10,"slot":1}),
    ],
    middle_value_names={
        "askprice1":[{'Askprice':0},None, 'raw'],
        "bidprice1":[{'Bidprice':0},None, 'raw'],
        "askvolume":[{"Askvolume":None},None,'raw'],
        "bidvolume":[{"Bidvolume":None},None,'raw'],
    },
    factor_type='snapshot',
    max_span=3,
    pop_condition='length'
)

mid_unit = FactorUnit(
    factor_names = ['mid'],
    factor_func=[
        (factor_calc.mid,{}),
    ],
    middle_value_names={
        "askprice1":[{'Askprice':0},None, 'raw'],
        "bidprice1":[{'Bidprice':0},None, 'raw'],
    },
    factor_type='snapshot',
    max_span=1,
    pop_condition='length'
)

trade_unit = FactorUnit(
    factor_names = ['factor16'],
    factor_func = [
        (factor_calc.factor16, {}),
    ],
    middle_value_names={
        "active_sell_size":[{"Volume":None,"Price":None,"buy_order_id":None,"sell_order_id":None},calc_active_sell_size,'accumulated'],
    },
    factor_type='trade',
    max_span=3000, # 3s
    pop_condition='time',
)

order_unit = FactorUnit(
    factor_names = ['factor19','factor20'],
    factor_func = [
        (factor_calc.factor19, {}),
        (factor_calc.factor20, {}),
    ],
    middle_value_names={
        "buy_cancel_volume":[{"volume":None,"price":None,"Side":None,'OrderKind':None},calc_buyin_cancel,'accumulated'],
        "sell_cancel_volume":[{"volume":None,"price":None,"Side":None,'OrderKind':None},calc_sellout_cancel,'accumulated'],
    },
    factor_type='order',
    max_span=3000, # 3s
    pop_condition='time',
)