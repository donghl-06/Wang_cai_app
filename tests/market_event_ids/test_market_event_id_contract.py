"""Regression tests for the AData order/trade/cancel ID relationship."""

from __future__ import annotations

import pytest

from conftest import MarketEventRecorder, order_row, trade_row


def _assert_calculated_trades_reference_prior_orders(timeline):
    """Validate the public identity contract across the complete callback stream."""
    published_orders = {}
    calculated_trades = []

    for event in timeline:
        if event["kind"] == "order":
            published_orders[
                (event["Instrument"], event["ChannelNo"], event["OrderNo"])
            ] = event["Side"]
            continue

        if event["ExecType"] != "1":
            continue

        calculated_trades.append(event)
        assert event["BuyNo"] != 0
        assert event["SellNo"] != 0

        buy_key = (event["Instrument"], event["ChannelNo"], event["BuyNo"])
        sell_key = (event["Instrument"], event["ChannelNo"], event["SellNo"])
        assert published_orders.get(buy_key) == "1", (
            "BuyNo must reference a previously published buy order in the same channel"
        )
        assert published_orders.get(sell_key) == "2", (
            "SellNo must reference a previously published sell order in the same channel"
        )

    assert calculated_trades, "the fixture must exercise at least one calculated trade"


def test_complete_timeline_never_exposes_internal_order_ids(run_events):
    symbol = "000009.SZ"
    channel_a = 731
    channel_b = 732
    sell_a = 8_100_000_101
    buy_a = 8_100_000_102
    buy_b = 8_100_000_201
    sell_b = 8_100_000_202

    timeline = run_events(
        symbol,
        [
            order_row(symbol, "09:30:00.001000", price=10.00, size=100, side=-1,
                      order_id=sell_a, channel=channel_a, sequence=1),
            order_row(symbol, "09:30:00.002000", price=10.00, size=100, side=1,
                      order_id=buy_a, channel=channel_a, sequence=2),
            order_row(symbol, "09:30:00.003000", price=10.00, size=120, side=1,
                      order_id=buy_b, channel=channel_b, sequence=3),
            order_row(symbol, "09:30:00.004000", price=10.00, size=120, side=-1,
                      order_id=sell_b, channel=channel_b, sequence=4),
        ],
    )

    _assert_calculated_trades_reference_prior_orders(timeline)
    assert [
        (event["ChannelNo"], event["BuyNo"], event["SellNo"])
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    ] == [
        (channel_a, buy_a, sell_a),
        (channel_b, buy_b, sell_b),
    ]


@pytest.mark.parametrize(
    ("sell_id", "buy_id"),
    [
        pytest.param(1_000_000_101, 1_000_000_202, id="ordinary-adata-ids"),
        pytest.param(5_000_000_101, 5_000_000_202, id="ids-above-32-bit"),
        pytest.param(
            9_007_199_254_740_101,
            9_007_199_254_740_202,
            id="ids-above-javascript-safe-integer",
        ),
    ],
)
def test_sz_calculated_trade_uses_original_order_ids(run_events, sell_id, buy_id):
    symbol = "000001.SZ"

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                "09:30:00.001000",
                price=10.00,
                size=400,
                side=-1,
                order_id=sell_id,
                sequence=1,
            ),
            order_row(
                symbol,
                "09:30:00.002000",
                price=10.00,
                size=400,
                side=1,
                order_id=buy_id,
                sequence=2,
            ),
        ],
    )

    calculated_trades = [
        event
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    ]
    assert calculated_trades, "the two crossing SZ orders must calculate a trade"
    assert {
        (event["BuyNo"], event["SellNo"]) for event in calculated_trades
    } == {(buy_id, sell_id)}

    published_order_ids = {
        event["OrderNo"] for event in timeline if event["kind"] == "order"
    }
    assert {buy_id, sell_id} <= published_order_ids


