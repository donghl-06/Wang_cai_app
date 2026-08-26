"""Regression tests for isolation between market order identities."""

from __future__ import annotations

from conftest import MarketEventRecorder, order_row, trade_row
from wangcai_syn import make_order


class NumericUserLocalIdRecorder(MarketEventRecorder):
    """Submit a numeric user local ID that equals a historical market ID."""

    def __init__(self, symbol: str, colliding_id: int) -> None:
        super().__init__()
        self.symbol = symbol
        self.colliding_id = colliding_id
        self.sent = False
        self.user_order_ids: list[str] = []

    def onOrderEvent(self, order):
        super().onOrderEvent(order)
        if not self.sent and order.OrderNo == self.colliding_id:
            self.sent = True
            return [
                make_order(
                    Broker="",
                    Account=self.getStrategyId(),
                    Exchange=1,
                    Instrument=self.symbol,
                    OrderLocalID=str(self.colliding_id),
                    OrderType=0,
                    Direction=1,
                    Price=95_000,
                    Volume=100,
                )
            ]
        return []

    def onOrderCallback(self, callback) -> None:
        self.user_order_ids.append(callback.orderlocalid)


def _calculated_trade_keys(timeline):
    return {
        (event["ChannelNo"], event["BuyNo"], event["SellNo"])
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    }


def test_numeric_user_local_id_cannot_overwrite_market_cancel_mapping(run_events):
    symbol = "000003.SZ"
    channel = 101
    colliding_market_id = 410_001
    resting_sell_id = 410_002
    later_buy_id = 410_003
    cancel_trade_id = 510_001
    recorder = NumericUserLocalIdRecorder(symbol, colliding_market_id)

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                "09:30:00.001000",
                price=10.00,
                size=100,
                side=1,
                order_id=colliding_market_id,
                channel=channel,
                sequence=1,
            ),
            # If the cancel incorrectly targets the numeric user order, this
            # sell consumes the still-live historical buy.  Correct behavior
            # leaves this sell resting for later_buy_id.
            order_row(
                symbol,
                "09:30:00.003000",
                price=10.00,
                size=100,
                side=-1,
                order_id=resting_sell_id,
                channel=channel,
                sequence=3,
            ),
            order_row(
                symbol,
                "09:30:00.004000",
                price=10.00,
                size=100,
                side=1,
                order_id=later_buy_id,
                channel=channel,
                sequence=4,
            ),
        ],
        [
            trade_row(
                symbol,
                "09:30:00.002000",
                bid_order_id=colliding_market_id,
                ask_order_id=0,
                trade_id=cancel_trade_id,
                trade_bs_flag="B",
                channel=channel,
                sequence=2,
            )
        ],
        recorder=recorder,
    )

    assert str(colliding_market_id) in recorder.user_order_ids
    assert _calculated_trade_keys(timeline) == {
        (channel, later_buy_id, resting_sell_id)
    }


def test_same_raw_ids_on_two_channels_keep_distinct_trade_identity(run_events):
    symbol = "000004.SZ"
    sell_id = 420_001
    buy_id = 420_002
    channel_a = 111
    channel_b = 222

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                "09:30:00.001000",
                price=10.00,
                size=100,
                side=-1,
                order_id=sell_id,
                channel=channel_a,
                sequence=1,
            ),
            order_row(
                symbol,
                "09:30:00.002000",
                price=10.00,
                size=100,
                side=1,
                order_id=buy_id,
                channel=channel_a,
                sequence=2,
            ),
            order_row(
                symbol,
                "09:30:00.003000",
                price=10.00,
                size=100,
                side=-1,
                order_id=sell_id,
                channel=channel_b,
                sequence=3,
            ),
            order_row(
                symbol,
                "09:30:00.004000",
                price=10.00,
                size=100,
                side=1,
                order_id=buy_id,
                channel=channel_b,
                sequence=4,
            ),
        ],
    )

    assert _calculated_trade_keys(timeline) == {
        (channel_a, buy_id, sell_id),
        (channel_b, buy_id, sell_id),
    }
    published_order_keys = {
        (event["ChannelNo"], event["OrderNo"])
        for event in timeline
        if event["kind"] == "order"
    }
    assert {
        (channel_a, sell_id),
        (channel_a, buy_id),
        (channel_b, sell_id),
        (channel_b, buy_id),
    } <= published_order_keys


