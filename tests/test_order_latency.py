#!/usr/bin/env python3
"""策略下单/撤单延迟(order latency)测试。

语义(见 Strategy::setOrderLatencyMs / run_backtest(order_latency_ms=)):
- latency=0(默认):下单/撤单同步进簿,行为与旧版完全一致;
- latency>0:策略单延迟到 发出时刻+latency 才"到达交易所"——过校验、进簿、
  参与撮合,下单确认回调同时延迟;撤单同延迟通道,与下单保 FIFO;
- 回测时钟只随市场事件前进:release 落在两个事件之间时,随下一个事件进簿。

实验引擎影子包用法(全量校验期间不替换官方 .so):
    PYTHONPATH=exp_pkg .venv/bin/python -m pytest tests/test_order_latency.py -v
旧版引擎(无 setOrderLatencyMs)下整个模块自动 skip。
"""

from typing import List, Optional

import pandas as pd
import pytest

try:
    # 实验引擎影子包(editable 安装的 import 钩子会拦截 wangcai_syn,
    # 故影子包改名 wangcai_lat,内部绝对导入已同步替换)
    from wangcai_lat import (Strategy, UserEvent, TradeCallback, OrderCallback,
                             make_order, make_cancel, run_backtest)
except ImportError:
    from wangcai_syn import (Strategy, UserEvent, TradeCallback, OrderCallback,
                             make_order, make_cancel, run_backtest)

pytestmark = pytest.mark.skipif(
    not hasattr(Strategy, "setOrderLatencyMs"),
    reason="当前 wangcai_cpp 无下单延迟 API(旧引擎),跳过")


# ---------------------------------------------------------------------------
# 合成数据构造(与 tests/price_cage/test_price_cage.py 同口径,自带一份保持独立)
# ---------------------------------------------------------------------------

_CSTICK_COLS = (
    ["date", "time", "sym", "prevclose", "open", "high", "low", "close",
     "volume", "turnover", "tradecount"]
    + [c for i in range(1, 11) for c in (f"bid{i}", f"bsize{i}")]
    + [c for i in range(1, 11) for c in (f"ask{i}", f"asize{i}")]
    + ["avgbid", "avgask", "totalbsize", "totalasize", "iopv",
       "etfpchcount", "etfredmcount", "etfpchsize", "etfredmsize",
       "etfpchamt", "etfredmamt", "status", "updatetime"]
)


def make_latency_data(sym: str, date: str, prev_close: float,
                      ords: list, tras: list, ticks: list,
                      upper: float, lower: float):
    """ords: [(time, price, size, side, orderid)];tras: [(time, price, size, bid, ask, tid, exectype)]"""
    cstick = pd.DataFrame([
        {**{c: 0.0 for c in _CSTICK_COLS},
         "date": date, "time": f"0 days {t}", "sym": sym,
         "prevclose": prev_close, "open": prev_close, "high": prev_close,
         "low": prev_close, "close": prev_close, "status": "b'T'",
         "bid1": prev_close, "ask1": prev_close + 0.01,
         "updatetime": f"{date} {t}"}
        for t in ticks
    ], columns=_CSTICK_COLS)

    csord = pd.DataFrame([
        {"date": date, "time": f"0 days {t}", "sym": sym, "price": p, "size": s,
         "side": side, "ordertype": 2, "orderid": oid, "channelno": 1,
         "seqno": oid, "bizindex": 0, "updatetime": f"{date} {t}"}
        for t, p, s, side, oid in ords
    ])

    cstra = pd.DataFrame([
        {"date": date, "time": f"0 days {t}", "sym": sym, "price": p, "size": s,
         "bidorderid": bid, "askorderid": ask, "tradeid": tid,
         "exectype": f"b'{ex}'", "tradebsflag": "b'B'", "channelno": 1,
         "bizindex": 0, "updatetime": f"{date} {t}"}
        for t, p, s, bid, ask, tid, ex in tras
    ], columns=["date", "time", "sym", "price", "size", "bidorderid",
                "askorderid", "tradeid", "exectype", "tradebsflag",
                "channelno", "bizindex", "updatetime"])

    csbar1d = pd.DataFrame([{
        "sym": sym, "prevclose": prev_close, "open": prev_close,
        "high": prev_close, "low": prev_close, "close": prev_close,
        "volume": 0, "turnover": 0, "tradecount": 0, "af": 1.0,
        "upperlimit": upper, "lowerlimit": lower,
    }])
    return {sym: (cstick, csord, cstra, csbar1d)}


SYM, DATE = "000001.SZ", "2025-06-02"
# 场景:09:30:00.005 历史卖单 9001 挂出 10.00x1000;09:30:00.010 历史买单 9002 把它吃光;
# 09:30:00.030 再放一条无关委托 9003——合成 tick 事件的 datetime 字符串为空
# (Snapshot.datetime 亦为空),延迟事件需要一条带真实时间的 ord 事件触发释放
_ORDS = [("09:30:00.005", 10.00, 1000, 2, 9001),
         ("09:30:00.010", 10.00, 1000, 1, 9002),
         ("09:30:00.030", 9.90, 100, 1, 9003)]
