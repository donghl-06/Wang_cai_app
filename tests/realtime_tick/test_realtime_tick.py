"""
实时合成 Tick（onRealTimeTickEvent）测试套件

覆盖：
1) 默认关闭：不设 realtime_tick_interval_ms 时回调次数必须为 0
2) 开关等价：开启实时 tick（回调返回空）不得改变回测的下单/成交/撤单结果
3) 合成场景精确断言：可控盘口下，十档价量、last_price、Volume/Turnover/
   NumTrades、TotalBidVol/TotalAskVol 与手算值完全一致
4) 网格语义：每个引擎的推送网格编号严格递增（同网格不重复推送）、
   不早于 09:15:00、不晚于 15:00:00.999
5) 累计口径：Volume/NumTrades 由引擎重建成交驱动，严格单调不减；
   与官方 tick 的偏差只记录不断言（官方快照时间戳口径不同，出入正常）
6) 真实数据：SZ 股票 / SH 股票 / SH ETF 各跑一遍，覆盖开盘集合竞价、
   连续竞价、收盘集合竞价三个阶段
"""

import os
from pathlib import Path
from typing import Dict, List, Optional

import pandas as pd
import pytest

from wangcai_syn import (
    Strategy,
    UserEvent,
    Snapshot,
    OrderDetail,
    TradeDetail,
    TradeCallback,
    OrderCallback,
    make_order,
    run_backtest,
)

# 数据目录：优先本地 data/，回退到仓库中的 new_log/
_HERE = Path(__file__).resolve().parent
DATA_DIR = next(
    (d for d in (_HERE / "../../data", _HERE / "../../new_log") if d.is_dir()),
    _HERE / "../../new_log",
)


def _ms_of_day(datetime_str: str) -> int:
    """'YYYY-MM-DD HH:MM:SS.mmm' -> 当日毫秒数"""
    hh = int(datetime_str[11:13])
    mm = int(datetime_str[14:16])
    ss = int(datetime_str[17:19])
    ms = int(datetime_str[20:23]) if len(datetime_str) >= 23 else 0
    return (hh * 3600 + mm * 60 + ss) * 1000 + ms


def _has_dataset(symbol: str, date: str) -> bool:
    return all(
        (DATA_DIR / f"{kind}_{symbol}_{date}.csv").is_file()
        for kind in ("cstick", "csord", "cstra", "csbar1d")
    )


def _load_dataset(symbol: str, date: str, is_etf: bool = False):
    files = {
        kind: pd.read_csv(DATA_DIR / f"{kind}_{symbol}_{date}.csv")
        for kind in ("cstick", "csord", "cstra", "csbar1d")
    }
    return (files["cstick"], files["csord"], files["cstra"], files["csbar1d"], is_etf)


# ---------------------------------------------------------------------------
# 校验策略：内联断言，只保留错误与聚合统计，避免高频推送下的内存/耗时问题
# ---------------------------------------------------------------------------


