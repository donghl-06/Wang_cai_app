#pragma once

#include "orderbook.h"
#include "call_auction_engine.hpp"
#include "con_auction_engine.hpp"
#include "close_auction_engine.hpp"
#include "price_cage.h"
#include "data_manager.h"
#include "OrderLoader.h"
#include "market_info.h"
#include <queue>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>
#include <fstream>
#include <atomic>  // 用于 atomic<bool>
#include <stdexcept>

namespace wangcai {

// 延迟订单进簿位置:同一 release 时刻有多笔订单同时到达时,
// 策略单排在同时间订单的头部(先处理,抢排队优先级)还是尾部(后处理)
enum class LatencyEntryPosition { Head, Tail };

// 前向声明和类型别名
using OrderDetail = wangcai::OrderDetail;
using TradeDetail = wangcai::TradeDetail; 
using Snapshot = wangcai::Snapshot;

// 用户策略接口
class Strategy {
public:
    virtual ~Strategy() = default;
    
    // 接收订单事件，返回要处理的事件列表（下单或撤单）  
    virtual std::vector<UserEvent> onOrderEvent(const OrderDetail& order) = 0;
    
    // 接收成交事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onTradeEvent(const TradeDetail& trade) = 0;

    // 批量成交推送:同一市场事件撮合产生的全部历史成交一次性推送。
    // 引擎默认走此入口;默认实现逐条回落到 onTradeEvent(完全兼容旧策略)。
    // Python 策略覆写 onTradeEventsBatch 后,每市场事件只跨一次 Python 边界,
    // 高频重建成交场景(数万笔/日)可省掉大量 GIL 往返。
    virtual std::vector<UserEvent> onTradeEventsBatch(const std::vector<TradeDetail>& trades) {
        std::vector<UserEvent> out;
        for (const auto& t : trades) {
            auto v = onTradeEvent(t);
            out.insert(out.end(), std::make_move_iterator(v.begin()),
                       std::make_move_iterator(v.end()));
        }
        return out;
    }

    //  接受Tick事件，返回要处理的事件列表（下单或撤单）
    virtual std::vector<UserEvent> onTickEvent(const Snapshot& snapshot) = 0;
    
    // 接收用户自定义事件，返回要处理的事件列表（下单或撤单）
    // 参数 index 对应 Python 层自定义数据列表的索引，实际数据由 Python 绑定层传入
    // 默认返回空事件，避免影响未实现该接口的策略
    virtual std::vector<UserEvent> onCustomEvent(size_t index) { return {}; }

    // 接收由内部订单簿合成的高频实时 Tick（按 setRealTimeTickInterval 设定的间隔推送）
    // 快照字段与真实 3 秒 Tick 一致：十档/最新价来自订单簿实时状态，
    // Volume/Turnover/NumTrades/High/Low 由引擎按重建的历史成交逐笔累计。
    // 与官方快照的数值差异来自时间戳口径不同（官方 tick 有独立时间戳），属正常现象。
    // 默认返回空事件，避免影响未实现该接口的策略
    virtual std::vector<UserEvent> onRealTimeTickEvent(const Snapshot& snapshot) { return {}; }

    // 接收事件驱动快照（setEventSnapshotEnabled 开启后，每个市场事件
    // ——逐笔委托/逐笔成交（含撤单）——的全部处理（撮合 + 策略响应产生的
    // 下单/撤单）结束后，从内部订单簿合成一个十档 Snapshot 推送一次）
    // 快照口径与 onRealTimeTickEvent 一致：十档只含历史订单（公开订单簿，
    // 不含策略虚拟单挂单）；Volume 等累计器为引擎重建口径。
    // 推送次数 == 市场事件数（ord/tra），tick 事件不触发。
    // 默认返回空事件，避免影响未实现该接口的策略
    virtual std::vector<UserEvent> onEventSnapshot(const Snapshot& snapshot) { return {};}
    
    // 新的统一交易回调接口（包含持仓管理）
    virtual void onTradeCallback(const TradeCallback& callback) = 0;
    
    // 下单回调接口
    virtual void onOrderCallback(const OrderCallback& callback) = 0;
    
    // 原有接口保留（兼容性）
    virtual void onOrderFilled(const std::string& order_id, Price price, Quantity volume) = 0;
    virtual void onOrderCancelled(const std::string& order_id, const std::string& reason) = 0;
    
    // 获取策略ID
    virtual std::string getStrategyId() const = 0;

