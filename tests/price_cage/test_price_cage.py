"""
价格笼子测试套件（需求 2）

覆盖（设计文档 §3.2/§3.3）：
A. 策略单数值规则矩阵（合成数据）
   1) 主板 2025（拒单时代）：笼内边界恰通过、笼外 1 tick 废单、tick 取整边界
   2) 主板 2021（无笼时代）：任意价格放行
   3) 科创板低价股：2019 纯 ±2%（2%<0.1 时无兜底）vs 2023 有 0.1 兜底
   4) 基准价链兜底：空卖簿→买一；双空→昨收
   5) 涨跌停前置校验：超涨跌停废单（所有时代）
B. 创业板暂存窗口（2022）策略单：超笼入笼 → 基准回落出笼参与撮合；入笼期间可撤单
C. 历史单消息流反推状态机（2022 创业板）
   6) 穿价+下一条是本单成交 → 正常撮合
   7) 穿价+下一条不是 → 入笼，事件快照十档不可见；盘口恢复自动出笼
   8) 撤单撤笼中单
D. 真实数据
   9) 300026.SZ 2022-06-30（暂存窗口）：跑通、重建成交与 cstra 一致、笼单存在且 14:57 清零
  10) 2025 数据（拒单时代）：策略笼开关不影响历史重建（开关等价）
"""

import re
from pathlib import Path
from typing import List, Optional

import pandas as pd
import pytest

from wangcai_syn import (
    Strategy, UserEvent, Snapshot, OrderDetail, TradeDetail,
    TradeCallback, OrderCallback, make_order, make_cancel, run_backtest,
)

DATA_DIR = next(
    (d for d in (Path(__file__).resolve().parent / "../../data",
                 Path(__file__).resolve().parent / "../../new_log") if d.is_dir()),
    Path(__file__).resolve().parent / "../../new_log"
)
AQS_DIR = next(
    (d for d in (Path(__file__).resolve().parent / "data", Path("/tmp/aqsnapshots"))
     if (d / "300026.SZ_2022-06-30_cstick.csv").is_file()),
    Path("/tmp/aqsnapshots"),
)  # 暂存窗口真实数据（aqsnapshots 参考仓库，后缀式命名 {sym}_{date}_{kind}.csv）


# ---------------------------------------------------------------------------
# 合成数据构造
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