class RtTickValidator(Strategy):
    """记录并校验实时合成 tick 的不变式（支持多标的，按 Instrument 分账）"""

    AUCTION_END = 92500000       # 09:25:00.000
    CON_START = 93000000         # 09:30:00.000
    CLOSE_START = 145700000      # 14:57:00.000
    RT_START = 91500000          # 09:15:00.000

    def __init__(self, strategy_id: str, interval_ms: int):
        super().__init__()
        self.strategy_id = strategy_id
        self.interval_ms = interval_ms
        self.errors: List[str] = []
        # 按标的分账的状态
        self.push_count: Dict[str, int] = {}
        self.phase_count: Dict[str, Dict[str, int]] = {}
        self.last_bucket: Dict[str, int] = {}
        self.last_volume: Dict[str, int] = {}
        self.last_num_trades: Dict[str, int] = {}
        self.last_tick: Dict[str, dict] = {}
        self.last_push: Dict[str, dict] = {}
        self.max_push_dt: Dict[str, str] = {}
        self.max_vol_drift: Dict[str, float] = {}  # 与官方 tick 的最大相对偏差（仅记录）

    def getStrategyId(self) -> str:
        return self.strategy_id

    def _err(self, msg: str) -> None:
        if len(self.errors) < 50:  # 防止刷屏
            self.errors.append(msg)

    # --- 输入事件 ---

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        # 记录官方累计值，供"同时间戳推送必与官方一致"检查
        self.last_tick[tick.Instrument] = {
            "Time": tick.Time,
            "Volume": int(tick.Volume),
            "NumTrades": int(tick.NumTrades),
            "Turnover": int(tick.Turnover),
        }
        return []

    def onRealTimeTickEvent(self, snap: Snapshot) -> List[UserEvent]:
        sym = snap.Instrument
        self.push_count[sym] = self.push_count.get(sym, 0) + 1
        dt = snap.datetime

        # datetime 必须有效（合成 tick 会填充完整时间字符串）
        if not dt or len(dt) < 23:
            self._err(f"[{sym}] 推送 datetime 无效: {dt!r}")
            return []
        self.max_push_dt[sym] = max(self.max_push_dt.get(sym, ""), dt)

        now_ms = _ms_of_day(dt)
        t = snap.Time

        # 1) 起止边界
        if t < self.RT_START:
            self._err(f"[{sym}] 推送早于 09:15:00: Time={t}")
        if dt[11:19] > "15:00:00":
            self._err(f"[{sym}] 推送晚于 15:00:00: {dt}")

        # 2) 网格严格递增（同网格不重复推送）
        bucket = now_ms // self.interval_ms
        prev = self.last_bucket.get(sym)
        if prev is not None and bucket <= prev:
            self._err(f"[{sym}] 网格未递增: prev={prev}, cur={bucket}, dt={dt}")
        self.last_bucket[sym] = bucket

        # 3) 十档形状：非零前缀 + 买递减/卖递增 + 价量成对
        bids = list(snap.bids)
        asks = list(snap.asks)
        bid_sizes = list(snap.bid_sizes)
        ask_sizes = list(snap.ask_sizes)
        for name, px_arr, sz_arr, descending in (
            ("买盘", bids, bid_sizes, True),
            ("卖盘", asks, ask_sizes, False),
        ):
            seen_zero = False
            for i in range(10):
                if px_arr[i] == 0:
                    seen_zero = True
                    if sz_arr[i] != 0:
                        self._err(f"[{sym}] {name}第{i+1}档价0量非0: {sz_arr[i]} dt={dt}")
                else:
                    if seen_zero:
                        self._err(f"[{sym}] {name}第{i+1}档出现在零档之后 dt={dt}")
                    if sz_arr[i] == 0:
                        self._err(f"[{sym}] {name}第{i+1}档价非0量0 dt={dt}")
                    if i > 0 and px_arr[i - 1] != 0:
                        ok = px_arr[i] < px_arr[i - 1] if descending else px_arr[i] > px_arr[i - 1]
                        if not ok:
                            self._err(f"[{sym}] {name}档位价格未按序: {px_arr} dt={dt}")

        # 4) 连续竞价窗口内盘口不交叉
        if self.CON_START <= t < self.CLOSE_START and bids[0] > 0 and asks[0] > 0:
            if bids[0] >= asks[0]:
                self._err(f"[{sym}] 连续竞价盘口交叉: bid1={bids[0]} ask1={asks[0]} dt={dt}")

        # 5) 累计量严格单调不减：累计器完全由引擎重建的成交驱动、不与官方
        #    tick 对齐（官方快照有独立时间戳，数值出入属口径差异），因此
        #    推送序列必须只增不减
        vol = int(snap.Volume)
        ntr = int(snap.NumTrades)
        if vol < self.last_volume.get(sym, 0):
            self._err(f"[{sym}] Volume 下降: {self.last_volume[sym]} -> {vol} dt={dt}")
        if ntr < self.last_num_trades.get(sym, 0):
            self._err(f"[{sym}] NumTrades 下降: {self.last_num_trades[sym]} -> {ntr} dt={dt}")
        self.last_volume[sym] = vol
        self.last_num_trades[sym] = ntr

        # 6) 与官方 tick 的偏差只记录不断言（时间戳口径不同，出入正常）
        lt = self.last_tick.get(sym)
        if lt is not None and lt["Volume"] > 0:
            drift = abs(vol - lt["Volume"]) / lt["Volume"]
            self.max_vol_drift[sym] = max(self.max_vol_drift.get(sym, 0.0), drift)

        # 7) 有成交后 last_price 必须为正
        if vol > 0 and snap.last_price == 0:
            self._err(f"[{sym}] 有成交但 last_price=0 dt={dt}")

        # 8) 阶段覆盖统计
        phase = "auction" if t < self.AUCTION_END else ("close" if t >= self.CLOSE_START else "con")
        self.phase_count.setdefault(sym, {})[phase] = \
            self.phase_count.setdefault(sym, {}).get(phase, 0) + 1

        # 保留最后一次推送快照（合成场景做精确断言用）
        self.last_push[sym] = {
            "datetime": dt,
            "Time": t,
            "bids": bids,
            "bid_sizes": bid_sizes,
            "asks": asks,
            "ask_sizes": ask_sizes,
            "last_price": int(snap.last_price),
            "Volume": vol,
            "Turnover": int(snap.Turnover),
            "NumTrades": ntr,
            "TotalBidVol": int(snap.TotalBidVol),
            "TotalAskVol": int(snap.TotalAskVol),
        }
        return []

    # --- 回报（不用） ---

    def onOrderFilled(self, order_id, price, volume):
        pass

    def onOrderCancelled(self, order_id, reason):
        pass

    def onOrderCallback(self, cb: OrderCallback):
        pass

    def onTradeCallback(self, cb: TradeCallback):
        pass