    // === 下单延迟（交易所链路时延模拟）===
    // latency_ms > 0 时，本策略的下单/撤单不再同步进簿，而是进入延迟队列，
    // 回测时钟推进到 发出时刻 + latency 时才"到达交易所"：过涨跌停/价格笼子
    // 校验、进订单簿、参与撮合；下单确认回调也延迟到到达时刻才发出。
    // 撤单走同一延迟通道，与下单保 FIFO（不会出现撤单比订单先到）。
    // 市场最小事件粒度 10ms：输入自动四舍五入对齐到 10ms 单位
    // （14→10，15→20；对齐后不足 10ms 的视为 0=关闭）。
    // 0 = 关闭（默认），行为与旧版一致。须在注册进引擎（run_backtest）之前设置。
    void setOrderLatencyMs(int latency_ms) {
        order_latency_ms_ = latency_ms > 0 ? ((latency_ms + 5) / 10) * 10 : 0;
    }
    int getOrderLatencyMs() const { return order_latency_ms_; }

    // 延迟订单进簿位置：release 时刻有多笔订单同时到达时，本策略单排在
    // 同时间订单的头部（默认，先处理，抢同价位排队优先级）还是尾部
    // （等同时间订单全部处理完再进簿）。须在注册进引擎之前设置。
    void setLatencyEntryPosition(LatencyEntryPosition pos) { latency_entry_head_ = (pos == LatencyEntryPosition::Head); }
    LatencyEntryPosition getLatencyEntryPosition() const {
        return latency_entry_head_ ? LatencyEntryPosition::Head : LatencyEntryPosition::Tail;
    }
    
    // 检查策略是否已完成所有处理（基于atomic状态变量）
    virtual bool isProcessingComplete() const { return processing_complete_.load(); }
    
    // 获取当前持仓（由策略基类管理）
    int64_t getPosition(const std::string& symbol) const {
        return position_manager_.getPosition(symbol);
    }

    void setPosition(const std::string& symbol, int64_t quantity) {
        position_manager_.setInitPosition(symbol, quantity);
    }
    
    // 获取所有持仓
    const std::map<std::string, int64_t>& getAllPositions() const {
        return position_manager_.getAllPositions();
    }
    
    // 公有方法：更新持仓（供 BacktestEngine 调用）
    void updateStrategyPosition(const std::string& symbol, int64_t quantity_change) {
        position_manager_.updatePosition(symbol, quantity_change);
    }
    
    // 异步处理状态管理（公有方法，供Python策略调用）
    void markProcessingStarted() {
        processing_complete_.store(false);
    }
    
    void markProcessingComplete() {
        processing_complete_.store(true);
    }

protected:
    // 持仓管理器，由基类提供
    mutable StrategyPositionManager position_manager_;
    
    // 异步处理状态管理（用于异步策略）
    mutable std::atomic<bool> processing_complete_{true}; // 默认同步策略已完成

    int order_latency_ms_ = 0;  // 下单/撤单延迟（毫秒），0=关闭
    bool latency_entry_head_ = true;  // 延迟单进簿位置：true=同时间订单头部
    
    // 更新持仓的受保护方法，供派生类使用
    void updatePosition(const std::string& symbol, int64_t quantity_change) {
        position_manager_.updatePosition(symbol, quantity_change);
    }
};

// 回测引擎
class BacktestEngine {
public:
    // 从CSV字符串初始化回测引擎（Python传入DataFrame.to_csv()生成的字符串）
    // is_etf: true=ETF（三位小数，tick=10）, false=股票（两位小数，tick=100）
    BacktestEngine(const std::string& symbol, 
                   const std::string& cstick_csv,
                   const std::string& order_csv, 
                   const std::string& trade_csv,
                   const std::string& csbar1d_csv,
                   bool is_etf = false);
    
    // 注册策略
    void registerStrategy(std::shared_ptr<Strategy> strategy);

    // 订单簿访问(供 MultiBacktestEngine 并行构建后搬运事件流)
    OrderBook& orderbook() { return *orderbook_; }
    
    // 获取回测结果
    std::map<std::string, Position> getPositions() const;
    double getTotalPnL() const;
    
    // 设置回调
    void setMarketDataCallback(std::function<void(const MarketData&)> callback);
    
    // 交易记录相关
    void enableTradeRecording(const std::string& output_file);  // 启用交易记录
    void writeTradeRecords() const;                             // 输出交易记录到CSV
    const std::vector<TradeRecord>& getTradeRecords() const;    // 获取所有交易记录
    
