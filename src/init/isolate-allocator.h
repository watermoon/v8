// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INIT_ISOLATE_ALLOCATOR_H_
#define V8_INIT_ISOLATE_ALLOCATOR_H_

#include <memory>

#include "src/base/bounded-page-allocator.h"
#include "src/base/page-allocator.h"
#include "src/common/globals.h"
#include "src/utils/allocation.h"

namespace v8 {

// Forward declarations.
namespace base {
class BoundedPageAllocator;
}  // namespace base

namespace internal {

// IsolateAllocator object is responsible for allocating memory for one (!)
// Isolate object. Depending on the whether pointer compression is enabled,
// the memory can be allocated
// 1) in the C++ heap (when pointer compression is disabled)
// 2) in a proper part of a properly aligned region of a reserved address space
//   (when pointer compression is enabled).
//
// Isolate::New() first creates IsolateAllocator object which allocates the
// memory and then it constructs Isolate object in this memory. Once it's done
// the Isolate object takes ownership of the IsolateAllocator object to keep
// the memory alive.
// Isolate::Delete() takes care of the proper order of the objects destruction.
//
// IsolateAllocator 对象负责为一个 Isolate 对象分配内存. 根据是否启用了指针压缩, 内存
// 可能在两个地方分配:
// 1) C++ 堆(不启用指针压缩)
// 2) 在一个正确对齐的保留地址空间
//
// Isolate::New() 首先创建 IsolateAllocator 对象, IsolateAllocator 会分配内存, 然后
// 在刚分配的内存中构建 Isolate 对象. 一旦构建成功, Isolate 对象会接管 IsolateAllocator
// 对象的所有权(相当于 hold 其句柄)
// Isolate::Delete 关注对象析构的正确顺序

class V8_EXPORT_PRIVATE IsolateAllocator final {
 public:
  IsolateAllocator();
  ~IsolateAllocator();

  void* isolate_memory() const { return isolate_memory_; }

  v8::PageAllocator* page_allocator() const { return page_allocator_; }

 private:
  Address InitReservation();
  void CommitPagesForIsolate(Address heap_reservation_address);

  // The allocated memory for Isolate instance.
  void* isolate_memory_ = nullptr;
  v8::PageAllocator* page_allocator_ = nullptr;
  std::unique_ptr<base::BoundedPageAllocator> page_allocator_instance_;
  VirtualMemory reservation_;

  DISALLOW_COPY_AND_ASSIGN(IsolateAllocator);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_INIT_ISOLATE_ALLOCATOR_H_