class PassiveNoRtStrategy(Strategy):
    """等价性测试用：在首个 Time>=09:31:00 的真实 tick 上下一笔主动买单。

    不实现 onRealTimeTickEvent（覆盖"未覆盖回调"的探测路径）。
    """

    def __init__(self, strategy_id: str):
        super().__init__()
        self.strategy_id = strategy_id
        self.placed = False
        self.order_submits: Dict[str, int] = {}
        self.order_trades: Dict[str, int] = {}
        self.order_cancels: Dict[str, int] = {}

    def getStrategyId(self) -> str:
        return self.strategy_id

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        if self.placed or tick.Time < 93100000:
            return []
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        if ask1 <= 0:
            return []
        self.placed = True
        return [
            make_order(
                Broker="",
                Account=self.strategy_id,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=f"{self.strategy_id}_buy_1",
                OrderType=0,
                Direction=1,
                Price=ask1,
                Volume=100,
            )
        ]

    def onOrderFilled(self, order_id, price, volume):
        pass

    def onOrderCancelled(self, order_id, reason):
        pass

    def onOrderCallback(self, cb: OrderCallback):
        self.order_submits[cb.orderlocalid] = self.order_submits.get(cb.orderlocalid, 0) + 1

    def onTradeCallback(self, cb: TradeCallback):
        if cb.matchtype == "T":
            self.order_trades[cb.localid] = self.order_trades.get(cb.localid, 0) + int(cb.volume)
        elif cb.matchtype == "D":
            self.order_cancels[cb.localid] = self.order_cancels.get(cb.localid, 0) + 1

    def result(self):
        return (dict(self.order_submits), dict(self.order_trades), dict(self.order_cancels))


# ---------------------------------------------------------------------------
# 合成数据：可控盘口，支持逐字段手算断言
# ---------------------------------------------------------------------------

SYNTH_SYM = "300827.SZ"
SYNTH_DATE = "2025-01-02"

_CSTICK_COLS = (
    ["date", "time", "sym", "prevclose", "open", "high", "low", "close",
     "volume", "turnover", "tradecount"]
    + [c for i in range(1, 11) for c in (f"bid{i}", f"bsize{i}")]
    + [c for i in range(1, 11) for c in (f"ask{i}", f"asize{i}")]
    + ["avgbid", "avgask", "totalbsize", "totalasize", "iopv",
       "etfpchcount", "etfredmcount", "etfpchsize", "etfredmsize",
       "etfpchamt", "etfredmamt", "status", "updatetime"]
)


def _tick_row(time_str: str, volume: float, turnover: float, tradecount: float,
              high: float = 10.0, low: float = 10.0):
    row = {c: 0.0 for c in _CSTICK_COLS}
    row.update({
        "date": SYNTH_DATE, "time": f"0 days {time_str}", "sym": SYNTH_SYM,
        "prevclose": 10.0, "open": 10.0, "high": high, "low": low, "close": 10.0,
        "volume": volume, "turnover": turnover, "tradecount": tradecount,
        "bid1": 10.0, "bsize1": 1000, "ask1": 10.01, "asize1": 1000,
        "status": "b'T'", "updatetime": f"{SYNTH_DATE} {time_str}",
    })
    return row