    // === 严格主动单模式（欠债限制功能）===
    // 开启后，虚拟主动单吃掉的历史订单需要被真实市场消耗后才能下新的主动单
    void setStrictActiveOrderMode(bool enabled);
    bool isStrictActiveOrderMode() const;
    bool hasDebt() const;  // 检查是否有未还清的欠债
    
    // === 真实成交替代模式 ===
    // 开启后：主动单自动使用严格模式，被动单使用队列位置+真实成交池匹配
    void setRealTradeMatchMode(bool enabled);
    bool isRealTradeMatchMode() const;
    // === 用户下单队列回报开关 ===
    void setQueueInfoEnabled(bool enabled);
    bool isQueueInfoEnabled() const;

    // === 实时合成 Tick（onRealTimeTickEvent）===
    // interval_ms > 0 时启用：每跨过一个 interval_ms 网格边界，在当前市场事件
    // 处理完成后用内部订单簿合成一个十档 Snapshot 推给策略；0 = 关闭（默认）。
    // 时间只随市场事件前进，无事件的空白区间不补发。
    void setRealTimeTickInterval(int interval_ms);
    int getRealTimeTickInterval() const;

    // === 事件驱动快照（onEventSnapshot）===
    // 开启后每个市场事件（ord/tra）的全部处理（含策略响应的下单/撤单）结束后，
    // 从内部订单簿合成一个十档 Snapshot 推给策略一次；默认关闭。
    // 与 setRealTimeTickInterval 的网格模式相互独立，可并存。
    void setEventSnapshotEnabled(bool enabled);
    bool isEventSnapshotEnabled() const;

    // === 价格笼子 ===
    // 规则按数据日期×板块自动判定（price_cage.h 矩阵）。
    // 历史单反推状态机仅在"深市创业板暂存窗口(2020.8-2023.4)"自动激活；
    // 策略单数值判定默认启用（有笼子的时代/板块），可手动关闭（对照实验用）。
    void setUserCageEnabled(bool enabled) { user_cage_enabled_ = enabled; }
    bool isUserCageEnabled() const { return user_cage_enabled_; }
    bool isPriceCageEnabled() const { return cage_rule_.enabled; }          // 规则矩阵是否有笼子
    bool isCageInferenceActive() const { return cage_inference_active_; }   // 历史单反推是否激活
    size_t getSuspendedHistoricalCount() const;                             // 历史笼单数
    size_t getUserCageCount() const;                                        // 策略笼单数

    // === 用户自定义事件支持 ===
    // 提交用户事件（用于外部驱动，例如自定义数据推送）
    void submitUserEvent(const UserEvent& user_event);
    // 检查是否包含某个用户订单（用于撤单路由）
    bool hasUserOrder(const std::string& order_id) const;
    // 设置当前时间（用于自定义事件推送时的回调时间）
    void setCurrentDatetimeForCustomEvent(const std::string& datetime);

    // 取出并清空跨标的用户事件（策略在本引擎回调里返回但属于其它标的的订单/撤单）
    // 由上层 MultiBacktestEngine 在 Taskflow join 之后串行调用，按 symbol 路由到正确的子引擎
    std::vector<UserEvent> drainCrossSymbolEvents();

    void processEvent(const Event& ev, const std::unordered_set<int64_t>& will_trade_ids = {});
    void finish();
    
private:
    // 集合竞价期间暂存的用户订单（影子单）
    // 不参与真实集合竞价的 Fenwick 价格发现，仅在 settle 之后按集合竞价成交价做影子成交判定
    struct PendingAuctionOrder {
        std::shared_ptr<Order> order;
        bool is_close_auction;  // false=开盘集合竞价, true=收盘集合竞价
    };

    void initialize();
    void processUserOrder(const UserOrder& user_order);
    void processUserCancel(const UserCancel& user_cancel);
    void processUserEvent(const UserEvent& user_event);
    // 下单延迟入口：开了延迟的策略事件入延迟队列，否则同步走 processUserEvent
    void enqueueOrDispatch(const UserEvent& user_event);
    // 释放所有 release_ms <= now_ms 的延迟事件（回测时钟只随市场事件前进，
    // 实际进簿时机 = 到达时刻之后的第一个市场事件，释放先于该事件处理）
    void drainDelayedEvents(int64_t now_ms);
    bool tryFillImmediately(std::shared_ptr<Order> user_order);

