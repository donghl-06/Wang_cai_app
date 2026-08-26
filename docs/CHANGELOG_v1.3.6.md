# 1.3.6 更新：下单回调队列信息（Queue Info Callback）

## 1. 功能总结

本版本重点新增**下单回调队列信息**能力，用于帮助策略精确观察“订单提交瞬间”的历史队列位置，同时保持原有回调字段完全兼容。

- **新增可选开关**：`queue_info_enabled`（默认关闭）
- **新增回调字段**：`queue_ahead_count`、`queue_ahead_volume`、`prev_order_ids`
- **只作用于用户自己的下单回调**：不改变历史行情回调语义
- **原有字段不变**：`price`、`volume`、`orderlocalid` 等旧字段全部保留

---

## 2. 使用方法

```python
from wangcai_syn import run_backtest

run_backtest(
    data_dict=data,
    strategy=my_strategy,
    queue_info_enabled=True,  # 开启下单回调队列信息
)
```

在 `onOrderCallback` 中读取新增字段：

```python
def onOrderCallback(self, cb):
    print(cb.orderlocalid, cb.price, cb.volume)
    print(cb.queue_ahead_count, cb.queue_ahead_volume, cb.prev_order_ids)
```

---

## 3. 字段定义

| 字段 | 类型 | 含义 |
|------|------|------|
| `queue_ahead_count` | `int` | 提交时，同价位同侧、位于你之前的历史订单数量 |
| `queue_ahead_volume` | `int` | 提交时，同价位同侧、位于你之前的历史订单总量 |
| `prev_order_ids` | `list[int]` | 最近 3 个前序历史订单 ID（真实 `orderid`，从近到远） |

说明：
- 关闭开关时，`queue_ahead_count=-1`、`queue_ahead_volume=-1`、`prev_order_ids=[]`
- 返回的订单 ID 为数据源真实订单 ID（`input_id`），不是系统生成 ID

---

## 4. 设计与兼容性

### 4.1 默认零侵入

- `queue_info_enabled=False` 时，不额外采集队列信息
- 保持旧策略行为与性能路径不变

### 4.2 回调兼容

- `OrderCallback` 原字段完全保留
- 新字段仅追加，不破坏现有策略代码

### 4.3 多合约一致性

- `MultiBacktestEngine` 支持统一透传 `queue_info_enabled`
- 单合约/多合约接口行为一致

---

## 5. 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `include/types.h` | `OrderCallback` 新增 3 个队列字段 |
| `include/con_auction_engine.hpp` | 新增 `QueueInfo`、开关与查询接口 |
| `src/con_auction_engine.cpp` | 实现同价位同侧历史队列采集逻辑 |
| `include/backtest_engine.hpp` | 新增队列信息开关接口声明 |
| `src/backtest_engine.cpp` | 仅在用户下单回调时填充队列字段 |
| `include/multi_backtest_engine.h` | 新增队列信息开关声明 |
| `src/multi_backtest_engine.cpp` | 多引擎开关透传实现 |
| `bindings/wangcai_py.cpp` | Python 绑定新增字段与开关 |
| `wangcai_syn/utils.py` | `run_backtest` 新增 `queue_info_enabled` 参数 |
| `tests/queue_info/test_queue_info_callback.py` | 自动化测试（开关开/关两组断言） |
| `user_example/run_queue_info_test.py` | 用户示例脚本 |
| `user_example/README.md` | 新增示例与参数说明 |

---

## 6. 验证方式

### 自动化验证

```bash
python tests/queue_info/test_queue_info_callback.py
```

### 用户示例验证

```bash
cd user_example
python run_queue_info_test.py
```

预期：
- 能收到下单回调
- 原字段正常
- 开关开启时新增字段为有效值