def test_sh_crossing_order_is_published_before_trade_with_original_volume(run_events):
    symbol = "600000.SH"
    sell_id = 600_000_101
    buy_id = 600_000_202
    submitted_buy_volume = 1_000

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                "09:30:00.001000",
                price=10.00,
                size=400,
                side=-1,
                order_id=sell_id,
                sequence=1,
            ),
            # This order immediately trades 400 and leaves 600.  Its market
            # order callback still describes the original 1,000-share AData
            # order and must precede the calculated trade callback.
            order_row(
                symbol,
                "09:30:00.002000",
                price=10.00,
                size=submitted_buy_volume,
                side=1,
                order_id=buy_id,
                sequence=2,
            ),
        ],
    )

    buy_order_positions = [
        index
        for index, event in enumerate(timeline)
        if event["kind"] == "order" and event["OrderNo"] == buy_id
    ]
    related_trade_positions = [
        index
        for index, event in enumerate(timeline)
        if event["kind"] == "trade"
        and event["ExecType"] == "1"
        and (event["BuyNo"], event["SellNo"]) == (buy_id, sell_id)
    ]

    assert buy_order_positions, "an immediately crossing SH order must still be published"
    assert related_trade_positions, "the SH crossing orders must calculate a trade"
    assert buy_order_positions[0] < related_trade_positions[0]
    assert timeline[buy_order_positions[0]]["Volume"] == submitted_buy_volume


def test_buy_and_sell_cancels_reference_exactly_one_original_order(run_events):
    symbol = "000002.SZ"
    buy_id = 700_000_101
    sell_id = 700_000_202
    buy_cancel_trade_id = 800_000_301
    sell_cancel_trade_id = 800_000_302

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                "09:30:00.001000",
                price=9.90,
                size=100,
                side=1,
                order_id=buy_id,
                sequence=1,
            ),
            order_row(
                symbol,
                "09:30:00.002000",
                price=10.10,
                size=100,
                side=-1,
                order_id=sell_id,
                sequence=2,
            ),
        ],
        [
            trade_row(
                symbol,
                "09:30:00.003000",
                bid_order_id=buy_id,
                ask_order_id=0,
                trade_id=buy_cancel_trade_id,
                trade_bs_flag="B",
                sequence=3,
            ),
            trade_row(
                symbol,
                "09:30:00.004000",
                bid_order_id=0,
                ask_order_id=sell_id,
                trade_id=sell_cancel_trade_id,
                trade_bs_flag="S",
                sequence=4,
            ),
        ],
    )

    cancels = [
        event
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "2"
    ]
    assert {
        (event["TradeIndex"], event["BuyNo"], event["SellNo"])
        for event in cancels
    } == {
        (buy_cancel_trade_id, buy_id, 0),
        (sell_cancel_trade_id, 0, sell_id),
    }
    assert all(bool(event["BuyNo"]) ^ bool(event["SellNo"]) for event in cancels)


def test_sz_same_millisecond_order_precedes_its_cancel(run_events):
    """SZ uses the shared csord.orderid/cstra.tradeid stream inside one millisecond."""
    symbol = "300827.SZ"
    channel = 2012
    order_id = 57_196
    cancel_trade_id = 57_350
    timestamp = "09:15:00.240000"

    timeline = run_events(
        symbol,
        [
            order_row(
                symbol,
                timestamp,
                price=10.00,
                size=100,
                side=1,
                order_id=order_id,
                channel=channel,
                sequence=1,
            )
        ],
        [
            trade_row(
                symbol,
                timestamp,
                bid_order_id=order_id,
                ask_order_id=0,
                trade_id=cancel_trade_id,
                trade_bs_flag="B",
                channel=channel,
                sequence=2,
            )
        ],
    )

    public_events = [
        event
        for event in timeline
        if event["kind"] == "order" or event["ExecType"] == "2"
    ]
    assert [(event["kind"], event.get("OrderNo"), event.get("BuyNo"))
            for event in public_events] == [
        ("order", order_id, None),
        ("trade", None, order_id),
    ]
    assert public_events[1]["SellNo"] == 0
    assert public_events[1]["TradeIndex"] == cancel_trade_id