_TRAS = [("09:30:00.010", 10.00, 1000, 9002, 9001, 8001, "1")]
_TICKS = ["09:15:00", "09:30:00", "09:30:01"]


class LatencyProbe(Strategy):
    """在第 N 条历史委托事件(onOrderEvent 计数)时下单/撤单,记录全部回调"""

    def __init__(self, sid: str, symbol: str, latency_ms: int = 0):
        super().__init__()
        self.sid = sid
        self.symbol = symbol
        if latency_ms > 0:
            self.setOrderLatencyMs(latency_ms)
        self.actions: List[tuple] = []   # ("order"/"cancel", localid, price, vol, n_seen)
        self._fired = set()
        self.trade_cbs: List[tuple] = []  # (localid, matchtype, volume, price, matchtime)
        self.order_cbs: List[tuple] = []  # (localid, time)
        self.order_events_seen = 0

    def getStrategyId(self):
        return self.sid

    def onOrderEvent(self, order) -> List[UserEvent]:
        self.order_events_seen += 1
        evs = []
        for i, (kind, localid, price, vol, n_trig) in enumerate(self.actions):
            if i in self._fired or self.order_events_seen != n_trig:
                continue
            self._fired.add(i)
            if kind == "order":
                evs.append(make_order(Broker="", Account=self.sid, Exchange=1,
                                      Instrument=self.symbol, OrderLocalID=localid,
                                      OrderType=0, Direction=1,
                                      Price=price, Volume=vol))
            else:
                evs.append(make_cancel(Broker="", Account=self.sid, Exchange=1,
                                       Instrument=self.symbol,
                                       CancelOrderLocalID=f"C_{localid}",
                                       OrderLocalID=localid))
        return evs

    def onTradeEvent(self, trade) -> List[UserEvent]:
        return []

    def onTickEvent(self, tick) -> List[UserEvent]:
        return []

    def onOrderFilled(self, *a): pass
    def onOrderCancelled(self, *a): pass

    def onOrderCallback(self, cb: OrderCallback):
        self.order_cbs.append((cb.orderlocalid, cb.time))

    def onTradeCallback(self, cb: TradeCallback):
        self.trade_cbs.append((cb.localid, cb.matchtype, cb.volume,
                               cb.price, cb.matchtime))


def _build():
    return make_latency_data(SYM, DATE, 10.0, _ORDS, _TRAS, _TICKS,
                             upper=11.0, lower=9.0)


def _fills_for(strat, localid):
    return [t for t in strat.trade_cbs if t[0] == localid and t[1] == 'T']


# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------

def test_latency_zero_fills_and_immediate_callback():
    """latency=0:09:30:00.005(卖单 9001 已在簿)买 10.00 → 立即穿价成交;
    下单确认回调时间在下单当下(.005)——旧版行为不变"""
    s = LatencyProbe("LAT0", SYM)  # 不设延迟
    s.actions = [("order", "L0", 100000, 100, 1)]  # 10.00 x 100,第1条委托事件时下单
    assert run_backtest(_build(), s)
    assert _fills_for(s, "L0"), "latency=0 应立即进簿并吃到簿上的历史卖单"
    assert s.order_cbs and s.order_cbs[0][1].endswith("09:30:00.005"), \
        f"latency=0 回调应在下单当下发出,实际: {s.order_cbs}"


def test_latency_20ms_misses_trade_and_delays_callback():
    """latency=20ms(走 run_backtest 参数):同一买单 release=.005+.020=.025,
    晚于 .010 的历史成交(卖单已被 9002 吃光)→ 不成交;
    下单回调延迟到 release 之后第一条市场事件(.030 的 9003)才发出"""
    s = LatencyProbe("LAT20", SYM)  # 延迟由 run_backtest 参数设置
    s.actions = [("order", "L20", 100000, 100, 1)]
    assert run_backtest(_build(), s, order_latency_ms=20)
    assert not _fills_for(s, "L20"), "latency=20ms 应错过 .010 的历史成交"
    assert s.order_cbs, "订单到达后仍应收到下单确认回调"
    cb_time = s.order_cbs[0][1]
    assert cb_time > f"{DATE} 09:30:00.019", \
        f"回调时间应 >= 发出时刻+20ms,实际: {cb_time}"


def test_latency_cancel_fifo(capfd):
    """latency=20ms:第1条事件(.005)下买单 9.95(无对手价,待挂簿),
    第2条事件(.010)发撤单——此时订单还在延迟队列里(release=.025),
    撤单 release=.030;同延迟通道保 FIFO,.030 事件时订单先进簿、撤单随后到达,
    撤单成功(覆盖延迟队列中订单的撤单路由:不得报"找不到订单")"""
    s = LatencyProbe("LATC", SYM, latency_ms=20)
    s.actions = [("order", "LC", 99500, 100, 1),   # 9.95 x 100
                 ("cancel", "LC", 0, 0, 2)]
    assert run_backtest(_build(), s)
    out = capfd.readouterr().out
    assert "[策略撤单失败]" not in out, f"延迟队列中的订单撤单被误判找不到: {out}"
    assert "[策略撤单]" in out, f"撤单未生效: {out}"
    assert not _fills_for(s, "LC")
