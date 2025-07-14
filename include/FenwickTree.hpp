#pragma once
#include <vector>
#include <cstdint>

namespace wangcai_orderbook_cpp {

/**
 * @brief Fenwick Tree (Binary Indexed Tree) 实现
 * 用于高效计算区间和查询，支持O(log n)的更新和查询操作
 */
class FenwickTree {
public:
    explicit FenwickTree(int size) : tree_(size + 1, 0) {}
    
    /**
     * @brief 在位置idx增加val
     * @param idx 位置索引（1-based）
     * @param val 增加的值
     */
    void update(int idx, int64_t val) {
        for (int i = idx; i < static_cast<int>(tree_.size()); i += i & (-i)) {
            tree_[i] += val;
        }
    }
    
    /**
     * @brief 查询前缀和[1, idx]
     * @param idx 查询位置（1-based）
     * @return 前缀和
     */
    int64_t query(int idx) const {
        int64_t sum = 0;
        for (int i = idx; i > 0; i -= i & (-i)) {
            sum += tree_[i];
        }
        return sum;
    }
    
    /**
     * @brief 查询区间和[left, right]
     * @param left 左边界（1-based）
     * @param right 右边界（1-based）
     * @return 区间和
     */
    int64_t rangeQuery(int left, int right) const {
        if (left > right) return 0;
        return query(right) - query(left - 1);
    }
    
    /**
     * @brief 重置所有值为0
     */
    void clear() {
        std::fill(tree_.begin(), tree_.end(), 0);
    }

private:
    std::vector<int64_t> tree_;
};

} // namespace wangcai_orderbook_cpp 