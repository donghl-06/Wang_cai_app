"""下单延迟功能真实数据冒烟:在真实行情事件流上验证延迟语义。

用法(项目根目录):
    .venv/bin/python tests/price_cage/latency_real_smoke.py [SYM DATE]...

默认跑 000001.SZ 2024-11-01(adata_logs,深市拒单时代)与
600050.SH 2025-02-17(new_log,沪市)。

验证点(对照合成数据单元测试 tests/test_order_latency.py):
1. latency=0:策略单即刻进簿,下单确认回调时间==触发事件时刻(旧行为不变);
2. latency=50ms:回调时间 >= 触发时刻+50ms(事件粒度:release 落在两事件
   之间时随下一事件进簿,故只会更晚不会更早);
3. 撤单同延迟通道保 FIFO:延迟队列中的订单可撤,不得报"找不到订单";
4. 开/关延迟,历史成交重建结果完全一致(延迟只作用策略单,不污染行情回放)。
"""

import os
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_real_stocks import load_files, real_agg  # noqa: E402

from wangcai_syn import Strategy, run_backtest  # noqa: E402
from wangcai_syn import make_order, make_cancel  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent


def _ms_of_hhmmssfff(t: int) -> int:
    """HHMMSSfff 整数 -> 当日毫秒"""
    hh, rem = divmod(int(t), 10_000_000)
    mm, rem = divmod(rem, 100_000)
    ss, ms = divmod(rem, 1000)
    return ((hh * 60 + mm) * 60 + ss) * 1000 + ms


def _ms_of_cb(timestr: str) -> int:
    m = re.search(r"(\d{2}):(\d{2}):(\d{2})\.(\d{3})", timestr)
    assert m, f"回调时间格式异常: {timestr!r}"
    hh, mm, ss, ms = map(int, m.groups())
    return ((hh * 60 + mm) * 60 + ss) * 1000 + ms


class LatencySmoke(Strategy):
    """第 FIRE_AT 条委托事件按最近 tick 买一价下买单,FIRE_AT+5 条时撤单"""

    FIRE_AT = 1000

    def __init__(self, sid: str, symbol: str):
        super().__init__()
        self.sid = sid
        self.symbol = symbol
        self.n_ord = 0
        self.last_bid1 = 0
        self.fire_ms = None        # 下单触发时刻(事件时间,ms)
        self.cancel_fire_ms = None
        self.order_cb_ms = None    # 下单确认回调时间(ms)
        self.cancel_cb_seen = False
        self.fills = []
        self.hist_trades = []      # 连续段历史重建成交

    def getStrategyId(self):
        return self.sid

    def onOrderEvent(self, o):
        self.n_ord += 1
        evs = []
        if self.n_ord == self.FIRE_AT and self.last_bid1 > 0:
            self.fire_ms = _ms_of_hhmmssfff(o.Time)
            # 买一价下 2 档挂被动买:仍在笼子内(>=bid1*0.98),5 条事件内几乎
            # 不会被下穿成交,保证随后撤单测试的对象仍在簿/延迟队列中
            evs.append(make_order(Broker="", Account=self.sid, Exchange=1,
                                  Instrument=self.symbol, OrderLocalID="SMK1",
                                  OrderType=0, Direction=1,
                                  Price=int(self.last_bid1 * 10000) - 200,
                                  Volume=100))
        elif self.n_ord == self.FIRE_AT + 5:
            self.cancel_fire_ms = _ms_of_hhmmssfff(o.Time)
            evs.append(make_cancel(Broker="", Account=self.sid, Exchange=1,
                                   Instrument=self.symbol,
                                   CancelOrderLocalID="C_SMK1",
                                   OrderLocalID="SMK1"))
        return evs

    def onTradeEvent(self, t):
        if t.ExecType == '1':
            hhmmss = t.Time // 1000
            if 93000 <= hhmmss < 145700:
                self.hist_trades.append((int(t.ChannelNo), int(t.BuyNo),
                                         int(t.SellNo), int(t.Price),
                                         int(t.Volume)))
        return []

    def onTickEvent(self, s):
        return []

    def onEventSnapshot(self, snap):
        if snap.bids and snap.bids[0] > 0:
            self.last_bid1 = snap.bids[0] / 10000.0
        return []

    def onOrderFilled(self, *a): pass
    def onOrderCancelled(self, *a): pass

    def onOrderCallback(self, cb):
        if cb.orderlocalid == "SMK1" and self.order_cb_ms is None:
            self.order_cb_ms = _ms_of_cb(cb.time)
        if cb.orderlocalid == "C_SMK1":
            self.cancel_cb_seen = True

    def onTradeCallback(self, cb):
        if cb.localid == "SMK1" and cb.matchtype == 'T':
            self.fills.append((cb.volume, cb.price, cb.matchtime))