@pytest.mark.parametrize(
    ("encoded_exec_type", "encoded_buy_flag", "encoded_sell_flag"),
    [
        pytest.param("b'2'", "b'B'", "b'S'", id="bytes-literal-strings"),
        pytest.param(b"2", b"B", b"S", id="bytes-scalars"),
    ],
)
def test_cstra_cancel_preserves_64_bit_ids_and_normalizes_encoded_scalars(
    run_events,
    encoded_exec_type,
    encoded_buy_flag,
    encoded_sell_flag,
):
    symbol = "000010.SZ"
    channel = 733
    buy_id = 5_100_000_101
    sell_id = 5_100_000_202
    buy_cancel_trade_id = 6_100_000_301
    sell_cancel_trade_id = 6_100_000_302

    assert min(
        buy_id,
        sell_id,
        buy_cancel_trade_id,
        sell_cancel_trade_id,
    ) > 2**31

    timeline = run_events(
        symbol,
        [
            order_row(symbol, "09:30:00.001000", price=9.90, size=100, side=1,
                      order_id=buy_id, channel=channel, sequence=1),
            order_row(symbol, "09:30:00.002000", price=10.10, size=100, side=-1,
                      order_id=sell_id, channel=channel, sequence=2),
        ],
        [
            trade_row(
                symbol,
                "09:30:00.003000",
                bid_order_id=buy_id,
                ask_order_id=0,
                trade_id=buy_cancel_trade_id,
                trade_bs_flag=encoded_buy_flag,
                channel=channel,
                sequence=3,
                exec_type=encoded_exec_type,
            ),
            trade_row(
                symbol,
                "09:30:00.004000",
                bid_order_id=0,
                ask_order_id=sell_id,
                trade_id=sell_cancel_trade_id,
                trade_bs_flag=encoded_sell_flag,
                channel=channel,
                sequence=4,
                exec_type=encoded_exec_type,
            ),
        ],
    )

    assert {
        (
            event["TradeIndex"],
            event["BuyNo"],
            event["SellNo"],
            event["TradeBSFlag"],
        )
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "2"
    } == {
        (buy_cancel_trade_id, buy_id, 0, "B"),
        (sell_cancel_trade_id, 0, sell_id, "S"),
    }


@pytest.mark.parametrize(
    ("order_side", "cancel_field", "trade_bs_flag"),
    [
        pytest.param(1, "sell", "S", id="SellNo-points-to-buy-order"),
        pytest.param(-1, "buy", "B", id="BuyNo-points-to-sell-order"),
    ],
)
def test_cancel_direction_mismatch_fails_before_trade_callback(
    run_events,
    order_side,
    cancel_field,
    trade_bs_flag,
):
    symbol = "000012.SZ"
    channel = 735
    order_id = 8_500_000_101
    recorder = MarketEventRecorder()

    with pytest.raises(RuntimeError, match="方向"):
        run_events(
            symbol,
            [
                order_row(
                    symbol,
                    "09:30:00.001000",
                    price=10.00,
                    size=100,
                    side=order_side,
                    order_id=order_id,
                    channel=channel,
                    sequence=1,
                )
            ],
            [
                trade_row(
                    symbol,
                    "09:30:00.002000",
                    bid_order_id=order_id if cancel_field == "buy" else 0,
                    ask_order_id=order_id if cancel_field == "sell" else 0,
                    trade_id=8_600_000_301,
                    trade_bs_flag=trade_bs_flag,
                    channel=channel,
                    sequence=2,
                )
            ],
            recorder=recorder,
        )

    assert [
        (event["kind"], event["ChannelNo"], event["OrderNo"])
        for event in recorder.timeline
        if event["kind"] == "order"
    ] == [("order", channel, order_id)]
    assert not any(event["kind"] == "trade" for event in recorder.timeline)


def test_opening_auction_trade_uses_original_ids(run_events):
    symbol = "000007.SZ"
    channel = 701
    sell_id = 7_000_000_101
    buy_id = 7_000_000_202

    timeline = run_events(
        symbol,
        [
            order_row(symbol, "09:20:00.001000", price=10.00, size=100, side=-1,
                      order_id=sell_id, channel=channel, sequence=1),
            order_row(symbol, "09:20:00.002000", price=10.00, size=100, side=1,
                      order_id=buy_id, channel=channel, sequence=2),
        ],
    )

    assert {
        (event["ChannelNo"], event["BuyNo"], event["SellNo"])
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    } == {(channel, buy_id, sell_id)}


