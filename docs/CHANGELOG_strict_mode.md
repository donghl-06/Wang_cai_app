# 严格主动单模式 (Strict Active Order Mode)

## 1. 功能总结

新增**严格主动单模式**，用于更真实地模拟市场撮合：

- **欠债限制**：虚拟主动单吃掉的历史订单，在被真实市场消耗前，禁止下新的主动单
- **被动单不受影响**：挂单（价格不穿越对手盘）可正常提交
- **可配置开关**：默认关闭，通过 `strict_active_order_mode=True` 启用

---

## 2. 实现逻辑

### 核心数据结构

```cpp
// con_auction_engine.hpp
bool strict_active_order_mode_ = false;           // 开关
std::unordered_set<uint64_t> debt_orders_;        // 欠债订单ID集合
```

### 处理流程

1. **主动单提交时**（`accept_virtual_order`）：
   - 检查 `debt_orders_` 是否为空
   - 非空 → 拒绝订单，触发撤单回调
   - 为空 → 遍历对手盘，将吃掉的历史订单ID加入 `debt_orders_`

2. **历史订单被真实市场成交时**（`on_historical_order_removing`）：
   - 从 `debt_orders_` 中移除对应订单ID
   - 全部清除后，新的主动单可正常提交

3. **禁用快速成交捷径**（`tryFillImmediately`）：
   - 严格模式下返回 `false`，确保所有主动单走 `ConAuctionEngine` 完整逻辑

### 涉及文件

| 文件 | 修改内容 |
|------|----------|
| `include/con_auction_engine.hpp` | 新增成员变量和接口 |
| `src/con_auction_engine.cpp` | 欠债检查、记录、清除逻辑 |
| `src/backtest_engine.cpp` | 严格模式下禁用 `tryFillImmediately` |
| `bindings/wangcai_py.cpp` | Python 接口绑定 |
| `wangcai_syn/utils.py` | `run_backtest` 新增参数 |

---

## 3. 测试用例

测试文件位于 `tests/strict_mode/`：

| 脚本 | 测试内容 |
|------|----------|
| `run_comprehensive_test.py` | 主动买/卖、被动买/卖、连续拦截 |
| `run_debt_clear_test.py` | 欠债清零后可继续下单 |
| `run_strict_mode_off_test.py` | 关闭模式时无拦截 |

运行方式：
```bash
cd tests/strict_mode
python3 run_comprehensive_test.py
python3 run_debt_clear_test.py
```