    // 集合竞价 settle 时处理影子订单：
    //  - 能成交的按集合竞价成交价全量成交，发 onTradeCallback 并更新持仓
    //  - 开盘未成交订单结转到连续竞价（con_engine_->accept，仍然是 USER 影子单）
    //  - 收盘未成交订单直接撤单（'D' 回调）
    void settleAuctionUserOrders(bool is_close);

    void updatePosition(const std::string& strategy_id, const std::string& symbol, 
                       Direction direction, Quantity volume, Price price);
    void printResults() const;
    TradeDetail normalizeHistoricalExecution(const Execution& ex, const std::string& datetime) const;
    void recordTrade(const TradeDetail& trade, const std::string& datetime);
    void recordCancel(uint64_t order_id, const std::string& datetime);
    void recordCancelWithOrderInfo(uint64_t original_id, const std::string& datetime, 
                                  std::shared_ptr<Order> order_info);
    
    // 新增：通知策略成交
    void notifyStrategiesOnExecution(const Execution& ex);
    
    // 新增：创建交易回调对象
    TradeCallback createTradeCallback(const std::string& strategy_id, const std::string& order_id, 
                                    Direction direction, Quantity volume, Price price, 
                                    const std::string& datetime, char match_type);
    
    // 新增：通知策略统一交易回调
    void notifyStrategyTradeCallback(const std::string& strategy_id, const TradeCallback& callback);
    
    // 新增：创建下单回调对象
    OrderCallback createOrderCallback(const std::string& strategy_id, const std::string& order_id,
                                    Direction direction, Quantity volume, Price price, 
                                    const std::string& datetime);
    
    // 新增：通知策略下单回调
    void notifyStrategyOrderCallback(const std::string& strategy_id, const OrderCallback& callback);
    
    // 检查所有策略是否已完成处理
    bool areAllStrategiesComplete() const;
    
    // 等待所有策略完成处理
    void waitForStrategiesCompletion();
    
    // 成员变量
    std::string symbol_;
    std::string cstick_csv_;
    std::string order_csv_;
    std::string trade_csv_;
    std::string csbar1d_csv_;
    bool is_etf_ = false;      // 是否为ETF（ETF=三位小数/tick=10，股票=两位小数/tick=100）
    
    // 订单簿和引擎
    std::unique_ptr<OrderBook> orderbook_;
    std::unique_ptr<CallAuctionEngine> call_engine_;
    std::unique_ptr<ConAuctionEngine> con_engine_;
    std::unique_ptr<CloseAuctionEngine> close_engine_; // 收盘集合竞价引擎
    std::unique_ptr<CloseAuctionEngine> resume_engine_; // 临停复牌集合竞价引擎(专用实例)
    std::unique_ptr<DataManager> data_manager_;

    // 策略管理
    std::vector<std::shared_ptr<Strategy>> strategies_;
    std::map<std::string, std::map<std::string, Position>> positions_; // strategy_id -> symbol -> position
    
    // 市场数据队列
    std::queue<MarketData> market_data_queue_;
    std::function<void(const MarketData&)> market_data_callback_;
    
    // 成交事件产生的策略事件队列
    std::vector<UserEvent> pending_trade_events_;
    // 当前市场事件内撮合产生的历史成交缓冲,在事件处理末尾
    // 经 onTradeEventsBatch 一次性推送策略(批量跨界,见 Strategy 注释)
    std::vector<TradeDetail> pending_trade_batch_;

    // 跨标的用户事件暂存：策略在本引擎回调里返回、但 symbol 不属于本引擎的订单/撤单
    // 仅在 Taskflow 并行阶段内写入，join 之后由上层串行 drain + 路由
    std::vector<UserEvent> cross_symbol_events_;

