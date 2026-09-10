// === include/price_cage.h ===
/*
 * @brief : 价格笼子规则配置（板块 × 日期阶段矩阵）
 *
 * 规则版本（2026-09-09 用户确认）：
 *   第一阶段 2019.7.22 科创板首创，拒单模式（纯 ±2%，无 0.1 元兜底）；
 *   第二阶段 2020.8.24 创业板跟进，暂存模式（±2%），A 股唯一暂存窗口，
 *             仅存在于深市创业板（2020.8.24 – 2023.4.10）；
 *   第三阶段 2023.4.10 全面注册制统一，全部改拒单并加 0.1 元兜底
 *             （十个申报价格最小变动单位），主板首次有笼子。
 *   北交所全程无价格笼子；基金（ETF/LOF）、债券、回购、B 股不适用。
 *
 * 历史单路径（行情可见性重建）：仅"深市创业板暂存窗口"需要反推状态机
 *   （穿价但紧挨下一条消息不是它的成交 → 入笼；盘口不再穿价/对手盘空 → 出笼）；
 *   其余时期超范围均为废单、不进 csord，数据无痕迹，无需处理。
 * 策略单路径（合规模拟）：任何拒单/暂存时代按数值规则判定（无后续消息可反推）。
 */
#pragma once
#include <string>
#include <cstdint>
#include "types.h"

namespace wangcai {

// 板块（按 symbol 前缀判定）
enum class Board { Main, GEM, STAR, BSE, Fund };

inline Board boardOf(const std::string& symbol) {
    if (symbol.size() < 3) return Board::Main;
    const std::string p = symbol.substr(0, 3);
    if (p == "688" || p == "689") return Board::STAR;    // 科创板（沪市独有）
    if (p == "300" || p == "301") return Board::GEM;     // 创业板（深市）
    if (p[0] == '6') return Board::Main;                 // 沪主板 60x
    if (p[0] == '0') return Board::Main;                 // 深主板 00x（019 国债等边缘按主板，无实际影响）
    if (p[0] == '8' || p[0] == '4' || p == "920") return Board::BSE;  // 北交所
    if (p[0] == '5' || p[0] == '1') return Board::Fund;  // 基金/债券/可转债等（不适用笼子）
    if (p[0] == '9' || p[0] == '2') return Board::Fund;  // B 股 900/200
    return Board::Main;
}

// 交易日阶段（日期格式 YYYY-MM-DD，字典序即日期序）
enum class MarketStage { Before2019, StageB, StageC, StageD };

inline MarketStage stageOf(const std::string& date) {
    if (date < "2019-07-22") return MarketStage::Before2019;
    if (date < "2020-08-24") return MarketStage::StageB;
    if (date < "2023-04-10") return MarketStage::StageC;
    return MarketStage::StageD;
}

// 超范围处理动作
enum class CageAction { None, Dormant, Reject };

// 数值笼子规则（策略单判定用）
struct PriceCageRule {
    bool enabled = false;
    int64_t pct_num = 200;   // 幅度分子×100：2% → 200（整数比较 price*10000 <= base*(10000+pct_num)）
    Price min_abs = 0;       // 兜底额（厘）：0.1 元 = 1000；0 = 无兜底
    CageAction action = CageAction::None;
};

inline PriceCageRule priceCageRuleFor(Board b, const std::string& date) {
    const MarketStage st = stageOf(date);
    switch (b) {
        case Board::Main:  // 主板：仅 2023.4.10 后有笼子
            if (st == MarketStage::StageD) return {true, 200, 1000, CageAction::Reject};
            return {};
        case Board::GEM:   // 创业板：2020.8-2023.4 暂存；之后拒单+兜底
            if (st == MarketStage::StageC) return {true, 200, 0, CageAction::Dormant};
            if (st == MarketStage::StageD) return {true, 200, 1000, CageAction::Reject};
            return {};
        case Board::STAR:  // 科创板：开市起拒单纯 2%；2023.4.10 后加兜底
            if (st == MarketStage::StageB || st == MarketStage::StageC)
                return {true, 200, 0, CageAction::Reject};
            if (st == MarketStage::StageD) return {true, 200, 1000, CageAction::Reject};
            return {};
        default:           // 北交所 / 基金债券等：全程无笼子
            return {};
    }
}

// 历史单反推状态机仅在"深市创业板暂存窗口"激活
// （沪市无暂存模式；科创板全程拒单——废单不进 csord，数据无痕迹）
inline bool needsHistoricalCageInference(const std::string& symbol, const std::string& date) {
    return symbol.size() > 3 && symbol.compare(symbol.size() - 3, 3, ".SZ") == 0
        && boardOf(symbol) == Board::GEM
        && stageOf(date) == MarketStage::StageC;
}

} // namespace wangcai