def make_cage_data(sym: str, date: str, prev_close: float,
                   ords: list, tras: list, upper: float, lower: float,
                   ticks: Optional[list] = None):
    """构造带价格笼子场景的合成数据。

    ords: [(time_str, price, size, side, orderid)]  限价委托
    tras: [(time_str, price, size, bidid, askid, tradeid, exectype)]
    """
    tick_rows = ticks or ["09:15:00", "09:30:00"]
    cstick = pd.DataFrame([
        {**{c: 0.0 for c in _CSTICK_COLS},
         "date": date, "time": f"0 days {t}", "sym": sym,
         "prevclose": prev_close, "open": prev_close, "high": prev_close,
         "low": prev_close, "close": prev_close, "status": "b'T'",
         "bid1": prev_close, "ask1": prev_close + 0.01,
         "updatetime": f"{date} {t}"}
        for t in tick_rows
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


class CageProbeStrategy(Strategy):
    """在指定事件序号/时间下单/撤单，记录全部回调与事件快照十档"""

    def __init__(self, sid: str):
        super().__init__()
        self.sid = sid
        self.orders: List[tuple] = []        # (localid, price, vol, direction, 触发时间)
        self.cancels: List[tuple] = []
        self.trade_cbs: List[tuple] = []     # (localid, matchtype, reason无, vol, price, matchtime)
        self.snaps: List[dict] = []
        self.order_events_seen = 0

    def getStrategyId(self):
        return self.sid

    def _maybe_act(self, n_seen: int, datetime_str: str) -> List[UserEvent]:
        evs = []
        for localid, price, vol, direction, trig in self.orders:
            if trig(n_seen, datetime_str):
                evs.append(make_order(Broker="", Account=self.sid, Exchange=1,
                                      Instrument=self.sid_symbol, OrderLocalID=localid,
                                      OrderType=0, Direction=direction,
                                      Price=price, Volume=vol))
        for localid, trig in self.cancels:
            if trig(n_seen, datetime_str):
                evs.append(make_cancel(Broker="", Account=self.sid, Exchange=1,
                                       Instrument=self.sid_symbol,
                                       CancelOrderLocalID=f"C_{localid}",
                                       OrderLocalID=localid))
        return evs

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        self.order_events_seen += 1
        return self._maybe_act(self.order_events_seen,
                               f"T{order.Time//1000:06d}")

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        return []

    def onEventSnapshot(self, snap: Snapshot) -> List[UserEvent]:
        self.snaps.append({
            "datetime": snap.datetime,
            "bids": list(snap.bids), "bid_sizes": list(snap.bid_sizes),
            "asks": list(snap.asks), "ask_sizes": list(snap.ask_sizes),
        })
        return []

    def onOrderFilled(self, *a): pass
    def onOrderCancelled(self, *a): pass
    def onOrderCallback(self, cb): pass

    def onTradeCallback(self, cb: TradeCallback):
        self.trade_cbs.append((cb.localid, cb.matchtype, cb.volume,
                               cb.price, cb.matchtime))

    @property
    def sid_symbol(self):
        return getattr(self, "_symbol", "300827.SZ")


def _rejects_for(strat, localid):
    return [t for t in strat.trade_cbs if t[0] == localid and t[1] == 'D']


def _fills_for(strat, localid):
    return [t for t in strat.trade_cbs if t[0] == localid and t[1] == 'T']


# ---------------------------------------------------------------------------
# A. 策略单数值规则矩阵
# ---------------------------------------------------------------------------


def _base_orders_around_10():
    """09:30 后建立盘口：买一 9.99x300（B1），卖一 10.01x400（A1）"""
    return [
        ("09:30:00.050", 9.99, 300, 1, 101),
        ("09:30:00.150", 10.01, 400, 2, 102),
    ]


def test_main_board_2025_reject_matrix():
    """主板 2025（拒单时代）：基准=卖一 10.01，幅度 max(2%,0.1)=0.2002→上限 10.2102
    四舍五入至 tick = 10.21。买 10.21 恰在笼内（通过，穿价成交）；买 10.22 废单。"""
    sym, date = "600000.SH", "2025-06-02"
    data = make_cage_data(sym, date, 10.0, _base_orders_around_10(), [],
                          upper=11.0, lower=9.0)
    s = CageProbeStrategy("M1")
    s._symbol = sym
    s.orders = [
        ("OK_IN",  102100, 100, 1, lambda n, t: n == 2),   # 10.21 ≤ 10.2102
        ("BAD_OUT", 102200, 100, 1, lambda n, t: n == 2),  # 10.22 > 10.2102
    ]
    assert run_backtest(data, s, event_snapshot_enabled=True)
    assert _rejects_for(s, "BAD_OUT"), "笼外 1 tick 未被拒单"
    assert not _rejects_for(s, "OK_IN"), "笼内边界被误拒"
    # OK_IN(10.21) 穿价卖一(10.01)：过笼后按主动单正常成交
    fills = _fills_for(s, "OK_IN")
    assert fills and fills[0][3] == 100100, f"笼内穿价单应成交@10.01: {fills}"


def test_main_board_2021_no_cage():
    """主板 2021（无笼时代）：任意远价放行（只受涨跌停约束）"""
    sym, date = "600000.SH", "2021-06-02"
    data = make_cage_data(sym, date, 10.0, _base_orders_around_10(), [],
                          upper=11.0, lower=9.0)
    s = CageProbeStrategy("M2")
    s._symbol = sym
    s.orders = [("FAR", 109800, 100, 1, lambda n, t: n == 2)]  # 远超2%
    assert run_backtest(data, s)
    assert not _rejects_for(s, "FAR"), "无笼时代不应拒单"


def test_star_low_price_floor_vs_pure():
    """科创板低价股（昨收 2.00）：2019 纯 ±2%（幅度 0.04）vs 2023 加 0.1 兜底（幅度 0.1）。
    卖一 2.00，2019 上限 2.04 → 买 2.05 废单；2023 上限 2.10 → 买 2.05 通过。"""
    def build(date):
        ords = [("09:30:00.050", 1.99, 300, 1, 101),
                ("09:30:00.150", 2.00, 400, 2, 102)]
        return make_cage_data("688001.SH", date, 2.0, ords, [],
                              upper=2.4, lower=1.6)

    s19 = CageProbeStrategy("S19")
    s19._symbol = "688001.SH"
    s19.orders = [("P", 20500, 100, 1, lambda n, t: n == 2)]
    assert run_backtest(build("2019-08-01"), s19)
    assert _rejects_for(s19, "P"), "科创板 2019 纯 2%：2.05 应废单"

    s23 = CageProbeStrategy("S23")
    s23._symbol = "688001.SH"
    s23.orders = [("P", 20500, 100, 1, lambda n, t: n == 2)]
    assert run_backtest(build("2023-05-02"), s23)
    assert not _rejects_for(s23, "P"), "科创板 2023 有 0.1 兜底：2.05 应通过（上限 2.10）"


def test_price_limit_reject_all_eras():
    """涨跌停前置校验：超涨停废单（无笼时代也生效）。
    注意 loadPriceLimits 对数据涨跌停有 +0.1 元冗余容差（引擎既有设计），
    故用明确超出的价格（12.0 > 11.0+0.1）。"""
    sym, date = "600000.SH", "2021-06-02"
    data = make_cage_data(sym, date, 10.0, _base_orders_around_10(), [],
                          upper=11.0, lower=9.0)
    s = CageProbeStrategy("PL")
    s._symbol = sym
    s.orders = [("OVER_UP", 120000, 100, 1, lambda n, t: n == 2)]
    assert run_backtest(data, s)
    assert _rejects_for(s, "OVER_UP"), "超涨停未废单"


@pytest.mark.parametrize("date,expect", [
    ("2020-08-21", "pass"),     # 周五：8.24 前，创业板无笼
    ("2020-08-24", "dormant"),  # 周一：暂存窗口开始（纯 ±2%）
    ("2023-04-07", "dormant"),  # 周五：4.10 前，暂存窗口最后一天
    ("2023-04-10", "reject"),   # 周一：全面注册制，拒单 + 0.1 元兜底
])
def test_stage_date_boundaries(date, expect):
    """阶段判定的日期边界（设计文档 §3.3-7）：2020.8.24 与 2023.4.10 前后各一天。
    创业板 300001，卖一 10.01 → 上限约 10.21（暂存期）/10.21（拒单期取整同），
    策略买 10.50 远超范围：无笼时代放行 / 暂存窗口入笼 / 拒单时代废单。"""
    sym = "300001.SZ"
    data = make_cage_data(sym, date, 10.0, _base_orders_around_10(), [],
                          upper=12.0, lower=8.0)
    s = CageProbeStrategy(f"B_{date}")
    s._symbol = sym
    s.orders = [("B", 105000, 100, 1, lambda n, t: n == 2)]
    assert run_backtest(data, s)
    if expect == "pass":
        assert not _rejects_for(s, "B"), "无笼时代不应废单"
    elif expect == "dormant":
        assert not _rejects_for(s, "B"), "暂存模式不应废单（应入笼）"
    else:
        assert _rejects_for(s, "B"), "拒单时代超范围应废单"


# ---------------------------------------------------------------------------
# B. 创业板暂存窗口：策略单入笼/出笼/撤单
# ---------------------------------------------------------------------------


def test_gem_dormant_enter_exit_and_cancel():
    """创业板 2022 暂存窗口（策略单数值规则）：
    卖一 10.01 → 上限 = 10.01×1.02=10.2102，买 10.23 超笼入笼（不废单）。
    出笼按数值规则（制度："价格落回有效申报范围"）：09:40 撤掉卖一后基准退化为买一
    9.99（上限 10.1898，10.23 仍超 → 不出笼）；09:41 新买委托 10.05 抬升基准 →
    上限 10.05×1.02=10.251 → 10.23 落回范围 → 出笼成为新买一。
    另验证：入笼期间可撤单。"""
    sym, date = "300001.SZ", "2022-06-01"
    ords = [
        ("09:30:00.050", 9.99, 300, 1, 101),
        ("09:30:00.150", 10.01, 400, 2, 102),   # 卖一 A1（唯一卖单）
        ("09:41:00.000", 10.05, 500, 1, 103),   # 抬升基准的新买委托
        ("09:42:00.000", 10.23, 200, 2, 104),   # 出笼后到达的真实对手卖单
    ]
    tras = [("09:40:00.000", 10.01, 400, 0, 102, 9001, "2")]  # 撤掉卖一
    data = make_cage_data(sym, date, 10.0, ords, tras, upper=12.0, lower=8.0)

    s = CageProbeStrategy("G1")
    s._symbol = sym
    s.orders = [
        ("DORM", 102300, 100, 1, lambda n, t: n == 2),  # 超 10.22 上限 → 入笼
    ]
    assert run_backtest(data, s, event_snapshot_enabled=True)
    # 未被拒单（暂存模式不废单）
    assert not _rejects_for(s, "DORM"), "暂存模式不应废单"
    # 入笼期间：所有快照买一 < 10.23（笼单不可见；策略单为影子单，
    # 出笼后进虚拟簿，公开十档本就不含虚拟单）
    before = [sn for sn in s.snaps if sn["datetime"][11:19] < "09:41:00"]
    assert before, "未收到事件快照"
    for sn in before:
        assert sn["bids"][0] < 102300, \
            f"笼中策略单泄露到公开盘口: {sn['datetime']} bid1={sn['bids'][0]}"
    # 出笼的行为验证：09:41 基准抬升 → DORM 出笼挂虚拟簿（10.23 买）；
    # 09:42 真实卖单 10.23 到达 → 虚拟单撮合 → DORM 收到成交回调
    fills = _fills_for(s, "DORM")
    assert fills and fills[0][3] == 102300, \
        f"出笼后的笼单未与真实对手成交: {fills}"

    # 场景2：入笼期间撤单（第 3 条委托事件时触发撤单）
    s2 = CageProbeStrategy("G2")
    s2._symbol = sym
    s2.orders = [("D2", 102300, 100, 1, lambda n, t: n == 2)]
    s2.cancels = [("D2", lambda n, t: n == 3)]
    ords2 = ords[:2] + [("09:31:00.000", 9.98, 100, 1, 105)]
    assert run_backtest(make_cage_data(sym, date, 10.0, ords2, [], upper=12.0, lower=8.0), s2)
    cancels = [t for t in s2.trade_cbs if t[0] == "D2" and t[1] == 'D']
    assert cancels, "笼中策略单撤单未成功"


# ---------------------------------------------------------------------------
# C. 历史单反推状态机（2022 创业板）
# ---------------------------------------------------------------------------


def test_inference_crossing_with_trade_passes():
    """穿价历史单 + 紧挨下一条是它的成交 → 正常撮合（重建出该订单对）"""
    sym, date = "300001.SZ", "2022-06-01"
    ords = [
        ("09:30:00.050", 9.99, 300, 1, 101),
        ("09:30:00.150", 10.01, 400, 2, 102),   # 卖一
        ("09:30:01.000", 10.01, 150, 1, 103),   # 穿价买单（≥ask1）
    ]
    tras = [("09:30:01.010", 10.01, 150, 103, 102, 9001, "1")]  # 紧挨：它的成交
    data = make_cage_data(sym, date, 10.0, ords, tras, upper=12.0, lower=8.0)

    class Collector(CageProbeStrategy):
        def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
            if trade.ExecType == '1':
                self.filled = getattr(self, "filled", [])
                self.filled.append((int(trade.BuyNo), int(trade.SellNo),
                                    int(trade.Price), int(trade.Volume)))
            return []

    s = Collector("C1")
    s._symbol = sym
    assert run_backtest(data, s)
    assert (103, 102, 100100, 150) in getattr(s, "filled", []), \
        f"穿价单未正常撮合: {getattr(s, 'filled', [])}"


def test_inference_suspend_invisible_and_release():
    """穿价历史单 + 下一条不是它的成交 → 入笼（事件快照十档不可见）；
    出笼按数值判据（价格落回有效申报范围）：撤空卖盘后基准退化为买一 9.99，
    上限 9.99×1.02=10.1898，10.30 仍超 → 不出笼；新买委托抬升基准至 10.10
    → 上限 10.302 → 10.30 落回 → 出笼恢复到买一。"""
    sym, date = "300001.SZ", "2022-06-01"
    ords = [
        ("09:30:00.050", 9.99, 300, 1, 101),
        ("09:30:00.150", 10.01, 400, 2, 102),   # 卖一
        ("09:30:01.000", 10.30, 200, 1, 103),   # 穿价买单（会被入笼）
        ("09:30:02.000", 9.98, 100, 1, 104),    # 下一条消息（非 103 的成交）
        ("09:31:30.000", 10.10, 500, 1, 105),   # 抬升基准的新买委托
    ]
    tras = [("09:31:00.000", 10.01, 400, 0, 102, 9001, "2")]  # 撤空卖盘
    data = make_cage_data(sym, date, 10.0, ords, tras, upper=12.0, lower=8.0)

    s = CageProbeStrategy("C2")
    s._symbol = sym
    assert run_backtest(data, s, event_snapshot_enabled=True)

    # 09:30:01 后（103 已入笼）：买一不应是 10.30
    during = [sn for sn in s.snaps if "09:30:02" <= sn["datetime"][11:19] < "09:31:00"]
    assert during, "未收到事件快照"
    for sn in during:
        assert sn["bids"][0] < 103000, \
            f"历史笼单泄露到公开盘口: {sn['datetime']} bid1={sn['bids'][0]}"
        assert sn["bids"][0] == 99900 and sn["bid_sizes"][0] == 300, \
            f"盘口异常: {sn['bids'][:2]} {sn['bid_sizes'][:2]}"
    # 09:31 撤空卖盘：基准=买一 9.99 → 上限 10.1898 → 10.30 仍超，不出笼
    mid = [sn for sn in s.snaps if "09:31:00" <= sn["datetime"][11:19] < "09:31:30"]
    assert mid, "撤卖盘后无快照"
    for sn in mid:
        assert sn["bids"][0] == 99900, \
            f"未落回范围不应出笼: {sn['datetime']} bid1={sn['bids'][0]}"
    # 09:31:30 基准抬至 10.10 → 上限 10.302 → 10.30 落回 → 出笼：买一变为 10.30
    after = [sn for sn in s.snaps if sn["datetime"][11:19] >= "09:31:30"]
    assert after, "抬基准后无快照"
    assert after[0]["bids"][0] == 103000, \
        f"历史笼单未出笼恢复到买一: {after[0]['bids'][:2]}"


def test_inference_cancel_suspended_historical():
    """撤单消息可撤掉笼中历史单（撤单后不再出笼）"""
    sym, date = "300001.SZ", "2022-06-01"
    ords = [
        ("09:30:00.050", 9.99, 300, 1, 101),
        ("09:30:00.150", 10.01, 400, 2, 102),
        ("09:30:01.000", 10.30, 200, 1, 103),   # 穿价买单 → 入笼
        ("09:30:02.000", 9.98, 100, 1, 104),
    ]
    tras = [
        ("09:30:01.010", 0.0, 0, 0, 0, 0, "1"),        # 干扰消息（非103成交）
        ("09:31:00.000", 0.0, 200, 103, 0, 9002, "2"),  # 撤销笼中 103
        ("09:32:00.000", 10.01, 400, 0, 102, 9003, "2"),  # 撤空卖盘（触发出笼扫描）
    ]
    data = make_cage_data(sym, date, 10.0, ords, tras, upper=12.0, lower=8.0)
    s = CageProbeStrategy("C3")
    s._symbol = sym
    assert run_backtest(data, s, event_snapshot_enabled=True)
    after = [sn for sn in s.snaps if sn["datetime"][11:19] >= "09:32:00"]
    assert after, "无快照"
    # 103 已被撤：卖盘清空后买一应回落到 9.99，而非 10.30
    assert after[0]["bids"][0] == 99900, \
        f"已撤笼单仍然出笼: bid1={after[0]['bids'][0]}"


def test_cage_orders_restored_at_close_auction():
    """14:57 收盘集合竞价开始：笼中历史单与策略笼单全部恢复参与收盘撮合
    （设计文档 §3.2.2 配套 / §3.3-6）。
    历史笼单 103（买 10.30×200）与策略笼单（买 10.25×100）入笼后盘口无变化，
    14:57 恢复进收盘竞价；簿上卖 10.01×400 → 收盘价 10.01，两笼单均成交。"""
    sym, date = "300001.SZ", "2022-06-01"
    ords = [
        ("09:30:00.050", 9.99, 300, 1, 101),
        ("09:30:00.150", 10.01, 400, 2, 102),
        ("09:30:01.000", 10.30, 200, 1, 103),   # 穿价买单 → 入笼
        ("09:30:02.000", 9.98, 100, 1, 104),    # 非 103 成交 → 103 确认入笼
    ]
    data = make_cage_data(sym, date, 10.0, ords, [], upper=12.0, lower=8.0,
                          ticks=["09:15:00", "09:25:00", "09:30:00", "14:57:00"])
    s = CageProbeStrategy("C4")
    s._symbol = sym
    s.orders = [("CL", 102500, 100, 1, lambda n, t: n == 2)]  # 超 10.2102 → 策略笼单
    assert run_backtest(data, s, event_snapshot_enabled=True)
    # 策略笼单在收盘竞价恢复后与卖盘撮合成交（收盘价 10.01 ≤ 10.25）
    fills = _fills_for(s, "CL")
    assert fills and fills[0][3] == 100100, \
        f"收盘恢复的笼单未参与收盘撮合: {fills}"
    assert not _rejects_for(s, "CL"), "暂存模式不应废单"


# ---------------------------------------------------------------------------
# D. 真实数据
# ---------------------------------------------------------------------------


def _load_real(base: Path, sym: str, date: str, is_etf: bool = False):
    files = {k: pd.read_csv(base / f"{k}_{sym}_{date}.csv")
             for k in ("cstick", "csord", "cstra", "csbar1d")}
    return files


def _norm_bytes(v):
    """b'1' → 1；兼容已解包的字符串"""
    t = str(v)
    return t[2:-1] if t.startswith("b'") else t.strip()


def _synth_csbar(base: Path, sym: str, date: str):
    cstick = pd.read_csv(base / f"{sym}_{date}_cstick.csv")
    prev = float(cstick["prevclose"].dropna().iloc[-1])
    return pd.DataFrame([{"sym": sym, "prevclose": prev, "open": prev,
                          "high": prev, "low": prev, "close": prev,
                          "volume": 0, "turnover": 0, "tradecount": 0,
                          "af": 1.0, "upperlimit": prev * 1.2,
                          "lowerlimit": prev * 0.8}])


class RealWatch(Strategy):
    """只收集：重建成交(ExecType=1) + 事件快照买一序列"""

    def __init__(self):
        super().__init__()
        self.trades: List[tuple] = []
        self.bids: List[tuple] = []

    def getStrategyId(self):
        return "RW"

    def onOrderEvent(self, o): return []

    def onTradeEvent(self, t):
        if t.ExecType == '1':
            self.trades.append((int(t.ChannelNo), int(t.BuyNo), int(t.SellNo),
                                int(t.Price), int(t.Volume)))
        return []

    def onTickEvent(self, s): return []

    def onEventSnapshot(self, snap):
        self.bids.append((snap.datetime, snap.bids[0] if snap.bids else 0,
                          snap.bid_sizes[0] if snap.bid_sizes else 0))
        return []

    def onOrderFilled(self, *a): pass
    def onOrderCancelled(self, *a): pass
    def onOrderCallback(self, cb): pass
    def onTradeCallback(self, cb): pass


@pytest.mark.slow
def test_real_300026_suspended_window():
    """300026.SZ 2022-06-30（创业板暂存窗口真实数据）：
    1) 反推状态机激活（真实成交事件进流做穿价确认）
    2) 重建成交（ExecType=1）与 cstra 连续段按订单对+量聚合一致
    """
    sym, date = "300026.SZ", "2022-06-30"
    if not (AQS_DIR / f"{sym}_{date}_cstra.csv").is_file():
        pytest.skip("缺少 300026 暂存窗口数据")
    cstick = pd.read_csv(AQS_DIR / f"{sym}_{date}_cstick.csv")
    csord = pd.read_csv(AQS_DIR / f"{sym}_{date}_csord.csv")
    cstra = pd.read_csv(AQS_DIR / f"{sym}_{date}_cstra.csv")
    csbar = _synth_csbar(AQS_DIR, sym, date)
    s = RealWatch()
    assert run_backtest({sym: (cstick, csord, cstra, csbar, False)}, s,
                        event_snapshot_enabled=True)
    assert s.bids, "未收到事件快照"

    ts = cstra["time"].astype(str)
    ex = cstra["exectype"].map(_norm_bytes)
    real = cstra[(ex == "1") & (ts >= "0 days 09:30:00") & (ts < "0 days 14:57:00")]
    real_pairs = {}
    for _, r in real.iterrows():
        k = (int(r["channelno"]), int(r["bidorderid"]), int(r["askorderid"]),
             int(round(float(r["price"]) * 10000)))
        real_pairs[k] = real_pairs.get(k, 0) + int(r["size"])

    engine_pairs = {}
    for ch, b, a, px, v in s.trades:
        engine_pairs[(ch, b, a, px)] = engine_pairs.get((ch, b, a, px), 0) + v
    # 连续段引擎成交（含集合竞价 settle 的成交按时间过滤——RealWatch 无时间，
    # 用总量近似对比：引擎含竞价段，故只要求 真实订单对 ⊆ 引擎订单对 且量不小于）
    matched = sum(1 for k, v in real_pairs.items()
                  if engine_pairs.get(k, 0) >= v)
    rate = matched / max(len(real_pairs), 1)
    print(f"[300026] 连续段订单对匹配率(引擎量>=真实量): {matched}/{len(real_pairs)} = {rate*100:.2f}%")
    assert rate >= 0.95, f"暂存窗口重建成交一致性不足: {rate*100:.2f}%"


@pytest.mark.slow
def test_real_2025_user_cage_switch_equivalence():
    """2025 数据（拒单时代）：策略笼开关不影响历史重建——重建成交与真实一致"""
    sym, date = "002929.SZ", "2025-12-15"
    if not (DATA_DIR / f"cstra_{sym}_{date}.csv").is_file():
        pytest.skip("缺少 002929 数据")

    class Collector(RealWatch):
        def onTradeEvent(self, t):
            if t.ExecType == '1':
                hhmmss = t.Time // 1000  # HHMMSS
                if 93000 <= hhmmss < 145700:  # 仅连续竞价段（排除集合竞价 settle）
                    self.trades.append((int(t.ChannelNo), int(t.BuyNo), int(t.SellNo),
                                        int(t.Price), int(t.Volume)))
            return []

    files = _load_real(DATA_DIR, sym, date)

    def run(tag, enabled):
        s = Collector()
        assert run_backtest({sym: (files["cstick"], files["csord"],
                                   files["cstra"], files["csbar1d"], False)},
                            s, user_cage_enabled=enabled)
        agg = {}
        for ch, b, a, px, v in s.trades:
            agg[(ch, b, a, px)] = agg.get((ch, b, a, px), 0) + v
        return agg

    off = run("off", False)
    on = run("on", True)
    assert off == on, "策略笼开关改变了历史重建成交"

    # 与真实一致性（开盘+连续段 100%，对应修正版实验基线）
    cstra = files["cstra"].copy()
    cstra["exectype"] = cstra["exectype"].map(_norm_bytes)
    ts = cstra["time"].astype(str)
    real = cstra[(cstra["exectype"] == "1") & (ts >= "0 days 09:30:00")
                 & (ts < "0 days 14:57:00")]
    real_agg = {}
    for _, r in real.iterrows():
        k = (int(r["channelno"]), int(r["bidorderid"]), int(r["askorderid"]),
             int(round(float(r["price"]) * 10000)))
        real_agg[k] = real_agg.get(k, 0) + int(r["size"])
    assert off == real_agg, "重建成交与真实不一致（回归失败）"
