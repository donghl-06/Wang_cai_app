/*
 * @Author: chenlisen
 * @Date: 2025-08-13 08:44:09
 * @LastEditTime: 2025-08-13 08:46:43
 * @FilePath: /wangcai_cpp/include/market_info.h
 */

#pragma once
#ifndef WANGCAI_MARKET_INFO_H
#define WANGCAI_MARKET_INFO_H
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include "types.h"

namespace wangcai_orderbook_cpp {

struct Snapshot {
 ///交易所代码
 int Exchange;
 ///合约代码
 std::string Instrument;
 ///交易日
 int TradingDay;
 ///自然日
 int ActionDay;
 ///时间
 int Time;
 std::string datetime;
 ///状态
 std::string Status; // 暂时不清楚这个字段的具体含义 先用string类型
 ///前收盘价（为原始值*10000）
 std::uint64_t PreClose;
 ///开盘价（为原始值*10000）
 std::uint64_t Open;
 ///最高价（为原始值*10000）
 std::uint64_t High;
 ///最低价（为原始值*10000）
 std::uint64_t Low;
 ///最新价（为原始值*10000）
 std::uint64_t      last_price{}; // 最新成交价
std::array<std::uint64_t, 10> bids{}; // 十档买方价格
std::array<Quantity, 10> bid_sizes{}; // 十档买方数量
std::array<std::uint64_t, 10> asks{}; // 十档卖方价格
std::array<Quantity, 10> ask_sizes{}; // 十档卖方数量
 ///成交笔数
 std::uint64_t NumTrades;
 ///成交总量
 std::uint64_t Volume;
 ///成交总金额（原始值)
 std::uint64_t Turnover;
 ///涨停价（为原始值*10000）
 std::uint64_t UpperLimit;
 ///跌停价（为原始值*10000）
 std::uint64_t LowerLimit;
 ///今持仓量
 std::uint64_t OpenInterest;
 ///昨持仓量
 std::uint64_t PreOpenInterest;
 ///今虚实度（为原始值*10000）
 std::uint64_t Delta;
 ///昨虚实度（为原始值*10000）
 std::uint64_t PreDelta;
 ///今收盘价（系统所有价格为原始数据*10000）
 std::uint64_t Close;
 ///今结算价（系统所有价格为原始数据*10000）
 std::uint64_t SettlePrice;
 ///昨结算价（系统所有价格为原始数据*10000）
 std::uint64_t PreSettlePrice;
 ///波段性中断参考价（系统所有价格为原始数据*10000） 
 std::uint64_t AuctionPrice;
 ///波段性中断集合竞价虚拟匹配量
 std::uint64_t AuctionQty;
 ///iopv x10000
 std::uint64_t Iopv; // 不知道这个是什么 暂留
 ///委托卖出总量
 std::uint64_t TotalAskVol;
 ///委托买入总量
 std::uint64_t TotalBidVol;
 ///加权平均委买价格（为原始值*10000）
 std::uint64_t WeightedAvgBidPrice;
 ///加权平均委卖价格（为原始值*10000）
 std::uint64_t WeightedAvgAskPrice;
 ///ETF申购总量
 std::uint64_t ETFCreateVol;
 ///ETF赎回总量
 std::uint64_t ETFRedeemVol;
 #ifdef PERF
 TEulerTimestampType     shm_time;       //行情写入shm时间
 TEulerTimestampType     mdfront_time;   //mdfront从shm取出行情的时间
 TEulerTimestampType     api_time;       //api收到行情的时间
 #endif
};

///逐笔委托行情
struct OrderDetail {
 ///交易所代码
 int Exchange;
 ///合约代码
 std::string Instrument;
 ///时间
 int Time;
 ///ChannelNo
 int ChannelNo;
 ///委托序号
 int OrderNo;
 ///委托价格（为原始值*10000）
 std::uint64_t Price;
 ///委托数量
 std::uint64_t Volume;
 ///委托方向  SH: 'B':买; 'S':卖  |  SZ: '1':买; '2':卖; 'G':借入; 'F':出借
 std::string Side;
 ///委托类别  '1'：市价  '2'：限价  'U'：本方最优 (深交所)  |  'A'：新增 'D'：删除（上交所）
 char OrderKind;
 ///
 long long SeqNo;
 ///业务编号
 long long BizIndex;
 #ifdef PERF
 TEulerTimestampType     shm_time;       //行情写入shm时间
 TEulerTimestampType     mdfront_time;   //mdfront从shm取出行情的时间
 TEulerTimestampType     api_time;       //api收到行情的时间
 #endif
};

///逐笔成交行情
struct TradeDetail {
 ///交易所代码
 int Exchange;
 ///合约代码
 std::string Instrument;
 ///ChannelNo
 int ChannelNo;
 ///TradeIndex
 long long TradeIndex;
 ///时间
 int Time;
 ///成交价格（为原始值*10000）
 std::uint64_t Price;
 ///成交数量
 std::uint64_t Volume;
 ///成交类别  '1':成交 '2': 撤销 'N':未知 （仅深交所有效，上交所始终未知）
 char ExecType;
 ///买方委托序号
 long long BuyNo;
 ///卖方委托序号
 long long SellNo;
 ///SH: 内外盘标识('B':主动买; 'S':主动卖; 'N':未知) | SZ: 成交标识('4':撤; 'F':成交)
 char TradeBSFlag;
 ///业务编号
 long long BizIndex;
 #ifdef PERF
 TEulerTimestampType     shm_time;       //行情写入shm时间
 TEulerTimestampType     mdfront_time;   //mdfront从shm取出行情的时间
 TEulerTimestampType     api_time;       //api收到行情的时间
 #endif
};

} // namespace wangcai

#endif