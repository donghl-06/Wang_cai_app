# 用户自定义数据推送 (Custom Data Push)
## 1. 功能总结
新增**用户自定义数据推送**功能，允许用户注入任意自定义数据（如因子信号、交易信号等），系统会在指定时间戳推送给策略：
- **灵活数据格式**：只需包含 `datetime` 列，其他列完全自定义
- **按时间推送**：到达指定时间戳时自动推送给策略
- **支持下单**：策略可根据自定义数据直接下单/撤单
- **可配置开关**：默认关闭
---
## 2. 使用方法
```python
import pandas as pd
from wangcai_syn import run_backtest
# 准备自定义数据（必须有 datetime 列）
custom_df = pd.DataFrame({
    'datetime': ['2025-11-17 09:35:00', '2025-11-17 10:00:00', '2025-11-17 14:30:00'],
    'signal': ['buy', 'hold', 'sell'],
    'target_price': [44.5, 0, 43.8]
})
# 启用自定义数据推送
run_backtest(
    data_dict=data,
    strategy=my_strategy,
    custom_data=custom_df,         # 自定义数据 DataFrame
    enable_custom_data=True        # 必须开启！
)
```
参数说明：
- `custom_data`：用户自定义数据 DataFrame，**必须包含 `datetime` 列**
- `enable_custom_data=True`：开启自定义数据推送
- `enable_custom_data=False`（默认）：关闭自定义数据推送
---
## 3. 策略回调
在策略中实现 `onCustomEvent` 方法接收自定义数据：
```python
class MyStrategy(Strategy):
    def onCustomEvent(self, data: dict):
        """
        收到自定义数据回调
        
        Args:
            data: 包含 datetime 及用户自定义字段的字典
                  如 {'datetime': '2025-11-17 09:35:00', 'signal': 'buy', 'target_price': 44.5}
        
        Returns:
            List[UserEvent]: 下单/撤单事件列表，空列表表示不操作
        """
        if data.get('signal') == 'buy':
            return [make_order(...)]
        return []
```
---
## 4. 实现逻辑
### 数据加载
- Python 层将 DataFrame 分拆为时间戳列表（传给 C++）和字典列表（存储在策略对象中）
- C++ 只管理时间戳和索引，不处理具体数据内容
### 时间排序
- 自定义事件按 `datetime` 排序
- 相同时间戳的数据按 DataFrame 行顺序（原始输入顺序）推送
### 推送机制
- 在每个市场时间戳处理完成后，检查是否有 `datetime <= current_time` 的自定义事件
- **一条条推送**：每条数据调用一次 `onCustomEvent()`
- **不重复推送**：使用全局递增索引，推送后索引前进
### 下单处理
- 策略返回的下单请求会被立即处理
- 下单时间以自定义事件的 `datetime` 为准
