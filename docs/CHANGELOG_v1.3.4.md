# 1.3.4 更新：真实成交替代模式 (Real Trade Match Mode)

## 1. 功能总结

新增**真实成交替代模式**，用于更真实地估计交易成本：

- **被动单只用真实成交量成交**：虚拟被动单只有在同价位发生了真实历史成交时才能成交，且成交量受真实成交量限制，用完就没有了
- **排队位置模拟**：下单时记录前方排队量，必须等前面的量被消耗后才轮到你
- **主动单自动走严格模式**：开启 RT 模式会自动启用严格主动单模式
- **支持部分成交**：被动单可以多次部分成交，逐步累积
- **可配置开关**：默认关闭

---

## 2. 使用方法

```python
from wangcai_syn import run_backtest

# 启用真实成交替代模式
run_backtest(
    data_dict=data,
    strategy=my_strategy,
    real_trade_match_mode=True  # 开启RT模式
)
```

参数说明：
- `real_trade_match_mode=True`：开启RT模式，主动单自动走严格模式，被动单使用真实成交池匹配
- `real_trade_match_mode=False`（默认）：关闭RT模式，使用原有撮合逻辑

注意：`real_trade_match_mode` 与 `strict_active_order_mode` 互斥，开启 RT 模式会自动启用严格主动单。

---

## 3. 核心概念

### 3.1 被动单排队模型

当你下一个被动单（价格不穿越对手盘）时，引擎会记录三个关键信息：

| 字段 | 含义 |
|------|------|
| `queue_position` | 下单时同价位同侧历史挂单总量（你前面排了多少量） |
| `pool_at_entry` | 下单时真实成交池的累积值（基线，只算之后的增量） |
| `frontier_hist_order_id` | 下单时同价位最后一个历史订单ID（撤单边界） |

### 3.2 成交判定公式

每次有新的真实成交事件时，引擎计算：

```
pool_delta = 当前真实成交池累积值 - pool_at_entry    （入场后的新增成交量）
available  = pool_delta - queue_position - 前面虚拟单已成交总量
fill_qty   = min(available, 订单剩余量)
```

`available > 0` 时才成交，且成交量不超过可用量。

### 3.3 撤单推进规则

历史订单被**撤单**（非成交）移除时：
- 只有撤掉的历史单 ID <= 你入场时的边界 ID，才会减少你的 `queue_position`
- 你入场之后新来的历史单撤掉不影响你的排队位置

历史订单被**成交**移除时：
- 不调整 `queue_position`，因为成交量会通过真实成交池自然累加

### 3.4 被动侧判定

真实成交事件中，如何判断买方/卖方谁是被动方：
- 规则：`bidorderid < askorderid` → 买方被动；反之卖方被动
- 强约束：`bidorderid` 和 `askorderid` 必须都有效，缺失直接报错终止

### 3.5 主动单处理

RT 模式下主动单完全复用严格主动单模式：
- 只能成交对手方订单簿上实际存在的量
- 吃掉的历史订单记入"欠债"，欠债未清前禁止下新主动单
- 市场量不足时剩余部分自动撤单

---

## 4. 与其他模式的关系

| 模式 | 主动单行为 | 被动单行为 |
|------|-----------|-----------|
| 默认模式 | 立即全部成交 | 等对手方历史订单被移除后成交 |
| 严格主动单模式 | 只吃实际可用量 + 欠债限制 | 同默认 |
| **真实成交替代模式** | 自动使用严格模式 | **只用真实成交量，排队等待** |

---

## 5. 测试覆盖

测试文件位于 `tests/real_trade_mode/`：

| 脚本 | 测试内容 |
|------|----------|
| `run_real_trade_mode_suite.py` | 4个场景覆盖测试（自动断言） |
| `run_real_trade_mode_test.py` | 基础冒烟测试 |

覆盖场景：

| 场景 | 验证内容 |
|------|----------|
| case_01 | RT 开启时主动单自动联动严格模式，欠债拦截生效 |
| case_02 | RT 关闭基线对照，主动单不受限制 |
| case_03 | RT 被动买/卖单双边撤单路径正常 |
| case_04 | 被动单成交时间晚于下单时间（无倒灌） |

运行方式：
```bash
cd tests/real_trade_mode
python3 run_real_trade_mode_suite.py
```

---

## 6. 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `include/con_auction_engine.hpp` | `RtVirtualOrder` 结构、RT 方法声明、RT 数据成员 |
| `src/con_auction_engine.cpp` | 6 个 RT 方法实现 + 入队/撤单/成交分支 |
| `include/backtest_engine.hpp` | `setRealTradeMatchMode` / `determineBuyPassive` 声明 |
| `src/backtest_engine.cpp` | 被动侧判定、事件时序、部分成交映射生命周期 |
| `src/multi_backtest_engine.cpp` | `setRealTradeMatchMode` 转发 |
| `bindings/wangcai_py.cpp` | Python 接口绑定 |
| `wangcai_syn/utils.py` | `run_backtest` 新增 `real_trade_match_mode` 参数 |