def test_cancel_uses_channel_when_raw_order_ids_are_reused(run_events):
    symbol = "000005.SZ"
    reused_buy_id = 430_001
    resting_sell_id = 430_002
    later_buy_id = 430_003
    channel_a = 311
    channel_b = 322

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                "09:30:00.001000",
                price=10.00,
                size=100,
                side=1,
                order_id=reused_buy_id,
                channel=channel_a,
                sequence=1,
            ),
            order_row(
                symbol,
                "09:30:00.002000",
                price=9.80,
                size=100,
                side=1,
                order_id=reused_buy_id,
                channel=channel_b,
                sequence=2,
            ),
            order_row(
                symbol,
                "09:30:00.004000",
                price=10.00,
                size=100,
                side=-1,
                order_id=resting_sell_id,
                channel=channel_a,
                sequence=4,
            ),
            order_row(
                symbol,
                "09:30:00.005000",
                price=10.00,
                size=100,
                side=1,
                order_id=later_buy_id,
                channel=channel_a,
                sequence=5,
            ),
        ],
        [
            trade_row(
                symbol,
                "09:30:00.003000",
                bid_order_id=reused_buy_id,
                ask_order_id=0,
                trade_id=530_001,
                trade_bs_flag="B",
                channel=channel_a,
                sequence=3,
            )
        ],
    )

    assert _calculated_trade_keys(timeline) == {
        (channel_a, later_buy_id, resting_sell_id)
    }


def _mixed_symbol_dataset(symbol: str):
    if symbol.endswith(".SZ"):
        # SZ same-time ordering uses raw order ID: sell 610001 before buy 610002.
        sell_id, buy_id = 610_001, 610_002
        sell_sequence, buy_sequence = 2, 1
    else:
        # SH same-time ordering uses BizIndex: sell sequence 1 before buy 2,
        # despite its raw order ID being numerically larger.
        sell_id, buy_id = 620_002, 620_001
        sell_sequence, buy_sequence = 1, 2
    orders = [
        order_row(
            symbol,
            "09:30:00.001000",
            price=10.00,
            size=100,
            side=-1,
            order_id=sell_id,
            channel=2012,
            sequence=sell_sequence,
        ),
        order_row(
            symbol,
            "09:30:00.001000",
            price=10.00,
            size=100,
            side=1,
            order_id=buy_id,
            channel=2012,
            sequence=buy_sequence,
        ),
    ]
    return (symbol, orders, []), sell_id, buy_id


def _symbol_signature(timeline, symbol):
    signature = []
    for event in timeline:
        if event["Instrument"] != symbol:
            continue
        if event["kind"] == "order":
            signature.append(("order", event["ChannelNo"], event["OrderNo"]))
        elif event["ExecType"] == "1":
            signature.append(
                ("trade", event["ChannelNo"], event["BuyNo"], event["SellNo"])
            )
    return signature


def _symbol_order_signature(timeline, symbol):
    return [
        ("order", event["ChannelNo"], event["OrderNo"])
        for event in timeline
        if event["Instrument"] == symbol and event["kind"] == "order"
    ]


def test_mixed_sh_sz_results_do_not_depend_on_symbol_load_order(run_multi_events):
    sh_dataset, sh_sell_id, sh_buy_id = _mixed_symbol_dataset("600001.SH")
    sz_dataset, sz_sell_id, sz_buy_id = _mixed_symbol_dataset("000006.SZ")
    expected = {
        "600001.SH": [
            ("order", 2012, sh_sell_id),
            ("order", 2012, sh_buy_id),
            ("trade", 2012, sh_buy_id, sh_sell_id),
        ],
        "000006.SZ": [
            ("order", 2012, sz_sell_id),
            ("order", 2012, sz_buy_id),
            ("trade", 2012, sz_buy_id, sz_sell_id),
        ],
    }

    sh_then_sz = run_multi_events([sh_dataset, sz_dataset])
    sz_then_sh = run_multi_events([sz_dataset, sh_dataset])

    # Check market-specific same-time ordering and callback coverage first.  On
    # the old global Event::is_SZ implementation, whichever market is loaded
    # last changes whether the fully-filled incoming order is published.
    for symbol, expected_signature in expected.items():
        expected_orders = expected_signature[:2]
        assert _symbol_order_signature(sh_then_sz, symbol) == expected_orders
        assert _symbol_order_signature(sz_then_sh, symbol) == expected_orders

    for symbol, expected_signature in expected.items():
        assert _symbol_signature(sh_then_sz, symbol) == expected_signature
        assert _symbol_signature(sz_then_sh, symbol) == expected_signature
