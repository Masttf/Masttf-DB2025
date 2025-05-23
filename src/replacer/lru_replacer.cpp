/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

/**
 * @description: 使用LRU策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id
 * 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t* frame_id) {
    // Todo:
    // !利用lru_replacer中的LRUlist_,LRUHash_实现LRU策略
    // !选择合适的frame指定为淘汰页面,赋值给*frame_id

    std::scoped_lock lock{latch_};
    if (LRUlist_.empty()) {
        return false;
    }

    *frame_id = LRUlist_.back();  // 选择最久未使用的页面(链表尾部)
    LRUhash_.erase(*frame_id);    // 从哈希表中删除
    LRUlist_.pop_back();          // 从链表中删除

    return true;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    // Todo:
    // !固定指定id的frame
    // !在数据结构中移除该frame

    std::scoped_lock lock{latch_};
    auto iter = LRUhash_.find(frame_id);
    if (iter != LRUhash_.end()) {
        LRUlist_.erase(iter->second);  // 从链表中删除
        LRUhash_.erase(iter);          // 从哈希表中删除
    }
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin(frame_id_t frame_id) {
    // 支持并发锁
    std::scoped_lock lock{latch_};

    // 选择一个frame取消固定
    if (LRUhash_.find(frame_id) != LRUhash_.end()) return;
    LRUlist_.push_front(frame_id);                 // 加入链表头部(最近使用)
    LRUhash_.emplace(frame_id, LRUlist_.begin());  // 加入哈希表
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() { return LRUlist_.size(); }