def run_case(sym, date, latency_ms):
    files = load_files(sym, date)
    s = LatencySmoke(f"SMK{latency_ms}", sym)
    kw = {"event_snapshot_enabled": True}
    if latency_ms > 0:
        kw.update(order_latency_enabled=True, order_latency_ms=latency_ms)
    t0 = time.time()
    # C++ 侧打印直接写 fd 1,dup2 重定向捕获以检查撤单失败告警
    old_fd = os.dup(1)
    tmp = tempfile.TemporaryFile()
    os.dup2(tmp.fileno(), 1)
    try:
        ok = run_backtest({sym: (files["cstick"], files["csord"], files["cstra"],
                                 files["csbar1d"])}, s, **kw)
    finally:
        os.dup2(old_fd, 1)
        os.close(old_fd)
    tmp.seek(0)
    s.engine_log = tmp.read().decode(errors="replace")
    tmp.close()
    assert ok, f"run_backtest 失败 {sym} {date}"
    assert s.fire_ms is not None, f"未触发下单 {sym}(tick 买一价缺失?)"
    return s, real_agg(files["cstra"]), time.time() - t0


def check(sym, date):
    print(f"===== {sym} {date} =====")
    s0, agg_real, el0 = run_case(sym, date, 0)
    s50, _, el50 = run_case(sym, date, 50)

    fails = []

    # 1. latency=0 回调即刻(<= 触发时刻,允许等于;旧行为同步回调)
    print(f"[0ms]  触发@{s0.fire_ms}ms 回调@{s0.order_cb_ms}ms "
          f"成交{len(s0.fills)}笔 ({el0:.0f}s)")
    if s0.order_cb_ms is None:
        fails.append("latency=0 未收到下单确认回调")
    elif s0.order_cb_ms > s0.fire_ms:
        fails.append(f"latency=0 回调应即刻发出: 回调 {s0.order_cb_ms} > "
                     f"触发 {s0.fire_ms}")

    # 2. latency=50ms 回调 >= 触发+50ms
    print(f"[50ms] 触发@{s50.fire_ms}ms 回调@{s50.order_cb_ms}ms "
          f"(delay={s50.order_cb_ms - s50.fire_ms}ms) "
          f"成交{len(s50.fills)}笔 ({el50:.0f}s)")
    if s50.order_cb_ms is None:
        fails.append("latency=50ms 未收到下单确认回调")
    elif s50.order_cb_ms < s50.fire_ms + 50:
        fails.append(f"latency=50ms 回调提前: {s50.order_cb_ms} < "
                     f"{s50.fire_ms}+50")
    if not s50.fills:
        if "[策略撤单失败]" in s50.engine_log:
            fails.append("latency=50ms 延迟队列中的订单撤单被判找不到")
        if "[策略撤单]" not in s50.engine_log:
            fails.append("latency=50ms 撤单未生效(无[策略撤单]记录)")
    if not s0.fills and "[策略撤单失败]" in s0.engine_log:
        fails.append("latency=0 撤单异常失败")

    # 3. 历史成交重建:开/关延迟完全一致
    def agg(trades):
        d = {}
        for k in trades:
            d[k[:4]] = d.get(k[:4], 0) + k[4]
        return d
    a0, a50 = agg(s0.hist_trades), agg(s50.hist_trades)
    if a0 != a50:
        fails.append(f"开/关延迟历史重建不一致: {len(a0)} vs {len(a50)} 键")
    # 4. 引擎重建 ⊇ 真实(该只次的既有校验口径,顺带确认引擎本身无恙)
    miss = {k: v for k, v in agg_real.items() if a0.get(k, 0) < v}
    print(f"[hist] 重建键 {len(a0)},真实键 {len(agg_real)},量不足键 {len(miss)}"
          f"{'(该只次已知 fail)' if miss else ''}")
    return fails


def main():
    args = sys.argv[1:]
    pairs = list(zip(args[::2], args[1::2])) or [
        ("000001.SZ", "2024-11-01"),
        ("600050.SH", "2025-02-17"),
    ]
    all_fails = []
    for sym, date in pairs:
        try:
            all_fails += [f"{sym}: {f}" for f in check(sym, date)]
        except AssertionError as e:
            all_fails.append(f"{sym}: {e}")
    print("\n===== 冒烟结论 =====")
    if all_fails:
        for f in all_fails:
            print("FAIL:", f)
        sys.exit(1)
    print("全部通过")


if __name__ == "__main__":
    main()