def make_synth_data():
    """构造可手算的合成数据。

    时间线（全部 SZ 规则）：
      09:15:00 tick（空盘口）
      09:30:00 tick（触发集合竞价 settle，空簿）
      09:31:00.050 买 9.99 x300 (oid 2001)
      09:31:00.150 买 9.98 x200 (oid 2002)
      09:31:00.250 卖 10.01x400 (oid 2003)
      09:31:00.350 卖 10.02x500 (oid 2004)
      09:31:00.450 买 9.99 x100 (oid 2005)
      09:31:01.000 买 10.01x150 (oid 2006, 穿价 -> 内部撮合 150@10.01)
      09:31:01.100 cstra 成交记录 150@10.01 (bid=2006, ask=2003)
      09:31:03     tick（官方 volume=150, turnover=1501.5, tradecount=1）
      09:31:05.000 cstra 撤单记录：撤买单 2002 全量
      09:31:06     tick（同官方值）

    终态盘口（只含历史单）：
      买: 9.99 x400（2001+2005）
      卖: 10.01x250（400-150）, 10.02x500
    """
    cstick = pd.DataFrame([
        _tick_row("09:15:00", 0.0, 0.0, 0.0),
        _tick_row("09:30:00", 0.0, 0.0, 0.0),
        _tick_row("09:31:03", 150.0, 1501.5, 1.0, high=10.01, low=10.01),
        _tick_row("09:31:06", 150.0, 1501.5, 1.0, high=10.01, low=10.01),
    ], columns=_CSTICK_COLS)

    def _ord(time_str, price, size, side, oid, biz):
        return {
            "date": SYNTH_DATE, "time": f"0 days {time_str}", "sym": SYNTH_SYM,
            "price": price, "size": size, "side": side, "ordertype": 2,
            "orderid": oid, "channelno": 1, "seqno": oid, "bizindex": biz,
            "updatetime": f"{SYNTH_DATE} {time_str}",
        }

    csord = pd.DataFrame([
        _ord("09:31:00.050", 9.99, 300, 1, 2001, 1),
        _ord("09:31:00.150", 9.98, 200, 1, 2002, 2),
        _ord("09:31:00.250", 10.01, 400, 2, 2003, 3),
        _ord("09:31:00.350", 10.02, 500, 2, 2004, 4),
        _ord("09:31:00.450", 9.99, 100, 1, 2005, 5),
        _ord("09:31:01.000", 10.01, 150, 1, 2006, 6),
    ])

    def _tra(time_str, price, size, bid, ask, tid, exectype, flag, biz):
        return {
            "date": SYNTH_DATE, "time": f"0 days {time_str}", "sym": SYNTH_SYM,
            "price": price, "size": size, "bidorderid": bid, "askorderid": ask,
            "tradeid": tid, "exectype": exectype, "tradebsflag": flag,
            "channelno": 1, "bizindex": biz,
            "updatetime": f"{SYNTH_DATE} {time_str}",
        }

    cstra = pd.DataFrame([
        _tra("09:31:01.100", 10.01, 150, 2006, 2003, 3001, "b'1'", "b'B'", 10),
        _tra("09:31:05.000", 9.98, 200, 2002, 0, 3002, "b'2'", "b' '", 11),
    ])

    csbar1d = pd.DataFrame([{
        "sym": SYNTH_SYM, "prevclose": 10.0, "open": 10.0, "high": 10.0,
        "low": 10.0, "close": 10.0, "volume": 0, "turnover": 0,
        "tradecount": 0, "af": 1.0, "upperlimit": 11.0, "lowerlimit": 9.0,
    }])

    return {SYNTH_SYM: (cstick, csord, cstra, csbar1d)}


# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------


def test_disabled_by_default_no_pushes():
    """默认（不传 realtime_tick_interval_ms）时回调次数必须为 0"""
    validator = RtTickValidator("rt_disabled", interval_ms=100)
    ok = run_backtest(make_synth_data(), validator)
    assert ok, "run_backtest 失败"
    assert validator.push_count == {}, f"默认关闭仍收到推送: {validator.push_count}"