    // === 下单延迟队列（策略单交易所链路时延模拟，见 Strategy::setOrderLatencyMs）===
    struct DelayedUserEvent {
        int64_t release_ms;  // 到达交易所的时刻（发出时刻 + 延迟）
        uint64_t seq;        // 同一 release_ms 内保 FIFO
        UserEvent event;
        bool operator>(const DelayedUserEvent& o) const {
            return release_ms != o.release_ms ? release_ms > o.release_ms
                                              : seq > o.seq;
        }
    };
    using DelayedQueue = std::priority_queue<DelayedUserEvent, std::vector<DelayedUserEvent>,
                                             std::greater<DelayedUserEvent>>;
    // 头/尾两个独立队列：头部单 release <= 当前事件时间即释放（先于同时间订单进簿）;
    // 尾部单须 release < 当前事件时间（等同时间订单处理完，随更晚的事件进簿）。
    // 分开存是为了避免队列里靠前的尾部单堵住后面的头部单
    DelayedQueue delayed_head_;
    DelayedQueue delayed_tail_;
    uint64_t delayed_seq_ = 0;
    int64_t current_ms_ = -1;  // 当前回测时钟（与 current_datetime_ 同源）
    bool any_latency_ = false; // 有策略开延迟才走队列（默认路径零开销短路）
    std::map<std::string, int> latency_map_;   // strategy_id -> 延迟ms（注册时快照）
    std::map<std::string, bool> latency_head_map_;  // strategy_id -> 进簿位置（注册时快照）
    // 尚在延迟队列中的用户订单ID：撤单路由判定时视为"本引擎持有"
    // （订单未 release 前 user_order_mapping_ 里查不到，撤单会被误路由为跨标的）
    std::unordered_set<std::string> delayed_order_ids_;

    // 集合竞价期间暂存的用户订单（开盘 09:15-09:25、收盘 14:57-15:00）
    // settle 时做影子成交判定
    std::vector<PendingAuctionOrder> pending_auction_orders_;
    
    // 事件数据
    bool continuous_mode_;
    bool closing_mode_ = false;                       // 是否进入收盘集合竞价阶段

    // 盘中临时停牌(新股前 5 日无涨跌幅:盘中成交价较当日开盘价首次越过
    // ±30%/±60% 各停牌 10 分钟,临停期接受申报/撤单但撮合暂停,复牌集合竞价;
    // 停牌跨越 14:57 的于 14:57 复牌。300869.SZ 2020-08-24/688425.SH
    // 2021-06-22 实证:引擎缺此语义,在真实临停空窗内照常撮合出大量幽灵成交)
    bool halt_applicable_ = false;  // csbar1d 涨跌停为 0(前 5 日)且开盘价有效
    bool halted_ = false;           // 当前是否临停中
    int64_t halt_until_dt_ms_ = 0;  // 复牌时刻(事件时间轴)
    // 深市专用:复牌打印时刻 = (触发+10min) 向上取整到下一秒——临停期申报/
    // 撤单被 adata 全部重戳到该整秒(14 只次普查 ε=0.010~0.970s 无一例外),
    // 复牌集合竞价必须等该毫秒的事件全部进簿后才结算
    int64_t halt_restamp_ms_ = 0;
    Price halt_trigger_px_ = 0;  // 临停触发成交价(复牌竞价±10%范围基准)
    Price last_hist_px_ = 0;     // 最近历史成交价(±10% 暂存池激活判据)
    bool halt_p30_done_ = false;    // 四档各触发一次:+30%/+60%/-30%/-60%
    bool halt_p60_done_ = false;
    bool halt_m30_done_ = false;
    bool halt_m60_done_ = false;
    void maybeEnterHalt(Price trade_px);            // 历史成交回调内驱动
    void resumeFromHalt(const std::string& datetime); // 复牌集合竞价
    bool price_cage_checked_ = false;                 // 是否已检查过价格笼子
    std::string current_datetime_;  // 当前事件时间
    std::string last_brk_datetime_; // 最后一条BRK事件时间（连续竞价成交回调用）
    
    // 订单ID管理
    uint64_t next_order_id_;
    uint64_t next_trade_id_;        // 交易ID生成器
    std::map<std::string, uint64_t> user_order_mapping_; // user_order_id -> system_order_id
    std::map<uint64_t, Direction> user_order_direction_;  // system_order_id -> direction
    
    // 虚拟订单映射 - 用于成交回调时识别虚拟订单
    std::map<uint64_t, std::string> virtual_order_strategy_; // system_order_id -> strategy_id
    std::map<uint64_t, std::string> virtual_order_local_id_; // system_order_id -> local_order_id
    
    // 价格信息
    Price prev_close_;
    Price upper_limit_;
    Price lower_limit_;
    Price actual_open_;
    
    // 交易记录
    std::vector<TradeRecord> trade_records_;  // 所有交易记录
    std::string trade_output_file_;           // 输出文件路径
    bool recording_enabled_;                  // 是否启用记录
    
