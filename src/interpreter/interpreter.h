// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTERPRETER_INTERPRETER_H_
#define V8_INTERPRETER_INTERPRETER_H_

#include <memory>

// Clients of this interface shouldn't depend on lots of interpreter internals.
// Do not include anything from src/interpreter other than
// src/interpreter/bytecodes.h here!
#include "src/base/macros.h"
#include "src/builtins/builtins.h"
#include "src/interpreter/bytecodes.h"
#include "src/runtime/runtime.h"

namespace v8 {
namespace internal {

class BytecodeArray;
class Callable;
class UnoptimizedCompilationJob;
class FunctionLiteral;
class Isolate;
class ParseInfo;
class RootVisitor;
class SetupIsolateDelegate;
template <typename>
class ZoneVector;

namespace interpreter {

class InterpreterAssembler;

// Ignition 设计文档: https://docs.google.com/document/d/11T2CRex9hXxoJwbYqVQ32yIPMh0uouUZLdyrtmMoL44/edit#
class Interpreter {
 public:
  explicit Interpreter(Isolate* isolate);
  virtual ~Interpreter() = default;
  Interpreter(const Interpreter&) = delete;
  Interpreter& operator=(const Interpreter&) = delete;

  // Creates a compilation job which will generate bytecode for |literal|.
  // Additionally, if |eager_inner_literals| is not null, adds any eagerly
  // compilable inner FunctionLiterals to this list.
  // 为 |literal| 创建用于生成字节码的编译工作
  static std::unique_ptr<UnoptimizedCompilationJob> NewCompilationJob(
      ParseInfo* parse_info, FunctionLiteral* literal,
      AccountingAllocator* allocator,
      std::vector<FunctionLiteral*>* eager_inner_literals);

  // Creates a compilation job which will generate source positions for
  // |literal| and when finalized, store the result into |existing_bytecode|.
  // 为 |literal| 创建用于生成代码位置的编译工作, 保存到 |existing_bytecode|
  static std::unique_ptr<UnoptimizedCompilationJob>
  NewSourcePositionCollectionJob(ParseInfo* parse_info,
                                 FunctionLiteral* literal,
                                 Handle<BytecodeArray> existing_bytecode,
                                 AccountingAllocator* allocator);

  // If the bytecode handler for |bytecode| and |operand_scale| has not yet
  // been loaded, deserialize it. Then return the handler.
  // 如果 |bytecode| 和 |operand_scale| 的字节码 handler 还没加载, 则反序列它,
  // 然后返回 handler. operand_scale 指字节码的操作范围, 有单、双、4 字节三种
  V8_EXPORT_PRIVATE Code GetBytecodeHandler(Bytecode bytecode,
                                            OperandScale operand_scale);

  // Set the bytecode handler for |bytecode| and |operand_scale|.
  void SetBytecodeHandler(Bytecode bytecode, OperandScale operand_scale,
                          Code handler);

  // Disassembler support.
  V8_EXPORT_PRIVATE const char* LookupNameOfBytecodeHandler(const Code code);

  V8_EXPORT_PRIVATE Local<v8::Object> GetDispatchCountersObject();

  void ForEachBytecode(const std::function<void(Bytecode, OperandScale)>& f);

  void Initialize();

  bool IsDispatchTableInitialized() const;

  Address dispatch_table_address() {
    return reinterpret_cast<Address>(&dispatch_table_[0]);
  }

  Address bytecode_dispatch_counters_table() {
    return reinterpret_cast<Address>(bytecode_dispatch_counters_table_.get());
  }

  Address address_of_interpreter_entry_trampoline_instruction_start() const {
    return reinterpret_cast<Address>(
        &interpreter_entry_trampoline_instruction_start_);
  }

 private:
  friend class SetupInterpreter;
  friend class v8::internal::SetupIsolateDelegate;

  uintptr_t GetDispatchCounter(Bytecode from, Bytecode to) const;

  // Get dispatch table index of bytecode.
  static size_t GetDispatchTableIndex(Bytecode bytecode,
                                      OperandScale operand_scale);

  static const int kNumberOfWideVariants = BytecodeOperands::kOperandScaleCount;
  static const int kDispatchTableSize = kNumberOfWideVariants * (kMaxUInt8 + 1);
  static const int kNumberOfBytecodes = static_cast<int>(Bytecode::kLast) + 1;

  Isolate* isolate_;
  Address dispatch_table_[kDispatchTableSize];
  std::unique_ptr<uintptr_t[]> bytecode_dispatch_counters_table_;
  Address interpreter_entry_trampoline_instruction_start_;
};

}  // namespace interpreter
}  // namespace internal
}  // namespace v8

#endif  // V8_INTERPRETER_INTERPRETER_H_