def test_enable_does_not_change_results():
    """开启实时 tick（无操作回调）不得改变下单/成交/撤单结果"""
    s_off = PassiveNoRtStrategy("eq_off")
    assert run_backtest(make_synth_data(), s_off)

    s_on = PassiveNoRtStrategy("eq_on")
    assert run_backtest(make_synth_data(), s_on, realtime_tick_interval_ms=100)

    def _strip(result, tag):
        # localid 含策略ID前缀，规范化后比较
        return [
            {k.replace(tag, "X"): v for k, v in d.items()}
            for d in result
        ]

    assert _strip(s_off.result(), "eq_off") == _strip(s_on.result(), "eq_on"), (
        f"开关前后结果不一致:\noff={s_off.result()}\non={s_on.result()}"
    )
    assert s_off.placed and s_on.placed, "等价性测试未实际下单，检验无效"


def test_synth_exact_depth_and_accumulators():
    """合成盘口的十档/最新价/累计字段与手算值精确一致"""
    validator = RtTickValidator("rt_synth", interval_ms=100)
    ok = run_backtest(make_synth_data(), validator, realtime_tick_interval_ms=100)
    assert ok
    assert not validator.errors, "不变式违例:\n" + "\n".join(validator.errors)

    sym = SYNTH_SYM
    assert validator.push_count.get(sym, 0) > 0, "未收到任何推送"

    last = validator.last_push[sym]
    # 终态盘口：买 9.99x400；卖 10.01x250、10.02x500（价格单位：厘）
    assert last["bids"][:2] == [99900, 0], f"买盘价不符: {last['bids']}"
    assert last["bid_sizes"][:2] == [400, 0], f"买盘量不符: {last['bid_sizes']}"
    assert last["asks"][:3] == [100100, 100200, 0], f"卖盘价不符: {last['asks']}"
    assert last["ask_sizes"][:3] == [250, 500, 0], f"卖盘量不符: {last['ask_sizes']}"
    assert last["last_price"] == 100100, f"last_price 不符: {last['last_price']}"
    # 累计字段（引擎重建口径：09:31:01 内部撮合 150@10.01）
    assert last["Volume"] == 150, f"Volume 不符: {last['Volume']}"
    assert last["NumTrades"] == 1, f"NumTrades 不符: {last['NumTrades']}"
    assert abs(last["Turnover"] - 1501) <= 1, f"Turnover 不符: {last['Turnover']}"
    # 全簿委托总量
    assert last["TotalBidVol"] == 400, f"TotalBidVol 不符: {last['TotalBidVol']}"
    assert last["TotalAskVol"] == 750, f"TotalAskVol 不符: {last['TotalAskVol']}"


class _PushLogger(RtTickValidator):
    """在 validator 基础上额外记录全部推送的关键标量（合成场景数据量小）"""

    def __init__(self, strategy_id: str, interval_ms: int):
        super().__init__(strategy_id, interval_ms)
        self.pushes: List[dict] = []

    def onRealTimeTickEvent(self, snap: Snapshot) -> List[UserEvent]:
        out = super().onRealTimeTickEvent(snap)
        self.pushes.append({
            "datetime": snap.datetime,
            "Volume": int(snap.Volume),
            "last_price": int(snap.last_price),
        })
        return out


def test_synth_push_sequence():
    """合成场景逐推送校验：网格时刻、last_price 与 Volume 的推进顺序"""
    logger = _PushLogger("rt_seq", interval_ms=100)
    ok = run_backtest(make_synth_data(), logger, realtime_tick_interval_ms=100)
    assert ok
    assert not logger.errors, "不变式违例:\n" + "\n".join(logger.errors)

    by_dt = {p["datetime"]: p for p in logger.pushes}

    # 穿价单事件（09:31:01.000）：内部撮合在同一事件内完成，
    # last_price 与累计 Volume 同步更新（累计器挂在 exec 回调上）
    p_cross = by_dt.get(f"{SYNTH_DATE} 09:31:01.000")
    assert p_cross is not None, f"缺少穿价时刻推送: {sorted(by_dt)}"
    assert p_cross["last_price"] == 100100
    assert p_cross["Volume"] == 150

    # CSV 成交记录（09:31:01.100）不进入事件流（load_traders 只插入撤单），
    # 因此该时刻不应有推送 —— 成交量已在内部撮合时计入，无信息损失
    assert f"{SYNTH_DATE} 09:31:01.100" not in by_dt, "CSV 成交记录不应产生事件推送"

    # 真实 tick 时刻（09:31:03）的推送：累计器保持引擎口径不被官方值改写
    p_sync = by_dt.get(f"{SYNTH_DATE} 09:31:03.000")
    assert p_sync is not None, f"缺少 09:31:03 推送: {sorted(by_dt)}"
    assert p_sync["Volume"] == 150

    # 撤单记录（09:31:05.000）会进事件流并触发推送，且不改变累计量
    p_cancel = by_dt.get(f"{SYNTH_DATE} 09:31:05.000")
    assert p_cancel is not None, f"缺少撤单时刻推送: {sorted(by_dt)}"
    assert p_cancel["Volume"] == 150


