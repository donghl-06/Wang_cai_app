#pragma once
#include <boost/pool/object_pool.hpp>
#include <memory>
#include <utility>      // std::forward
#include <new>          // placement new
#include "order.h"

namespace wangcai {

class OrderPool {
public:
    template<typename... Args>
    std::shared_ptr<Order> acquire(Args&&... args)
    {
        // 1. 先 malloc 一块裸内存
        Order* raw = pool_.malloc();

        // 2. placement-new 原地构造
        try {
            new (raw) Order(std::forward<Args>(args)...);
        } catch (...) {                 // 构造失败时回收内存
            pool_.free(raw);
            throw;
        }

        // 3. 用自定义 deleter，注意正确的析构和释放顺序
        return { raw, [this](Order* p){
            if (p) {
                p->~Order();            // 手动调用析构函数
                pool_.free(p);         // 归还内存到池
            }
        }};
    }

private:
    boost::object_pool<Order> pool_;
};

} // namespace wangcai