def test_closing_auction_trade_uses_original_ids(run_events):
    symbol = "600007.SH"
    channel = 702
    sell_id = 7_100_000_101
    buy_id = 7_100_000_202

    timeline = run_events(
        symbol,
        [
            order_row(symbol, "14:58:00.001000", price=10.00, size=100, side=-1,
                      order_id=sell_id, channel=channel, sequence=1),
            order_row(symbol, "14:58:00.002000", price=10.00, size=100, side=1,
                      order_id=buy_id, channel=channel, sequence=2),
        ],
    )

    assert {
        (event["ChannelNo"], event["BuyNo"], event["SellNo"])
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    } == {(channel, buy_id, sell_id)}


def test_one_order_can_link_to_multiple_partial_trades(run_events):
    symbol = "000008.SZ"
    channel = 703
    sell_a = 7_200_000_101
    sell_b = 7_200_000_102
    buy_id = 7_200_000_201

    timeline = run_events(
        symbol,
        [
            order_row(symbol, "09:30:00.001000", price=10.00, size=100, side=-1,
                      order_id=sell_a, channel=channel, sequence=1),
            order_row(symbol, "09:30:00.002000", price=10.00, size=200, side=-1,
                      order_id=sell_b, channel=channel, sequence=2),
            order_row(symbol, "09:30:00.003000", price=10.00, size=300, side=1,
                      order_id=buy_id, channel=channel, sequence=3),
        ],
    )

    trades = [
        (event["BuyNo"], event["SellNo"], event["Volume"])
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    ]
    assert trades == [(buy_id, sell_a, 100), (buy_id, sell_b, 200)]


def test_one_order_keeps_its_raw_id_across_partial_fills_then_cancel(run_events):
    symbol = "000011.SZ"
    channel = 734
    resting_sell_id = 8_300_000_101
    first_buy_id = 8_300_000_201
    second_buy_id = 8_300_000_202
    post_cancel_buy_id = 8_300_000_203
    cancel_trade_id = 8_400_000_301

    timeline = run_events(
        symbol,
        [
            order_row(symbol, "09:30:00.001000", price=10.00, size=300, side=-1,
                      order_id=resting_sell_id, channel=channel, sequence=1),
            order_row(symbol, "09:30:00.002000", price=10.00, size=100, side=1,
                      order_id=first_buy_id, channel=channel, sequence=2),
            order_row(symbol, "09:30:01.001000", price=10.00, size=80, side=1,
                      order_id=second_buy_id, channel=channel, sequence=3),
            # The final 120 shares have already been cancelled, so this order
            # must not produce a third trade against resting_sell_id.
            order_row(symbol, "09:30:03.001000", price=10.00, size=120, side=1,
                      order_id=post_cancel_buy_id, channel=channel, sequence=5),
        ],
        [
            trade_row(
                symbol,
                "09:30:02.001000",
                bid_order_id=0,
                ask_order_id=resting_sell_id,
                trade_id=cancel_trade_id,
                trade_bs_flag="S",
                channel=channel,
                sequence=4,
            )
        ],
    )

    calculated = [
        event
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "1"
    ]
    assert [
        (event["BuyNo"], event["SellNo"], event["Volume"])
        for event in calculated
    ] == [
        (first_buy_id, resting_sell_id, 100),
        (second_buy_id, resting_sell_id, 80),
    ]
    assert len({event["Time"] for event in calculated}) == 2
    assert [event["Time"] for event in calculated] == sorted(
        event["Time"] for event in calculated
    )
    assert sum(event["Volume"] for event in calculated) == 180

    cancels = [
        event
        for event in timeline
        if event["kind"] == "trade" and event["ExecType"] == "2"
    ]
    assert [
        (event["TradeIndex"], event["BuyNo"], event["SellNo"])
        for event in cancels
    ] == [(cancel_trade_id, 0, resting_sell_id)]

    calculated_positions = [
        index
        for index, event in enumerate(timeline)
        if event["kind"] == "trade" and event["ExecType"] == "1"
    ]
    cancel_position = next(
        index
        for index, event in enumerate(timeline)
        if event["kind"] == "trade" and event["TradeIndex"] == cancel_trade_id
    )
    assert max(calculated_positions) < cancel_position
