// === include/FenwickTree.hpp ===
/*
 * @brief  : Binary Indexed Tree (Fenwick)
 */
#pragma once
#include <vector>
#include <cstdint>

namespace wangcai_orderbook_cpp {

class Fenwick {
public:
    explicit Fenwick(std::size_t n = 0) : tree_(n + 1, 0) {}
    void reset(std::size_t n)           { tree_.assign(n + 1, 0); }

    inline void add(int idx, int64_t delta) noexcept {
        for (++idx; idx < (int)tree_.size(); idx += idx & -idx)
            tree_[idx] += delta;
    }
    inline int64_t prefixSum(int idx) const noexcept {
        if (idx < 0) return 0;
        int64_t s = 0; ++idx;
        for (; idx; idx -= idx & -idx) s += tree_[idx];
        return s;
    }
private:
    std::vector<int64_t> tree_;
};

} // namespace wangcai_orderbook_cpp 