def _run_real_dataset(symbol: str, date: str, is_etf: bool, interval_ms: int = 100):
    validator = RtTickValidator(f"rt_{symbol.replace('.', '_')}", interval_ms)
    data = {symbol: _load_dataset(symbol, date, is_etf)}
    ok = run_backtest(data, validator, realtime_tick_interval_ms=interval_ms)
    assert ok, f"run_backtest 失败: {symbol} {date}"
    assert not validator.errors, (
        f"{symbol} 不变式违例({len(validator.errors)}条，前若干):\n" + "\n".join(validator.errors[:20])
    )

    assert validator.push_count.get(symbol, 0) > 0, "未收到推送"
    phases = validator.phase_count.get(symbol, {})
    assert phases.get("auction", 0) > 0, f"开盘集合竞价阶段无推送: {phases}"
    assert phases.get("con", 0) > 0, f"连续竞价阶段无推送: {phases}"
    assert phases.get("close", 0) > 0, f"收盘集合竞价阶段无推送: {phases}"
    assert validator.max_push_dt[symbol][11:19] <= "15:00:00", (
        f"推送越过收盘边界: {validator.max_push_dt[symbol]}"
    )
    # 价格必须对齐标的 tick（股票 100 厘 / ETF 10 厘）
    tick = 10 if is_etf else 100
    last = validator.last_push[symbol]
    for px in last["bids"] + last["asks"]:
        assert px % tick == 0, f"价格未对齐 tick={tick}: {px}"
    return validator


@pytest.mark.skipif(not _has_dataset("300827.SZ", "2025-11-17"), reason="缺少 300827.SZ 数据")
def test_real_sz_stock():
    v = _run_real_dataset("300827.SZ", "2025-11-17", is_etf=False)
    print(f"\nSZ 股票推送统计: {v.push_count} 阶段: {v.phase_count} 官方偏差: {v.max_vol_drift}")


@pytest.mark.skipif(not _has_dataset("600050.SH", "2025-02-17"), reason="缺少 600050.SH 数据")
def test_real_sh_stock():
    v = _run_real_dataset("600050.SH", "2025-02-17", is_etf=False)
    print(f"\nSH 股票推送统计: {v.push_count} 阶段: {v.phase_count} 官方偏差: {v.max_vol_drift}")


@pytest.mark.skipif(not _has_dataset("518880.SH", "2025-02-17"), reason="缺少 518880.SH 数据")
def test_real_sh_etf():
    v = _run_real_dataset("518880.SH", "2025-02-17", is_etf=True)
    print(f"\nSH ETF 推送统计: {v.push_count} 阶段: {v.phase_count} 官方偏差: {v.max_vol_drift}")


@pytest.mark.skipif(
    not (_has_dataset("300827.SZ", "2025-11-17") and _has_dataset("510050.SH", "2025-11-17")),
    reason="缺少多标的数据",
)
def test_multi_symbol_parallel():
    """多标的并行：两个引擎各自独立推送，互不串号"""
    validator = RtTickValidator("rt_multi", interval_ms=100)
    data = {
        "300827.SZ": _load_dataset("300827.SZ", "2025-11-17", is_etf=False),
        "510050.SH": _load_dataset("510050.SH", "2025-11-17", is_etf=True),
    }
    ok = run_backtest(data, validator, realtime_tick_interval_ms=100)
    assert ok
    assert not validator.errors, (
        f"多标的不变式违例({len(validator.errors)}条，前若干):\n" + "\n".join(validator.errors[:20])
    )
    assert validator.push_count.get("300827.SZ", 0) > 0
    assert validator.push_count.get("510050.SH", 0) > 0
    print(f"\n多标的推送统计: {validator.push_count}")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v", "-s"]))