    // 真实成交替代模式
    bool real_trade_match_mode_ = false;
    // 用户下单队列回报开关（默认关闭）
    bool queue_info_enabled_ = false;
    // 判断真实成交中被消耗的一侧（买方被动 or 卖方被动）
    bool determineBuyPassive(const Event& ev) const;

    // === 实时合成 Tick（onRealTimeTickEvent）===
    int realtime_tick_interval_ms_ = 0;      // 推送间隔（毫秒），0=关闭
    int64_t realtime_tick_last_bucket_ = -1; // 已推送的间隔网格编号（当日毫秒 / 间隔）
    bool has_real_tick_ = false;             // 是否已收到过真实 tick（决定快照承接来源）
    bool event_snapshot_enabled_ = false;    // 事件驱动快照开关（每个 ord/tra 事件推一次）
    // 日累计器：随内部撮合的历史成交（HistoricalHistorical Execution）递增。
    // 完全由引擎重建口径驱动，不与官方 tick 对齐 —— 官方快照有独立时间戳，
    // 两者数值出入属于口径差异；本累计器与订单簿状态严格自洽且单调不减。
    // 注意：不能挂在 CSV tra 事件上 —— load_traders 只把撤单记录插入事件流，
    // 连续时段的成交记录进入 continuous_trades_ 后不再消费；引擎重建的成交
    // 全部经由 OrderBook 的 exec 回调（连续竞价撮合 + 开/收盘集合竞价 settle）。
    uint64_t rt_volume_ = 0;                 // 累计成交量（股）
    double rt_turnover_ = 0.0;               // 累计成交额（元，双精度累加避免整除截断）
    uint64_t rt_num_trades_ = 0;             // 累计成交笔数
    uint64_t rt_high_ = 0;                   // 当日最高成交价（厘，0=尚无成交）
    uint64_t rt_low_ = 0;                    // 当日最低成交价（厘，0=尚无成交）

    void accumulateInternalTrade(Price price, Quantity volume); // 历史撮合成交递增累计器
    Snapshot buildRealTimeSnapshot(const Event& ev) const; // 从订单簿合成十档快照
    void maybeEmitRealTimeTick(const Event& ev);    // 跨过网格边界时推送

    // === 价格笼子 ===
    std::string trading_date_;                      // 交易日（从数据解析，YYYY-MM-DD）
    PriceCageRule cage_rule_;                       // 规则矩阵判定结果
    bool cage_inference_active_ = false;            // 历史单反推状态机是否激活
    bool user_cage_enabled_ = true;                 // 策略单数值判定开关（默认开）
    std::shared_ptr<Order> pending_crossing_hist_;  // 挂起待确认的穿价历史单（反推窗口）
    int64_t pending_crossing_ms_ = -1;              // 挂起单的委托毫秒（同毫秒确认判据）

    // === 深市裸市价单事件驱动执行（≥2023-04-10，子类被压平，见 accept_sz 注释）===
    // 委托到达先挂起；消息流中紧随的本单成交逐笔执行（executeMarketRealTrade）；
    // 出现非本单消息即事件段终结：剩余>0 以最后成交价挂簿，零成交登记进场即撤。
    std::shared_ptr<Order> pending_market_hist_;    // 挂起中的裸市价历史单
    // 出笼回放中的历史笼中单（事件驱动出笼兜底，创业板暂存窗口）：
    // 数值出笼判据的基准价来自引擎簿，簿偏差会使边界单漏判出笼
    // （300745.SZ 2022-06-27 实证：引擎买一 54.85 vs 真实 54.82，范围
    // 下限 53.76 vs 53.72，53.73 卖单真实出笼吃穿、引擎蹲笼假阴）。
    // 校正：真实成交事件引用了仍在笼中的订单 → 该单此刻必已在真实簿
    // 参与撮合，强制取出并进入回放——本单紧随的连续成交逐笔按记录吃掉
    // 引擎簿内对手（executeMarketRealTrade，校验身份/价格/量，对手缺失
    // 即暴露簿偏离）；出现非本单引用的消息 → 终结，剩余量按原限价挂簿
    // 交自主撮合。集合支持多单并行回放（高频日多个笼中单成交串交错，
    // 300651.SZ 单容量实证漏回放）。
    std::map<uint64_t, std::shared_ptr<Order>> replaying_caged_;  // key=input_id
    Price pending_market_last_px_ = 0;              // 其最后真实成交价（=对方最优一档）
};

} // namespace wangcai
