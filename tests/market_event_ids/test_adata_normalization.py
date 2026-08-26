from __future__ import annotations

import io

import pandas as pd

from wangcai_syn.utils import create_symbol_data, revert_sh_order


def test_revert_sh_order_uses_only_trades_through_close_and_groups_by_channel():
    symbol = "600000.SH"
    orders = pd.DataFrame(
        [
            {
                "date": "2025-01-02",
                "time": "0 days 09:30:00.000000",
                "sym": symbol,
                "price": 9.9,
                "size": 10,
                "side": -1,
                "ordertype": 2,
                "orderid": 999,
                "channelno": 1,
                "seqno": 1,
                "bizindex": 1,
                "updatetime": "2025-01-02 09:30:00.000",
            }
        ]
    )
    trades = pd.DataFrame(
        [
            # 同一个 raw id 在两个通道中是两张不同订单；14:57 后仍属于收盘阶段。
            {"date": "2025-01-02", "time": "0 days 14:58:00.000000", "sym": symbol,
             "price": 10.0, "size": 100, "bidorderid": 100, "askorderid": 50,
             "tradeid": 1, "exectype": "b'1'", "tradebsflag": "b'B'",
             "channelno": 11, "bizindex": 11, "updatetime": ""},
            {"date": "2025-01-02", "time": "0 days 14:59:00.000000", "sym": symbol,
             "price": 10.1, "size": 200, "bidorderid": 100, "askorderid": 50,
             "tradeid": 2, "exectype": b"1", "tradebsflag": b"B",
             "channelno": 22, "bizindex": 22, "updatetime": ""},
            # 撤单不能被当作成交量还原进原始订单。
            {"date": "2025-01-02", "time": "0 days 14:59:30.000000", "sym": symbol,
             "price": 0.0, "size": 999, "bidorderid": 100, "askorderid": 0,
             "tradeid": 3, "exectype": "b'2'", "tradebsflag": "b'D'",
             "channelno": 11, "bizindex": 23, "updatetime": ""},
        ]
    )

    restored = revert_sh_order(orders, trades, symbol)
    rows = restored[restored["orderid"] == 100].sort_values("channelno")

    assert rows[["channelno", "size"]].to_records(index=False).tolist() == [
        (11, 100.0),
        (22, 200.0),
    ]


def test_create_symbol_data_normalizes_bytes_and_bytes_literal_fields():
    symbol = "000001.SZ"
    trade_df = pd.DataFrame(
        [{"exectype": "b'1'", "tradebsflag": b"B"}]
    )

    data = create_symbol_data(
        symbol,
        pd.DataFrame(),
        pd.DataFrame(),
        trade_df,
        pd.DataFrame(),
    )
    normalized = pd.read_csv(io.StringIO(data.trade_csv), dtype=str)

    assert normalized.loc[0, "exectype"] == "1"
    assert normalized.loc[0, "tradebsflag"] == "B"
