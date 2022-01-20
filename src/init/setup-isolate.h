// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INIT_SETUP_ISOLATE_H_
#define V8_INIT_SETUP_ISOLATE_H_

#include "src/base/macros.h"

namespace v8 {
namespace internal {

class Builtins;
class Code;
class Heap;
class Isolate;

// This class is an abstraction layer around initialization of components
// that are either deserialized from the snapshot or generated from scratch.
// Currently this includes builtins and interpreter bytecode handlers.
// There are two implementations to choose from at link time:
// - setup-isolate-deserialize.cc: always loads things from snapshot.
// - setup-isolate-full.cc: loads from snapshot or bootstraps from scratch,
//                          controlled by the |create_heap_objects| flag.
// For testing, the implementation in setup-isolate-for-tests.cc can be chosen
// to force the behavior of setup-isolate-full.cc at runtime.
//
// The actual implementations of generation of builtins and handlers is in
// setup-builtins-internal.cc and setup-interpreter-internal.cc, and is
// linked in by the latter two Delegate implementations.
//
// 这个类是初始化组件的一个抽象层, 负责从 snapshot 中反序列化或者从头创建.
// 当前这包含 builtins 和解析器字节码处理器.
// 在链接时有两种实现可以选择:
// - setup-isolate-deserialize.cc: 总是从 snapshot 加载
// - setup-isolate-full.cc: 从 snapshot 加载或者从头引导, 由 create_heap_objects
// 控制 setup-isolate-for-tests.cc 实现可以用于测试以强制指定 setup-isolate-full.cc
// 的运行期行为
//
// 生成 builtins 和 handlers 的实现在 setup-builtins-internal.cc 和
// setup-interpreter-internal.cc 中, 然后在稍后的两个托管实现中进行链接
class V8_EXPORT_PRIVATE SetupIsolateDelegate {
 public:
  explicit SetupIsolateDelegate(bool create_heap_objects)
      : create_heap_objects_(create_heap_objects) {}
  virtual ~SetupIsolateDelegate() = default;

  virtual void SetupBuiltins(Isolate* isolate);

  virtual bool SetupHeap(Heap* heap);

 protected:
  static void SetupBuiltinsInternal(Isolate* isolate);
  static void AddBuiltin(Builtins* builtins, int index, Code code);
  static void PopulateWithPlaceholders(Isolate* isolate);
  static void ReplacePlaceholders(Isolate* isolate);

  static bool SetupHeapInternal(Heap* heap);

  const bool create_heap_objects_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_INIT_SETUP_ISOLATE_H_
