// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/code-stub-assembler.h"

#include "include/v8-internal.h"
#include "src/base/macros.h"
#include "src/codegen/code-factory.h"
#include "src/codegen/tnode.h"
#include "src/common/globals.h"
#include "src/execution/frames-inl.h"
#include "src/execution/frames.h"
#include "src/execution/protectors.h"
#include "src/heap/heap-inl.h"  // For MemoryChunk. TODO(jkummerow): Drop.
#include "src/heap/memory-chunk.h"
#include "src/logging/counters.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/cell.h"
#include "src/objects/descriptor-array.h"
#include "src/objects/function-kind.h"
#include "src/objects/heap-number.h"
#include "src/objects/js-generator.h"
#include "src/objects/oddball.h"
#include "src/objects/ordered-hash-table-inl.h"
#include "src/objects/property-cell.h"
#include "src/roots/roots.h"
#include "src/wasm/wasm-objects.h"

namespace v8 {
namespace internal {

using compiler::Node;

CodeStubAssembler::CodeStubAssembler(compiler::CodeAssemblerState* state)
    : compiler::CodeAssembler(state),
      TorqueGeneratedExportedMacrosAssembler(state) {
  if (DEBUG_BOOL && FLAG_csa_trap_on_node != nullptr) {
    HandleBreakOnNode();
  }
}

void CodeStubAssembler::HandleBreakOnNode() {
  // FLAG_csa_trap_on_node should be in a form "STUB,NODE" where STUB is a
  // string specifying the name of a stub and NODE is number specifying node id.
  const char* name = state()->name();
  size_t name_length = strlen(name);
  if (strncmp(FLAG_csa_trap_on_node, name, name_length) != 0) {
    // Different name.
    return;
  }
  size_t option_length = strlen(FLAG_csa_trap_on_node);
  if (option_length < name_length + 2 ||
      FLAG_csa_trap_on_node[name_length] != ',') {
    // Option is too short.
    return;
  }
  const char* start = &FLAG_csa_trap_on_node[name_length + 1];
  char* end;
  int node_id = static_cast<int>(strtol(start, &end, 10));
  if (start == end) {
    // Bad node id.
    return;
  }
  BreakOnNode(node_id);
}

void CodeStubAssembler::Assert(const BranchGenerator& branch,
                               const char* message, const char* file, int line,
                               std::initializer_list<ExtraNode> extra_nodes) {
#if defined(DEBUG)
  if (FLAG_debug_code) {
    Check(branch, message, file, line, extra_nodes);
  }
#endif
}

void CodeStubAssembler::Assert(const NodeGenerator<BoolT>& condition_body,
                               const char* message, const char* file, int line,
                               std::initializer_list<ExtraNode> extra_nodes) {
#if defined(DEBUG)
  if (FLAG_debug_code) {
    Check(condition_body, message, file, line, extra_nodes);
  }
#endif
}

void CodeStubAssembler::Assert(TNode<Word32T> condition_node,
                               const char* message, const char* file, int line,
                               std::initializer_list<ExtraNode> extra_nodes) {
#if defined(DEBUG)
  if (FLAG_debug_code) {
    Check(condition_node, message, file, line, extra_nodes);
  }
#endif
}

void CodeStubAssembler::Check(const BranchGenerator& branch,
                              const char* message, const char* file, int line,
                              std::initializer_list<ExtraNode> extra_nodes) {
  Label ok(this);
  Label not_ok(this, Label::kDeferred);
  if (message != nullptr && FLAG_code_comments) {
    Comment("[ Assert: ", message);
  } else {
    Comment("[ Assert");
  }
  branch(&ok, &not_ok);

  BIND(&not_ok);
  std::vector<FileAndLine> file_and_line;
  if (file != nullptr) {
    file_and_line.push_back({file, line});
  }
  FailAssert(message, file_and_line, extra_nodes);

  BIND(&ok);
  Comment("] Assert");
}

void CodeStubAssembler::Check(const NodeGenerator<BoolT>& condition_body,
                              const char* message, const char* file, int line,
                              std::initializer_list<ExtraNode> extra_nodes) {
  BranchGenerator branch = [=](Label* ok, Label* not_ok) {
    TNode<BoolT> condition = condition_body();
    Branch(condition, ok, not_ok);
  };

  Check(branch, message, file, line, extra_nodes);
}

void CodeStubAssembler::Check(TNode<Word32T> condition_node,
                              const char* message, const char* file, int line,
                              std::initializer_list<ExtraNode> extra_nodes) {
  BranchGenerator branch = [=](Label* ok, Label* not_ok) {
    Branch(condition_node, ok, not_ok);
  };

  Check(branch, message, file, line, extra_nodes);
}

void CodeStubAssembler::IncrementCallCount(
    TNode<FeedbackVector> feedback_vector, TNode<UintPtrT> slot_id) {
  Comment("increment call count");
  TNode<Smi> call_count =
      CAST(LoadFeedbackVectorSlot(feedback_vector, slot_id, kTaggedSize));
  // The lowest {FeedbackNexus::CallCountField::kShift} bits of the call
  // count are used as flags. To increment the call count by 1 we hence
  // have to increment by 1 << {FeedbackNexus::CallCountField::kShift}.
  TNode<Smi> new_count = SmiAdd(
      call_count, SmiConstant(1 << FeedbackNexus::CallCountField::kShift));
  // Count is Smi, so we don't need a write barrier.
  StoreFeedbackVectorSlot(feedback_vector, slot_id, new_count,
                          SKIP_WRITE_BARRIER, kTaggedSize);
}

void CodeStubAssembler::FastCheck(TNode<BoolT> condition) {
  Label ok(this), not_ok(this, Label::kDeferred);
  Branch(condition, &ok, &not_ok);
  BIND(&not_ok);
  Unreachable();
  BIND(&ok);
}

void CodeStubAssembler::FailAssert(
    const char* message, const std::vector<FileAndLine>& files_and_lines,
    std::initializer_list<ExtraNode> extra_nodes) {
  DCHECK_NOT_NULL(message);
  EmbeddedVector<char, 1024> chars;
  std::stringstream stream;
  for (auto it = files_and_lines.rbegin(); it != files_and_lines.rend(); ++it) {
    if (it->first != nullptr) {
      stream << " [" << it->first << ":" << it->second << "]";
#ifndef DEBUG
      // To limit the size of these strings in release builds, we include only
      // the innermost macro's file name and line number.
      break;
#endif
    }
  }
  std::string files_and_lines_text = stream.str();
  if (files_and_lines_text.size() != 0) {
    SNPrintF(chars, "%s%s", message, files_and_lines_text.c_str());
    message = chars.begin();
  }
  TNode<String> message_node = StringConstant(message);

#ifdef DEBUG
  // Only print the extra nodes in debug builds.
  for (auto& node : extra_nodes) {
    CallRuntime(Runtime::kPrintWithNameForAssert, SmiConstant(0),
                StringConstant(node.second), node.first);
  }
#endif

  AbortCSAAssert(message_node);
  Unreachable();
}

TNode<Int32T> CodeStubAssembler::SelectInt32Constant(TNode<BoolT> condition,
                                                     int true_value,
                                                     int false_value) {
  return SelectConstant<Int32T>(condition, Int32Constant(true_value),
                                Int32Constant(false_value));
}

TNode<IntPtrT> CodeStubAssembler::SelectIntPtrConstant(TNode<BoolT> condition,
                                                       int true_value,
                                                       int false_value) {
  return SelectConstant<IntPtrT>(condition, IntPtrConstant(true_value),
                                 IntPtrConstant(false_value));
}

TNode<Oddball> CodeStubAssembler::SelectBooleanConstant(
    TNode<BoolT> condition) {
  return SelectConstant<Oddball>(condition, TrueConstant(), FalseConstant());
}

TNode<Smi> CodeStubAssembler::SelectSmiConstant(TNode<BoolT> condition,
                                                Smi true_value,
                                                Smi false_value) {
  return SelectConstant<Smi>(condition, SmiConstant(true_value),
                             SmiConstant(false_value));
}

TNode<Smi> CodeStubAssembler::NoContextConstant() {
  return SmiConstant(Context::kNoContext);
}

#define HEAP_CONSTANT_ACCESSOR(rootIndexName, rootAccessorName, name)        \
  TNode<std::remove_pointer<std::remove_reference<decltype(                  \
      std::declval<Heap>().rootAccessorName())>::type>::type>                \
      CodeStubAssembler::name##Constant() {                                  \
    return UncheckedCast<std::remove_pointer<std::remove_reference<decltype( \
        std::declval<Heap>().rootAccessorName())>::type>::type>(             \
        LoadRoot(RootIndex::k##rootIndexName));                              \
  }
HEAP_MUTABLE_IMMOVABLE_OBJECT_LIST(HEAP_CONSTANT_ACCESSOR)
#undef HEAP_CONSTANT_ACCESSOR

#define HEAP_CONSTANT_ACCESSOR(rootIndexName, rootAccessorName, name)        \
  TNode<std::remove_pointer<std::remove_reference<decltype(                  \
      std::declval<ReadOnlyRoots>().rootAccessorName())>::type>::type>       \
      CodeStubAssembler::name##Constant() {                                  \
    return UncheckedCast<std::remove_pointer<std::remove_reference<decltype( \
        std::declval<ReadOnlyRoots>().rootAccessorName())>::type>::type>(    \
        LoadRoot(RootIndex::k##rootIndexName));                              \
  }
HEAP_IMMUTABLE_IMMOVABLE_OBJECT_LIST(HEAP_CONSTANT_ACCESSOR)
#undef HEAP_CONSTANT_ACCESSOR

#define HEAP_CONSTANT_TEST(rootIndexName, rootAccessorName, name)          \
  TNode<BoolT> CodeStubAssembler::Is##name(SloppyTNode<Object> value) {    \
    return TaggedEqual(value, name##Constant());                           \
  }                                                                        \
  TNode<BoolT> CodeStubAssembler::IsNot##name(SloppyTNode<Object> value) { \
    return TaggedNotEqual(value, name##Constant());                        \
  }
HEAP_IMMOVABLE_OBJECT_LIST(HEAP_CONSTANT_TEST)
#undef HEAP_CONSTANT_TEST

TNode<BInt> CodeStubAssembler::BIntConstant(int value) {
#if defined(BINT_IS_SMI)
  return SmiConstant(value);
#elif defined(BINT_IS_INTPTR)
  return IntPtrConstant(value);
#else
#error Unknown architecture.
#endif
}

template <>
TNode<Smi> CodeStubAssembler::IntPtrOrSmiConstant<Smi>(int value) {
  return SmiConstant(value);
}

template <>
TNode<IntPtrT> CodeStubAssembler::IntPtrOrSmiConstant<IntPtrT>(int value) {
  return IntPtrConstant(value);
}

template <>
TNode<UintPtrT> CodeStubAssembler::IntPtrOrSmiConstant<UintPtrT>(int value) {
  return Unsigned(IntPtrConstant(value));
}

template <>
TNode<RawPtrT> CodeStubAssembler::IntPtrOrSmiConstant<RawPtrT>(int value) {
  return ReinterpretCast<RawPtrT>(IntPtrConstant(value));
}

bool CodeStubAssembler::TryGetIntPtrOrSmiConstantValue(
    TNode<Smi> maybe_constant, int* value) {
  Smi smi_constant;
  if (ToSmiConstant(maybe_constant, &smi_constant)) {
    *value = Smi::ToInt(smi_constant);
    return true;
  }
  return false;
}

bool CodeStubAssembler::TryGetIntPtrOrSmiConstantValue(
    TNode<IntPtrT> maybe_constant, int* value) {
  int32_t int32_constant;
    if (ToInt32Constant(maybe_constant, &int32_constant)) {
      *value = int32_constant;
      return true;
    }
  return false;
}

TNode<IntPtrT> CodeStubAssembler::IntPtrRoundUpToPowerOfTwo32(
    TNode<IntPtrT> value) {
  Comment("IntPtrRoundUpToPowerOfTwo32");
  CSA_ASSERT(this, UintPtrLessThanOrEqual(value, IntPtrConstant(0x80000000u)));
  value = Signed(IntPtrSub(value, IntPtrConstant(1)));
  for (int i = 1; i <= 16; i *= 2) {
    value = Signed(WordOr(value, WordShr(value, IntPtrConstant(i))));
  }
  return Signed(IntPtrAdd(value, IntPtrConstant(1)));
}

TNode<BoolT> CodeStubAssembler::WordIsPowerOfTwo(SloppyTNode<IntPtrT> value) {
  intptr_t constant;
  if (ToIntPtrConstant(value, &constant)) {
    return BoolConstant(base::bits::IsPowerOfTwo(constant));
  }
  // value && !(value & (value - 1))
  return IntPtrEqual(
      Select<IntPtrT>(
          IntPtrEqual(value, IntPtrConstant(0)),
          [=] { return IntPtrConstant(1); },
          [=] { return WordAnd(value, IntPtrSub(value, IntPtrConstant(1))); }),
      IntPtrConstant(0));
}

TNode<Float64T> CodeStubAssembler::Float64Round(SloppyTNode<Float64T> x) {
  TNode<Float64T> one = Float64Constant(1.0);
  TNode<Float64T> one_half = Float64Constant(0.5);

  Label return_x(this);

  // Round up {x} towards Infinity.
  TVARIABLE(Float64T, var_x, Float64Ceil(x));

  GotoIf(Float64LessThanOrEqual(Float64Sub(var_x.value(), one_half), x),
         &return_x);
  var_x = Float64Sub(var_x.value(), one);
  Goto(&return_x);

  BIND(&return_x);
  return TNode<Float64T>::UncheckedCast(var_x.value());
}

TNode<Float64T> CodeStubAssembler::Float64Ceil(SloppyTNode<Float64T> x) {
  if (IsFloat64RoundUpSupported()) {
    return Float64RoundUp(x);
  }

  TNode<Float64T> one = Float64Constant(1.0);
  TNode<Float64T> zero = Float64Constant(0.0);
  TNode<Float64T> two_52 = Float64Constant(4503599627370496.0E0);
  TNode<Float64T> minus_two_52 = Float64Constant(-4503599627370496.0E0);

  TVARIABLE(Float64T, var_x, x);
  Label return_x(this), return_minus_x(this);

  // Check if {x} is greater than zero.
  Label if_xgreaterthanzero(this), if_xnotgreaterthanzero(this);
  Branch(Float64GreaterThan(x, zero), &if_xgreaterthanzero,
         &if_xnotgreaterthanzero);

  BIND(&if_xgreaterthanzero);
  {
    // Just return {x} unless it's in the range ]0,2^52[.
    GotoIf(Float64GreaterThanOrEqual(x, two_52), &return_x);

    // Round positive {x} towards Infinity.
    var_x = Float64Sub(Float64Add(two_52, x), two_52);
    GotoIfNot(Float64LessThan(var_x.value(), x), &return_x);
    var_x = Float64Add(var_x.value(), one);
    Goto(&return_x);
  }

  BIND(&if_xnotgreaterthanzero);
  {
    // Just return {x} unless it's in the range ]-2^52,0[
    GotoIf(Float64LessThanOrEqual(x, minus_two_52), &return_x);
    GotoIfNot(Float64LessThan(x, zero), &return_x);

    // Round negated {x} towards Infinity and return the result negated.
    TNode<Float64T> minus_x = Float64Neg(x);
    var_x = Float64Sub(Float64Add(two_52, minus_x), two_52);
    GotoIfNot(Float64GreaterThan(var_x.value(), minus_x), &return_minus_x);
    var_x = Float64Sub(var_x.value(), one);
    Goto(&return_minus_x);
  }

  BIND(&return_minus_x);
  var_x = Float64Neg(var_x.value());
  Goto(&return_x);

  BIND(&return_x);
  return TNode<Float64T>::UncheckedCast(var_x.value());
}

TNode<Float64T> CodeStubAssembler::Float64Floor(SloppyTNode<Float64T> x) {
  if (IsFloat64RoundDownSupported()) {
    return Float64RoundDown(x);
  }

  TNode<Float64T> one = Float64Constant(1.0);
  TNode<Float64T> zero = Float64Constant(0.0);
  TNode<Float64T> two_52 = Float64Constant(4503599627370496.0E0);
  TNode<Float64T> minus_two_52 = Float64Constant(-4503599627370496.0E0);

  TVARIABLE(Float64T, var_x, x);
  Label return_x(this), return_minus_x(this);

  // Check if {x} is greater than zero.
  Label if_xgreaterthanzero(this), if_xnotgreaterthanzero(this);
  Branch(Float64GreaterThan(x, zero), &if_xgreaterthanzero,
         &if_xnotgreaterthanzero);

  BIND(&if_xgreaterthanzero);
  {
    // Just return {x} unless it's in the range ]0,2^52[.
    GotoIf(Float64GreaterThanOrEqual(x, two_52), &return_x);

    // Round positive {x} towards -Infinity.
    var_x = Float64Sub(Float64Add(two_52, x), two_52);
    GotoIfNot(Float64GreaterThan(var_x.value(), x), &return_x);
    var_x = Float64Sub(var_x.value(), one);
    Goto(&return_x);
  }

  BIND(&if_xnotgreaterthanzero);
  {
    // Just return {x} unless it's in the range ]-2^52,0[
    GotoIf(Float64LessThanOrEqual(x, minus_two_52), &return_x);
    GotoIfNot(Float64LessThan(x, zero), &return_x);

    // Round negated {x} towards -Infinity and return the result negated.
    TNode<Float64T> minus_x = Float64Neg(x);
    var_x = Float64Sub(Float64Add(two_52, minus_x), two_52);
    GotoIfNot(Float64LessThan(var_x.value(), minus_x), &return_minus_x);
    var_x = Float64Add(var_x.value(), one);
    Goto(&return_minus_x);
  }

  BIND(&return_minus_x);
  var_x = Float64Neg(var_x.value());
  Goto(&return_x);

  BIND(&return_x);
  return TNode<Float64T>::UncheckedCast(var_x.value());
}

TNode<Float64T> CodeStubAssembler::Float64RoundToEven(SloppyTNode<Float64T> x) {
  if (IsFloat64RoundTiesEvenSupported()) {
    return Float64RoundTiesEven(x);
  }
  // See ES#sec-touint8clamp for details.
  TNode<Float64T> f = Float64Floor(x);
  TNode<Float64T> f_and_half = Float64Add(f, Float64Constant(0.5));

  TVARIABLE(Float64T, var_result);
  Label return_f(this), return_f_plus_one(this), done(this);

  GotoIf(Float64LessThan(f_and_half, x), &return_f_plus_one);
  GotoIf(Float64LessThan(x, f_and_half), &return_f);
  {
    TNode<Float64T> f_mod_2 = Float64Mod(f, Float64Constant(2.0));
    Branch(Float64Equal(f_mod_2, Float64Constant(0.0)), &return_f,
           &return_f_plus_one);
  }

  BIND(&return_f);
  var_result = f;
  Goto(&done);

  BIND(&return_f_plus_one);
  var_result = Float64Add(f, Float64Constant(1.0));
  Goto(&done);

  BIND(&done);
  return TNode<Float64T>::UncheckedCast(var_result.value());
}

TNode<Float64T> CodeStubAssembler::Float64Trunc(SloppyTNode<Float64T> x) {
  if (IsFloat64RoundTruncateSupported()) {
    return Float64RoundTruncate(x);
  }

  TNode<Float64T> one = Float64Constant(1.0);
  TNode<Float64T> zero = Float64Constant(0.0);
  TNode<Float64T> two_52 = Float64Constant(4503599627370496.0E0);
  TNode<Float64T> minus_two_52 = Float64Constant(-4503599627370496.0E0);

  TVARIABLE(Float64T, var_x, x);
  Label return_x(this), return_minus_x(this);

  // Check if {x} is greater than 0.
  Label if_xgreaterthanzero(this), if_xnotgreaterthanzero(this);
  Branch(Float64GreaterThan(x, zero), &if_xgreaterthanzero,
         &if_xnotgreaterthanzero);

  BIND(&if_xgreaterthanzero);
  {
    if (IsFloat64RoundDownSupported()) {
      var_x = Float64RoundDown(x);
    } else {
      // Just return {x} unless it's in the range ]0,2^52[.
      GotoIf(Float64GreaterThanOrEqual(x, two_52), &return_x);

      // Round positive {x} towards -Infinity.
      var_x = Float64Sub(Float64Add(two_52, x), two_52);
      GotoIfNot(Float64GreaterThan(var_x.value(), x), &return_x);
      var_x = Float64Sub(var_x.value(), one);
    }
    Goto(&return_x);
  }

  BIND(&if_xnotgreaterthanzero);
  {
    if (IsFloat64RoundUpSupported()) {
      var_x = Float64RoundUp(x);
      Goto(&return_x);
    } else {
      // Just return {x} unless its in the range ]-2^52,0[.
      GotoIf(Float64LessThanOrEqual(x, minus_two_52), &return_x);
      GotoIfNot(Float64LessThan(x, zero), &return_x);

      // Round negated {x} towards -Infinity and return result negated.
      TNode<Float64T> minus_x = Float64Neg(x);
      var_x = Float64Sub(Float64Add(two_52, minus_x), two_52);
      GotoIfNot(Float64GreaterThan(var_x.value(), minus_x), &return_minus_x);
      var_x = Float64Sub(var_x.value(), one);
      Goto(&return_minus_x);
    }
  }

  BIND(&return_minus_x);
  var_x = Float64Neg(var_x.value());
  Goto(&return_x);

  BIND(&return_x);
  return TNode<Float64T>::UncheckedCast(var_x.value());
}

template <>
TNode<Smi> CodeStubAssembler::TaggedToParameter(TNode<Smi> value) {
  return value;
}

template <>
TNode<IntPtrT> CodeStubAssembler::TaggedToParameter(TNode<Smi> value) {
  return SmiUntag(value);
}

TNode<IntPtrT> CodeStubAssembler::TaggedIndexToIntPtr(
    TNode<TaggedIndex> value) {
  return Signed(WordSarShiftOutZeros(BitcastTaggedToWordForTagAndSmiBits(value),
                                     IntPtrConstant(kSmiTagSize)));
}

TNode<TaggedIndex> CodeStubAssembler::IntPtrToTaggedIndex(
    TNode<IntPtrT> value) {
  return ReinterpretCast<TaggedIndex>(
      BitcastWordToTaggedSigned(WordShl(value, IntPtrConstant(kSmiTagSize))));
}

TNode<Smi> CodeStubAssembler::TaggedIndexToSmi(TNode<TaggedIndex> value) {
  if (SmiValuesAre32Bits()) {
    DCHECK_EQ(kSmiShiftSize, 31);
    return BitcastWordToTaggedSigned(
        WordShl(BitcastTaggedToWordForTagAndSmiBits(value),
                IntPtrConstant(kSmiShiftSize)));
  }
  DCHECK(SmiValuesAre31Bits());
  DCHECK_EQ(kSmiShiftSize, 0);
  return ReinterpretCast<Smi>(value);
}

TNode<TaggedIndex> CodeStubAssembler::SmiToTaggedIndex(TNode<Smi> value) {
  if (kSystemPointerSize == kInt32Size) {
    return ReinterpretCast<TaggedIndex>(value);
  }
  if (SmiValuesAre32Bits()) {
    DCHECK_EQ(kSmiShiftSize, 31);
    return ReinterpretCast<TaggedIndex>(BitcastWordToTaggedSigned(
        WordSar(BitcastTaggedToWordForTagAndSmiBits(value),
                IntPtrConstant(kSmiShiftSize))));
  }
  DCHECK(SmiValuesAre31Bits());
  DCHECK_EQ(kSmiShiftSize, 0);
  // Just sign-extend the lower 32 bits.
  TNode<Int32T> raw =
      TruncateWordToInt32(BitcastTaggedToWordForTagAndSmiBits(value));
  return ReinterpretCast<TaggedIndex>(
      BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(raw)));
}

TNode<Smi> CodeStubAssembler::NormalizeSmiIndex(TNode<Smi> smi_index) {
  if (COMPRESS_POINTERS_BOOL) {
    TNode<Int32T> raw =
        TruncateWordToInt32(BitcastTaggedToWordForTagAndSmiBits(smi_index));
    smi_index = BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(raw));
  }
  return smi_index;
}

TNode<Smi> CodeStubAssembler::SmiFromInt32(SloppyTNode<Int32T> value) {
  if (COMPRESS_POINTERS_BOOL) {
    static_assert(!COMPRESS_POINTERS_BOOL || (kSmiShiftSize + kSmiTagSize == 1),
                  "Use shifting instead of add");
    return BitcastWordToTaggedSigned(
        ChangeUint32ToWord(Int32Add(value, value)));
  }
  return SmiTag(ChangeInt32ToIntPtr(value));
}

TNode<Smi> CodeStubAssembler::SmiFromUint32(TNode<Uint32T> value) {
  CSA_ASSERT(this, IntPtrLessThan(ChangeUint32ToWord(value),
                                  IntPtrConstant(Smi::kMaxValue)));
  return SmiFromInt32(Signed(value));
}

TNode<BoolT> CodeStubAssembler::IsValidPositiveSmi(TNode<IntPtrT> value) {
  intptr_t constant_value;
  if (ToIntPtrConstant(value, &constant_value)) {
    return (static_cast<uintptr_t>(constant_value) <=
            static_cast<uintptr_t>(Smi::kMaxValue))
               ? Int32TrueConstant()
               : Int32FalseConstant();
  }

  return UintPtrLessThanOrEqual(value, IntPtrConstant(Smi::kMaxValue));
}

TNode<Smi> CodeStubAssembler::SmiTag(SloppyTNode<IntPtrT> value) {
  int32_t constant_value;
  if (ToInt32Constant(value, &constant_value) && Smi::IsValid(constant_value)) {
    return SmiConstant(constant_value);
  }
  if (COMPRESS_POINTERS_BOOL) {
    return SmiFromInt32(TruncateIntPtrToInt32(value));
  }
  TNode<Smi> smi =
      BitcastWordToTaggedSigned(WordShl(value, SmiShiftBitsConstant()));
  return smi;
}

TNode<IntPtrT> CodeStubAssembler::SmiUntag(SloppyTNode<Smi> value) {
  intptr_t constant_value;
  if (ToIntPtrConstant(value, &constant_value)) {
    return IntPtrConstant(constant_value >> (kSmiShiftSize + kSmiTagSize));
  }
  TNode<IntPtrT> raw_bits = BitcastTaggedToWordForTagAndSmiBits(value);
  if (COMPRESS_POINTERS_BOOL) {
    // Clear the upper half using sign-extension.
    raw_bits = ChangeInt32ToIntPtr(TruncateIntPtrToInt32(raw_bits));
  }
  return Signed(WordSarShiftOutZeros(raw_bits, SmiShiftBitsConstant()));
}

TNode<Int32T> CodeStubAssembler::SmiToInt32(SloppyTNode<Smi> value) {
  if (COMPRESS_POINTERS_BOOL) {
    return Signed(Word32SarShiftOutZeros(
        TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(value)),
        SmiShiftBitsConstant32()));
  }
  TNode<IntPtrT> result = SmiUntag(value);
  return TruncateIntPtrToInt32(result);
}

TNode<Float64T> CodeStubAssembler::SmiToFloat64(SloppyTNode<Smi> value) {
  return ChangeInt32ToFloat64(SmiToInt32(value));
}

TNode<Smi> CodeStubAssembler::SmiMax(TNode<Smi> a, TNode<Smi> b) {
  return SelectConstant<Smi>(SmiLessThan(a, b), b, a);
}

TNode<Smi> CodeStubAssembler::SmiMin(TNode<Smi> a, TNode<Smi> b) {
  return SelectConstant<Smi>(SmiLessThan(a, b), a, b);
}

TNode<IntPtrT> CodeStubAssembler::TryIntPtrAdd(TNode<IntPtrT> a,
                                               TNode<IntPtrT> b,
                                               Label* if_overflow) {
  TNode<PairT<IntPtrT, BoolT>> pair = IntPtrAddWithOverflow(a, b);
  TNode<BoolT> overflow = Projection<1>(pair);
  GotoIf(overflow, if_overflow);
  return Projection<0>(pair);
}

TNode<IntPtrT> CodeStubAssembler::TryIntPtrSub(TNode<IntPtrT> a,
                                               TNode<IntPtrT> b,
                                               Label* if_overflow) {
  TNode<PairT<IntPtrT, BoolT>> pair = IntPtrSubWithOverflow(a, b);
  TNode<BoolT> overflow = Projection<1>(pair);
  GotoIf(overflow, if_overflow);
  return Projection<0>(pair);
}

TNode<Int32T> CodeStubAssembler::TryInt32Mul(TNode<Int32T> a, TNode<Int32T> b,
                                             Label* if_overflow) {
  TNode<PairT<Int32T, BoolT>> pair = Int32MulWithOverflow(a, b);
  TNode<BoolT> overflow = Projection<1>(pair);
  GotoIf(overflow, if_overflow);
  return Projection<0>(pair);
}

TNode<Smi> CodeStubAssembler::TrySmiAdd(TNode<Smi> lhs, TNode<Smi> rhs,
                                        Label* if_overflow) {
  if (SmiValuesAre32Bits()) {
    return BitcastWordToTaggedSigned(
        TryIntPtrAdd(BitcastTaggedToWordForTagAndSmiBits(lhs),
                     BitcastTaggedToWordForTagAndSmiBits(rhs), if_overflow));
  } else {
    DCHECK(SmiValuesAre31Bits());
    TNode<PairT<Int32T, BoolT>> pair = Int32AddWithOverflow(
        TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(lhs)),
        TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(rhs)));
    TNode<BoolT> overflow = Projection<1>(pair);
    GotoIf(overflow, if_overflow);
    TNode<Int32T> result = Projection<0>(pair);
    return BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(result));
  }
}

TNode<Smi> CodeStubAssembler::TrySmiSub(TNode<Smi> lhs, TNode<Smi> rhs,
                                        Label* if_overflow) {
  if (SmiValuesAre32Bits()) {
    TNode<PairT<IntPtrT, BoolT>> pair =
        IntPtrSubWithOverflow(BitcastTaggedToWordForTagAndSmiBits(lhs),
                              BitcastTaggedToWordForTagAndSmiBits(rhs));
    TNode<BoolT> overflow = Projection<1>(pair);
    GotoIf(overflow, if_overflow);
    TNode<IntPtrT> result = Projection<0>(pair);
    return BitcastWordToTaggedSigned(result);
  } else {
    DCHECK(SmiValuesAre31Bits());
    TNode<PairT<Int32T, BoolT>> pair = Int32SubWithOverflow(
        TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(lhs)),
        TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(rhs)));
    TNode<BoolT> overflow = Projection<1>(pair);
    GotoIf(overflow, if_overflow);
    TNode<Int32T> result = Projection<0>(pair);
    return BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(result));
  }
}

TNode<Smi> CodeStubAssembler::TrySmiAbs(TNode<Smi> a, Label* if_overflow) {
  if (SmiValuesAre32Bits()) {
    TNode<PairT<IntPtrT, BoolT>> pair =
        IntPtrAbsWithOverflow(BitcastTaggedToWordForTagAndSmiBits(a));
    TNode<BoolT> overflow = Projection<1>(pair);
    GotoIf(overflow, if_overflow);
    TNode<IntPtrT> result = Projection<0>(pair);
    return BitcastWordToTaggedSigned(result);
  } else {
    CHECK(SmiValuesAre31Bits());
    CHECK(IsInt32AbsWithOverflowSupported());
    TNode<PairT<Int32T, BoolT>> pair = Int32AbsWithOverflow(
        TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(a)));
    TNode<BoolT> overflow = Projection<1>(pair);
    GotoIf(overflow, if_overflow);
    TNode<Int32T> result = Projection<0>(pair);
    return BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(result));
  }
}

TNode<Number> CodeStubAssembler::NumberMax(TNode<Number> a, TNode<Number> b) {
  // TODO(danno): This could be optimized by specifically handling smi cases.
  TVARIABLE(Number, result);
  Label done(this), greater_than_equal_a(this), greater_than_equal_b(this);
  GotoIfNumberGreaterThanOrEqual(a, b, &greater_than_equal_a);
  GotoIfNumberGreaterThanOrEqual(b, a, &greater_than_equal_b);
  result = NanConstant();
  Goto(&done);
  BIND(&greater_than_equal_a);
  result = a;
  Goto(&done);
  BIND(&greater_than_equal_b);
  result = b;
  Goto(&done);
  BIND(&done);
  return result.value();
}

TNode<Number> CodeStubAssembler::NumberMin(TNode<Number> a, TNode<Number> b) {
  // TODO(danno): This could be optimized by specifically handling smi cases.
  TVARIABLE(Number, result);
  Label done(this), greater_than_equal_a(this), greater_than_equal_b(this);
  GotoIfNumberGreaterThanOrEqual(a, b, &greater_than_equal_a);
  GotoIfNumberGreaterThanOrEqual(b, a, &greater_than_equal_b);
  result = NanConstant();
  Goto(&done);
  BIND(&greater_than_equal_a);
  result = b;
  Goto(&done);
  BIND(&greater_than_equal_b);
  result = a;
  Goto(&done);
  BIND(&done);
  return result.value();
}

TNode<Number> CodeStubAssembler::SmiMod(TNode<Smi> a, TNode<Smi> b) {
  TVARIABLE(Number, var_result);
  Label return_result(this, &var_result),
      return_minuszero(this, Label::kDeferred),
      return_nan(this, Label::kDeferred);

  // Untag {a} and {b}.
  TNode<Int32T> int_a = SmiToInt32(a);
  TNode<Int32T> int_b = SmiToInt32(b);

  // Return NaN if {b} is zero.
  GotoIf(Word32Equal(int_b, Int32Constant(0)), &return_nan);

  // Check if {a} is non-negative.
  Label if_aisnotnegative(this), if_aisnegative(this, Label::kDeferred);
  Branch(Int32LessThanOrEqual(Int32Constant(0), int_a), &if_aisnotnegative,
         &if_aisnegative);

  BIND(&if_aisnotnegative);
  {
    // Fast case, don't need to check any other edge cases.
    TNode<Int32T> r = Int32Mod(int_a, int_b);
    var_result = SmiFromInt32(r);
    Goto(&return_result);
  }

  BIND(&if_aisnegative);
  {
    if (SmiValuesAre32Bits()) {
      // Check if {a} is kMinInt and {b} is -1 (only relevant if the
      // kMinInt is actually representable as a Smi).
      Label join(this);
      GotoIfNot(Word32Equal(int_a, Int32Constant(kMinInt)), &join);
      GotoIf(Word32Equal(int_b, Int32Constant(-1)), &return_minuszero);
      Goto(&join);
      BIND(&join);
    }

    // Perform the integer modulus operation.
    TNode<Int32T> r = Int32Mod(int_a, int_b);

    // Check if {r} is zero, and if so return -0, because we have to
    // take the sign of the left hand side {a}, which is negative.
    GotoIf(Word32Equal(r, Int32Constant(0)), &return_minuszero);

    // The remainder {r} can be outside the valid Smi range on 32bit
    // architectures, so we cannot just say SmiFromInt32(r) here.
    var_result = ChangeInt32ToTagged(r);
    Goto(&return_result);
  }

  BIND(&return_minuszero);
  var_result = MinusZeroConstant();
  Goto(&return_result);

  BIND(&return_nan);
  var_result = NanConstant();
  Goto(&return_result);

  BIND(&return_result);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::SmiMul(TNode<Smi> a, TNode<Smi> b) {
  TVARIABLE(Number, var_result);
  TVARIABLE(Float64T, var_lhs_float64);
  TVARIABLE(Float64T, var_rhs_float64);
  Label return_result(this, &var_result);

  // Both {a} and {b} are Smis. Convert them to integers and multiply.
  TNode<Int32T> lhs32 = SmiToInt32(a);
  TNode<Int32T> rhs32 = SmiToInt32(b);
  auto pair = Int32MulWithOverflow(lhs32, rhs32);

  TNode<BoolT> overflow = Projection<1>(pair);

  // Check if the multiplication overflowed.
  Label if_overflow(this, Label::kDeferred), if_notoverflow(this);
  Branch(overflow, &if_overflow, &if_notoverflow);
  BIND(&if_notoverflow);
  {
    // If the answer is zero, we may need to return -0.0, depending on the
    // input.
    Label answer_zero(this), answer_not_zero(this);
    TNode<Int32T> answer = Projection<0>(pair);
    TNode<Int32T> zero = Int32Constant(0);
    Branch(Word32Equal(answer, zero), &answer_zero, &answer_not_zero);
    BIND(&answer_not_zero);
    {
      var_result = ChangeInt32ToTagged(answer);
      Goto(&return_result);
    }
    BIND(&answer_zero);
    {
      TNode<Int32T> or_result = Word32Or(lhs32, rhs32);
      Label if_should_be_negative_zero(this), if_should_be_zero(this);
      Branch(Int32LessThan(or_result, zero), &if_should_be_negative_zero,
             &if_should_be_zero);
      BIND(&if_should_be_negative_zero);
      {
        var_result = MinusZeroConstant();
        Goto(&return_result);
      }
      BIND(&if_should_be_zero);
      {
        var_result = SmiConstant(0);
        Goto(&return_result);
      }
    }
  }
  BIND(&if_overflow);
  {
    var_lhs_float64 = SmiToFloat64(a);
    var_rhs_float64 = SmiToFloat64(b);
    TNode<Float64T> value =
        Float64Mul(var_lhs_float64.value(), var_rhs_float64.value());
    var_result = AllocateHeapNumberWithValue(value);
    Goto(&return_result);
  }

  BIND(&return_result);
  return var_result.value();
}

TNode<Smi> CodeStubAssembler::TrySmiDiv(TNode<Smi> dividend, TNode<Smi> divisor,
                                        Label* bailout) {
  // Both {a} and {b} are Smis. Bailout to floating point division if {divisor}
  // is zero.
  GotoIf(TaggedEqual(divisor, SmiConstant(0)), bailout);

  // Do floating point division if {dividend} is zero and {divisor} is
  // negative.
  Label dividend_is_zero(this), dividend_is_not_zero(this);
  Branch(TaggedEqual(dividend, SmiConstant(0)), &dividend_is_zero,
         &dividend_is_not_zero);

  BIND(&dividend_is_zero);
  {
    GotoIf(SmiLessThan(divisor, SmiConstant(0)), bailout);
    Goto(&dividend_is_not_zero);
  }
  BIND(&dividend_is_not_zero);

  TNode<Int32T> untagged_divisor = SmiToInt32(divisor);
  TNode<Int32T> untagged_dividend = SmiToInt32(dividend);

  // Do floating point division if {dividend} is kMinInt (or kMinInt - 1
  // if the Smi size is 31) and {divisor} is -1.
  Label divisor_is_minus_one(this), divisor_is_not_minus_one(this);
  Branch(Word32Equal(untagged_divisor, Int32Constant(-1)),
         &divisor_is_minus_one, &divisor_is_not_minus_one);

  BIND(&divisor_is_minus_one);
  {
    GotoIf(Word32Equal(
               untagged_dividend,
               Int32Constant(kSmiValueSize == 32 ? kMinInt : (kMinInt >> 1))),
           bailout);
    Goto(&divisor_is_not_minus_one);
  }
  BIND(&divisor_is_not_minus_one);

  TNode<Int32T> untagged_result = Int32Div(untagged_dividend, untagged_divisor);
  TNode<Int32T> truncated = Int32Mul(untagged_result, untagged_divisor);

  // Do floating point division if the remainder is not 0.
  GotoIf(Word32NotEqual(untagged_dividend, truncated), bailout);

  return SmiFromInt32(untagged_result);
}

TNode<Smi> CodeStubAssembler::SmiLexicographicCompare(TNode<Smi> x,
                                                      TNode<Smi> y) {
  TNode<ExternalReference> smi_lexicographic_compare =
      ExternalConstant(ExternalReference::smi_lexicographic_compare_function());
  TNode<ExternalReference> isolate_ptr =
      ExternalConstant(ExternalReference::isolate_address(isolate()));
  return CAST(CallCFunction(smi_lexicographic_compare, MachineType::AnyTagged(),
                            std::make_pair(MachineType::Pointer(), isolate_ptr),
                            std::make_pair(MachineType::AnyTagged(), x),
                            std::make_pair(MachineType::AnyTagged(), y)));
}

TNode<Int32T> CodeStubAssembler::TruncateWordToInt32(SloppyTNode<WordT> value) {
  if (Is64()) {
    return TruncateInt64ToInt32(ReinterpretCast<Int64T>(value));
  }
  return ReinterpretCast<Int32T>(value);
}

TNode<Int32T> CodeStubAssembler::TruncateIntPtrToInt32(
    SloppyTNode<IntPtrT> value) {
  if (Is64()) {
    return TruncateInt64ToInt32(ReinterpretCast<Int64T>(value));
  }
  return ReinterpretCast<Int32T>(value);
}

TNode<BoolT> CodeStubAssembler::TaggedIsSmi(TNode<MaybeObject> a) {
  STATIC_ASSERT(kSmiTagMask < kMaxUInt32);
  return Word32Equal(
      Word32And(TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(a)),
                Int32Constant(kSmiTagMask)),
      Int32Constant(0));
}

TNode<BoolT> CodeStubAssembler::TaggedIsNotSmi(TNode<MaybeObject> a) {
  return Word32BinaryNot(TaggedIsSmi(a));
}

TNode<BoolT> CodeStubAssembler::TaggedIsPositiveSmi(SloppyTNode<Object> a) {
#if defined(V8_HOST_ARCH_32_BIT) || defined(V8_31BIT_SMIS_ON_64BIT_ARCH)
  return Word32Equal(
      Word32And(
          TruncateIntPtrToInt32(BitcastTaggedToWordForTagAndSmiBits(a)),
          Uint32Constant(static_cast<uint32_t>(kSmiTagMask | kSmiSignMask))),
      Int32Constant(0));
#else
  return WordEqual(WordAnd(BitcastTaggedToWordForTagAndSmiBits(a),
                           IntPtrConstant(kSmiTagMask | kSmiSignMask)),
                   IntPtrConstant(0));
#endif
}

TNode<BoolT> CodeStubAssembler::WordIsAligned(SloppyTNode<WordT> word,
                                              size_t alignment) {
  DCHECK(base::bits::IsPowerOfTwo(alignment));
  DCHECK_LE(alignment, kMaxUInt32);
  return Word32Equal(
      Int32Constant(0),
      Word32And(TruncateWordToInt32(word),
                Uint32Constant(static_cast<uint32_t>(alignment) - 1)));
}

#if DEBUG
void CodeStubAssembler::Bind(Label* label, AssemblerDebugInfo debug_info) {
  CodeAssembler::Bind(label, debug_info);
}
#endif  // DEBUG

void CodeStubAssembler::Bind(Label* label) { CodeAssembler::Bind(label); }

TNode<Float64T> CodeStubAssembler::LoadDoubleWithHoleCheck(
    TNode<FixedDoubleArray> array, TNode<IntPtrT> index, Label* if_hole) {
  return LoadFixedDoubleArrayElement(array, index, if_hole);
}

void CodeStubAssembler::BranchIfJSReceiver(SloppyTNode<Object> object,
                                           Label* if_true, Label* if_false) {
  GotoIf(TaggedIsSmi(object), if_false);
  STATIC_ASSERT(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
  Branch(IsJSReceiver(CAST(object)), if_true, if_false);
}

void CodeStubAssembler::GotoIfForceSlowPath(Label* if_true) {
#ifdef V8_ENABLE_FORCE_SLOW_PATH
  const TNode<ExternalReference> force_slow_path_addr =
      ExternalConstant(ExternalReference::force_slow_path(isolate()));
  const TNode<Uint8T> force_slow = Load<Uint8T>(force_slow_path_addr);

  GotoIf(force_slow, if_true);
#endif
}

TNode<HeapObject> CodeStubAssembler::AllocateRaw(TNode<IntPtrT> size_in_bytes,
                                                 AllocationFlags flags,
                                                 TNode<RawPtrT> top_address,
                                                 TNode<RawPtrT> limit_address) {
  Label if_out_of_memory(this, Label::kDeferred);

  // TODO(jgruber,jkummerow): Extract the slow paths (= probably everything
  // but bump pointer allocation) into a builtin to save code space. The
  // size_in_bytes check may be moved there as well since a non-smi
  // size_in_bytes probably doesn't fit into the bump pointer region
  // (double-check that).

  intptr_t size_in_bytes_constant;
  bool size_in_bytes_is_constant = false;
  if (ToIntPtrConstant(size_in_bytes, &size_in_bytes_constant)) {
    size_in_bytes_is_constant = true;
    CHECK(Internals::IsValidSmi(size_in_bytes_constant));
    CHECK_GT(size_in_bytes_constant, 0);
  } else {
    GotoIfNot(IsValidPositiveSmi(size_in_bytes), &if_out_of_memory);
  }

  TNode<RawPtrT> top = Load<RawPtrT>(top_address);
  TNode<RawPtrT> limit = Load<RawPtrT>(limit_address);

  // If there's not enough space, call the runtime.
  TVARIABLE(Object, result);
  Label runtime_call(this, Label::kDeferred), no_runtime_call(this), out(this);

  bool needs_double_alignment = flags & kDoubleAlignment;
  bool allow_large_object_allocation = flags & kAllowLargeObjectAllocation;

  if (allow_large_object_allocation) {
    Label next(this);
    GotoIf(IsRegularHeapObjectSize(size_in_bytes), &next);

    TNode<Smi> runtime_flags = SmiConstant(Smi::FromInt(
        AllocateDoubleAlignFlag::encode(needs_double_alignment) |
        AllowLargeObjectAllocationFlag::encode(allow_large_object_allocation)));
    if (FLAG_young_generation_large_objects) {
      result =
          CallRuntime(Runtime::kAllocateInYoungGeneration, NoContextConstant(),
                      SmiTag(size_in_bytes), runtime_flags);
    } else {
      result =
          CallRuntime(Runtime::kAllocateInOldGeneration, NoContextConstant(),
                      SmiTag(size_in_bytes), runtime_flags);
    }
    Goto(&out);

    BIND(&next);
  }

  TVARIABLE(IntPtrT, adjusted_size, size_in_bytes);

  if (needs_double_alignment) {
    Label next(this);
    GotoIfNot(WordAnd(top, IntPtrConstant(kDoubleAlignmentMask)), &next);

    adjusted_size = IntPtrAdd(size_in_bytes, IntPtrConstant(4));
    Goto(&next);

    BIND(&next);
  }

  TNode<IntPtrT> new_top =
      IntPtrAdd(UncheckedCast<IntPtrT>(top), adjusted_size.value());

  Branch(UintPtrGreaterThanOrEqual(new_top, limit), &runtime_call,
         &no_runtime_call);

  BIND(&runtime_call);
  {
    TNode<Smi> runtime_flags = SmiConstant(Smi::FromInt(
        AllocateDoubleAlignFlag::encode(needs_double_alignment) |
        AllowLargeObjectAllocationFlag::encode(allow_large_object_allocation)));
    if (flags & kPretenured) {
      result =
          CallRuntime(Runtime::kAllocateInOldGeneration, NoContextConstant(),
                      SmiTag(size_in_bytes), runtime_flags);
    } else {
      result =
          CallRuntime(Runtime::kAllocateInYoungGeneration, NoContextConstant(),
                      SmiTag(size_in_bytes), runtime_flags);
    }
    Goto(&out);
  }

  // When there is enough space, return `top' and bump it up.
  BIND(&no_runtime_call);
  {
    StoreNoWriteBarrier(MachineType::PointerRepresentation(), top_address,
                        new_top);

    TVARIABLE(IntPtrT, address, UncheckedCast<IntPtrT>(top));

    if (needs_double_alignment) {
      Label next(this);
      GotoIf(IntPtrEqual(adjusted_size.value(), size_in_bytes), &next);

      // Store a filler and increase the address by 4.
      StoreNoWriteBarrier(MachineRepresentation::kTagged, top,
                          OnePointerFillerMapConstant());
      address = IntPtrAdd(UncheckedCast<IntPtrT>(top), IntPtrConstant(4));
      Goto(&next);

      BIND(&next);
    }

    result = BitcastWordToTagged(
        IntPtrAdd(address.value(), IntPtrConstant(kHeapObjectTag)));
    Goto(&out);
  }

  if (!size_in_bytes_is_constant) {
    BIND(&if_out_of_memory);
    CallRuntime(Runtime::kFatalProcessOutOfMemoryInAllocateRaw,
                NoContextConstant());
    Unreachable();
  }

  BIND(&out);
  return UncheckedCast<HeapObject>(result.value());
}

TNode<HeapObject> CodeStubAssembler::AllocateRawUnaligned(
    TNode<IntPtrT> size_in_bytes, AllocationFlags flags,
    TNode<RawPtrT> top_address, TNode<RawPtrT> limit_address) {
  DCHECK_EQ(flags & kDoubleAlignment, 0);
  return AllocateRaw(size_in_bytes, flags, top_address, limit_address);
}

TNode<HeapObject> CodeStubAssembler::AllocateRawDoubleAligned(
    TNode<IntPtrT> size_in_bytes, AllocationFlags flags,
    TNode<RawPtrT> top_address, TNode<RawPtrT> limit_address) {
#if defined(V8_HOST_ARCH_32_BIT)
  return AllocateRaw(size_in_bytes, flags | kDoubleAlignment, top_address,
                     limit_address);
#elif defined(V8_HOST_ARCH_64_BIT)
#ifdef V8_COMPRESS_POINTERS
  // TODO(ishell, v8:8875): Consider using aligned allocations once the
  // allocation alignment inconsistency is fixed. For now we keep using
  // unaligned access since both x64 and arm64 architectures (where pointer
  // compression is supported) allow unaligned access to doubles and full words.
#endif  // V8_COMPRESS_POINTERS
  // Allocation on 64 bit machine is naturally double aligned
  return AllocateRaw(size_in_bytes, flags & ~kDoubleAlignment, top_address,
                     limit_address);
#else
#error Architecture not supported
#endif
}

TNode<HeapObject> CodeStubAssembler::AllocateInNewSpace(
    TNode<IntPtrT> size_in_bytes, AllocationFlags flags) {
  DCHECK(flags == kNone || flags == kDoubleAlignment);
  CSA_ASSERT(this, IsRegularHeapObjectSize(size_in_bytes));
  return Allocate(size_in_bytes, flags);
}

TNode<HeapObject> CodeStubAssembler::Allocate(TNode<IntPtrT> size_in_bytes,
                                              AllocationFlags flags) {
  Comment("Allocate");
  bool const new_space = !(flags & kPretenured);
  bool const allow_large_objects = flags & kAllowLargeObjectAllocation;
  // For optimized allocations, we don't allow the allocation to happen in a
  // different generation than requested.
  bool const always_allocated_in_requested_space =
      !new_space || !allow_large_objects || FLAG_young_generation_large_objects;
  if (!allow_large_objects) {
    intptr_t size_constant;
    if (ToIntPtrConstant(size_in_bytes, &size_constant)) {
      CHECK_LE(size_constant, kMaxRegularHeapObjectSize);
    } else {
      CSA_ASSERT(this, IsRegularHeapObjectSize(size_in_bytes));
    }
  }
  if (!(flags & kDoubleAlignment) && always_allocated_in_requested_space) {
    return OptimizedAllocate(
        size_in_bytes,
        new_space ? AllocationType::kYoung : AllocationType::kOld,
        allow_large_objects ? AllowLargeObjects::kTrue
                            : AllowLargeObjects::kFalse);
  }
  TNode<ExternalReference> top_address = ExternalConstant(
      new_space
          ? ExternalReference::new_space_allocation_top_address(isolate())
          : ExternalReference::old_space_allocation_top_address(isolate()));
  DCHECK_EQ(kSystemPointerSize,
            ExternalReference::new_space_allocation_limit_address(isolate())
                    .address() -
                ExternalReference::new_space_allocation_top_address(isolate())
                    .address());
  DCHECK_EQ(kSystemPointerSize,
            ExternalReference::old_space_allocation_limit_address(isolate())
                    .address() -
                ExternalReference::old_space_allocation_top_address(isolate())
                    .address());
  TNode<IntPtrT> limit_address =
      IntPtrAdd(ReinterpretCast<IntPtrT>(top_address),
                IntPtrConstant(kSystemPointerSize));

  if (flags & kDoubleAlignment) {
    return AllocateRawDoubleAligned(size_in_bytes, flags,
                                    ReinterpretCast<RawPtrT>(top_address),
                                    ReinterpretCast<RawPtrT>(limit_address));
  } else {
    return AllocateRawUnaligned(size_in_bytes, flags,
                                ReinterpretCast<RawPtrT>(top_address),
                                ReinterpretCast<RawPtrT>(limit_address));
  }
}

TNode<HeapObject> CodeStubAssembler::AllocateInNewSpace(int size_in_bytes,
                                                        AllocationFlags flags) {
  CHECK(flags == kNone || flags == kDoubleAlignment);
  DCHECK_LE(size_in_bytes, kMaxRegularHeapObjectSize);
  return CodeStubAssembler::Allocate(IntPtrConstant(size_in_bytes), flags);
}

TNode<HeapObject> CodeStubAssembler::Allocate(int size_in_bytes,
                                              AllocationFlags flags) {
  return CodeStubAssembler::Allocate(IntPtrConstant(size_in_bytes), flags);
}

TNode<HeapObject> CodeStubAssembler::InnerAllocate(TNode<HeapObject> previous,
                                                   TNode<IntPtrT> offset) {
  return UncheckedCast<HeapObject>(
      BitcastWordToTagged(IntPtrAdd(BitcastTaggedToWord(previous), offset)));
}

TNode<HeapObject> CodeStubAssembler::InnerAllocate(TNode<HeapObject> previous,
                                                   int offset) {
  return InnerAllocate(previous, IntPtrConstant(offset));
}

TNode<BoolT> CodeStubAssembler::IsRegularHeapObjectSize(TNode<IntPtrT> size) {
  return UintPtrLessThanOrEqual(size,
                                IntPtrConstant(kMaxRegularHeapObjectSize));
}

void CodeStubAssembler::BranchIfToBooleanIsTrue(SloppyTNode<Object> value,
                                                Label* if_true,
                                                Label* if_false) {
  Label if_smi(this), if_notsmi(this), if_heapnumber(this, Label::kDeferred),
      if_bigint(this, Label::kDeferred);
  // Rule out false {value}.
  GotoIf(TaggedEqual(value, FalseConstant()), if_false);

  // Check if {value} is a Smi or a HeapObject.
  Branch(TaggedIsSmi(value), &if_smi, &if_notsmi);

  BIND(&if_smi);
  {
    // The {value} is a Smi, only need to check against zero.
    BranchIfSmiEqual(CAST(value), SmiConstant(0), if_false, if_true);
  }

  BIND(&if_notsmi);
  {
    TNode<HeapObject> value_heapobject = CAST(value);

    // Check if {value} is the empty string.
    GotoIf(IsEmptyString(value_heapobject), if_false);

    // The {value} is a HeapObject, load its map.
    TNode<Map> value_map = LoadMap(value_heapobject);

    // Only null, undefined and document.all have the undetectable bit set,
    // so we can return false immediately when that bit is set.
    GotoIf(IsUndetectableMap(value_map), if_false);

    // We still need to handle numbers specially, but all other {value}s
    // that make it here yield true.
    GotoIf(IsHeapNumberMap(value_map), &if_heapnumber);
    Branch(IsBigInt(value_heapobject), &if_bigint, if_true);

    BIND(&if_heapnumber);
    {
      // Load the floating point value of {value}.
      TNode<Float64T> value_value =
          LoadObjectField<Float64T>(value_heapobject, HeapNumber::kValueOffset);

      // Check if the floating point {value} is neither 0.0, -0.0 nor NaN.
      Branch(Float64LessThan(Float64Constant(0.0), Float64Abs(value_value)),
             if_true, if_false);
    }

    BIND(&if_bigint);
    {
      TNode<BigInt> bigint = CAST(value);
      TNode<Word32T> bitfield = LoadBigIntBitfield(bigint);
      TNode<Uint32T> length = DecodeWord32<BigIntBase::LengthBits>(bitfield);
      Branch(Word32Equal(length, Int32Constant(0)), if_false, if_true);
    }
  }
}

TNode<ExternalPointerT> CodeStubAssembler::ChangeUint32ToExternalPointer(
    TNode<Uint32T> value) {
  STATIC_ASSERT(kExternalPointerSize == kSystemPointerSize);
  return ReinterpretCast<ExternalPointerT>(ChangeUint32ToWord(value));
}

TNode<Uint32T> CodeStubAssembler::ChangeExternalPointerToUint32(
    TNode<ExternalPointerT> value) {
  STATIC_ASSERT(kExternalPointerSize == kSystemPointerSize);
  return Unsigned(TruncateWordToInt32(ReinterpretCast<UintPtrT>(value)));
}

void CodeStubAssembler::InitializeExternalPointerField(TNode<HeapObject> object,
                                                       TNode<IntPtrT> offset) {
#ifdef V8_HEAP_SANDBOX
  TNode<ExternalReference> external_pointer_table_address = ExternalConstant(
      ExternalReference::external_pointer_table_address(isolate()));
  TNode<Uint32T> table_length = UncheckedCast<Uint32T>(
      Load(MachineType::Uint32(), external_pointer_table_address,
           UintPtrConstant(Internals::kExternalPointerTableLengthOffset)));
  TNode<Uint32T> table_capacity = UncheckedCast<Uint32T>(
      Load(MachineType::Uint32(), external_pointer_table_address,
           UintPtrConstant(Internals::kExternalPointerTableCapacityOffset)));

  Label grow_table(this, Label::kDeferred), finish(this);

  TNode<BoolT> compare = Uint32LessThan(table_length, table_capacity);
  Branch(compare, &finish, &grow_table);

  BIND(&grow_table);
  {
    TNode<ExternalReference> table_grow_function = ExternalConstant(
        ExternalReference::external_pointer_table_grow_table_function());
    CallCFunction(
        table_grow_function, MachineType::Pointer(),
        std::make_pair(MachineType::Pointer(), external_pointer_table_address));
    Goto(&finish);
  }
  BIND(&finish);

  TNode<Uint32T> new_table_length = Uint32Add(table_length, Uint32Constant(1));
  StoreNoWriteBarrier(
      MachineRepresentation::kWord32, external_pointer_table_address,
      UintPtrConstant(Internals::kExternalPointerTableLengthOffset),
      new_table_length);

  TNode<Uint32T> index = table_length;
  TNode<ExternalPointerT> encoded = ChangeUint32ToExternalPointer(index);
  StoreObjectFieldNoWriteBarrier<ExternalPointerT>(object, offset, encoded);
#endif
}

TNode<RawPtrT> CodeStubAssembler::LoadExternalPointerFromObject(
    TNode<HeapObject> object, TNode<IntPtrT> offset,
    ExternalPointerTag external_pointer_tag) {
#ifdef V8_HEAP_SANDBOX
  TNode<ExternalReference> external_pointer_table_address = ExternalConstant(
      ExternalReference::external_pointer_table_address(isolate()));
  TNode<RawPtrT> table = UncheckedCast<RawPtrT>(
      Load(MachineType::Pointer(), external_pointer_table_address,
           UintPtrConstant(Internals::kExternalPointerTableBufferOffset)));

  TNode<ExternalPointerT> encoded =
      LoadObjectField<ExternalPointerT>(object, offset);
  TNode<Word32T> index = ChangeExternalPointerToUint32(encoded);
  // TODO(v8:10391, saelo): bounds check if table is not caged
  TNode<IntPtrT> table_offset = ElementOffsetFromIndex(
      ChangeUint32ToWord(index), SYSTEM_POINTER_ELEMENTS, 0);

  TNode<UintPtrT> entry = Load<UintPtrT>(table, table_offset);
  if (external_pointer_tag != 0) {
    TNode<UintPtrT> tag = UintPtrConstant(external_pointer_tag);
    entry = UncheckedCast<UintPtrT>(WordXor(entry, tag));
  }
  return UncheckedCast<RawPtrT>(UncheckedCast<WordT>(entry));
#else
  return LoadObjectField<RawPtrT>(object, offset);
#endif  // V8_HEAP_SANDBOX
}

void CodeStubAssembler::StoreExternalPointerToObject(
    TNode<HeapObject> object, TNode<IntPtrT> offset, TNode<RawPtrT> pointer,
    ExternalPointerTag external_pointer_tag) {
#ifdef V8_HEAP_SANDBOX
  TNode<ExternalReference> external_pointer_table_address = ExternalConstant(
      ExternalReference::external_pointer_table_address(isolate()));
  TNode<RawPtrT> table = UncheckedCast<RawPtrT>(
      Load(MachineType::Pointer(), external_pointer_table_address,
           UintPtrConstant(Internals::kExternalPointerTableBufferOffset)));

  TNode<ExternalPointerT> encoded =
      LoadObjectField<ExternalPointerT>(object, offset);
  TNode<Word32T> index = ChangeExternalPointerToUint32(encoded);
  // TODO(v8:10391, saelo): bounds check if table is not caged
  TNode<IntPtrT> table_offset = ElementOffsetFromIndex(
      ChangeUint32ToWord(index), SYSTEM_POINTER_ELEMENTS, 0);

  TNode<UintPtrT> value = UncheckedCast<UintPtrT>(pointer);
  if (external_pointer_tag != 0) {
    TNode<UintPtrT> tag = UintPtrConstant(external_pointer_tag);
    value = UncheckedCast<UintPtrT>(WordXor(pointer, tag));
  }
  StoreNoWriteBarrier(MachineType::PointerRepresentation(), table, table_offset,
                      value);
#else
  StoreObjectFieldNoWriteBarrier<RawPtrT>(object, offset, pointer);
#endif  // V8_HEAP_SANDBOX
}

TNode<Object> CodeStubAssembler::LoadFromParentFrame(int offset) {
  TNode<RawPtrT> frame_pointer = LoadParentFramePointer();
  return LoadFullTagged(frame_pointer, IntPtrConstant(offset));
}

TNode<IntPtrT> CodeStubAssembler::LoadAndUntagObjectField(
    TNode<HeapObject> object, int offset) {
  if (SmiValuesAre32Bits()) {
#if V8_TARGET_LITTLE_ENDIAN
    offset += 4;
#endif
    return ChangeInt32ToIntPtr(LoadObjectField<Int32T>(object, offset));
  } else {
    return SmiToIntPtr(LoadObjectField<Smi>(object, offset));
  }
}

TNode<Int32T> CodeStubAssembler::LoadAndUntagToWord32ObjectField(
    TNode<HeapObject> object, int offset) {
  if (SmiValuesAre32Bits()) {
#if V8_TARGET_LITTLE_ENDIAN
    offset += 4;
#endif
    return LoadObjectField<Int32T>(object, offset);
  } else {
    return SmiToInt32(LoadObjectField<Smi>(object, offset));
  }
}

TNode<Float64T> CodeStubAssembler::LoadHeapNumberValue(
    TNode<HeapObject> object) {
  CSA_ASSERT(this, Word32Or(IsHeapNumber(object), IsOddball(object)));
  STATIC_ASSERT(HeapNumber::kValueOffset == Oddball::kToNumberRawOffset);
  return LoadObjectField<Float64T>(object, HeapNumber::kValueOffset);
}

TNode<Map> CodeStubAssembler::GetInstanceTypeMap(InstanceType instance_type) {
  Handle<Map> map_handle(
      Map::GetInstanceTypeMap(ReadOnlyRoots(isolate()), instance_type),
      isolate());
  return HeapConstant(map_handle);
}

TNode<Map> CodeStubAssembler::LoadMap(TNode<HeapObject> object) {
  return LoadObjectField<Map>(object, HeapObject::kMapOffset);
}

TNode<Uint16T> CodeStubAssembler::LoadInstanceType(TNode<HeapObject> object) {
  return LoadMapInstanceType(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::HasInstanceType(TNode<HeapObject> object,
                                                InstanceType instance_type) {
  return InstanceTypeEqual(LoadInstanceType(object), instance_type);
}

TNode<BoolT> CodeStubAssembler::DoesntHaveInstanceType(
    TNode<HeapObject> object, InstanceType instance_type) {
  return Word32NotEqual(LoadInstanceType(object), Int32Constant(instance_type));
}

TNode<BoolT> CodeStubAssembler::TaggedDoesntHaveInstanceType(
    TNode<HeapObject> any_tagged, InstanceType type) {
  /* return Phi <TaggedIsSmi(val), DoesntHaveInstanceType(val, type)> */
  TNode<BoolT> tagged_is_smi = TaggedIsSmi(any_tagged);
  return Select<BoolT>(
      tagged_is_smi, [=]() { return tagged_is_smi; },
      [=]() { return DoesntHaveInstanceType(any_tagged, type); });
}

TNode<BoolT> CodeStubAssembler::IsSpecialReceiverMap(TNode<Map> map) {
  TNode<BoolT> is_special =
      IsSpecialReceiverInstanceType(LoadMapInstanceType(map));
  uint32_t mask = Map::Bits1::HasNamedInterceptorBit::kMask |
                  Map::Bits1::IsAccessCheckNeededBit::kMask;
  USE(mask);
  // Interceptors or access checks imply special receiver.
  CSA_ASSERT(this,
             SelectConstant<BoolT>(IsSetWord32(LoadMapBitField(map), mask),
                                   is_special, Int32TrueConstant()));
  return is_special;
}

TNode<Word32T> CodeStubAssembler::IsStringWrapperElementsKind(TNode<Map> map) {
  TNode<Int32T> kind = LoadMapElementsKind(map);
  return Word32Or(
      Word32Equal(kind, Int32Constant(FAST_STRING_WRAPPER_ELEMENTS)),
      Word32Equal(kind, Int32Constant(SLOW_STRING_WRAPPER_ELEMENTS)));
}

void CodeStubAssembler::GotoIfMapHasSlowProperties(TNode<Map> map,
                                                   Label* if_slow) {
  GotoIf(IsStringWrapperElementsKind(map), if_slow);
  GotoIf(IsSpecialReceiverMap(map), if_slow);
  GotoIf(IsDictionaryMap(map), if_slow);
}

TNode<HeapObject> CodeStubAssembler::LoadFastProperties(
    TNode<JSReceiver> object) {
  CSA_SLOW_ASSERT(this, Word32BinaryNot(IsDictionaryMap(LoadMap(object))));
  TNode<Object> properties = LoadJSReceiverPropertiesOrHash(object);
  return Select<HeapObject>(
      TaggedIsSmi(properties), [=] { return EmptyFixedArrayConstant(); },
      [=] { return CAST(properties); });
}

TNode<HeapObject> CodeStubAssembler::LoadSlowProperties(
    TNode<JSReceiver> object) {
  CSA_SLOW_ASSERT(this, IsDictionaryMap(LoadMap(object)));
  TNode<Object> properties = LoadJSReceiverPropertiesOrHash(object);
  return Select<HeapObject>(
      TaggedIsSmi(properties),
      [=] { return EmptyPropertyDictionaryConstant(); },
      [=] { return CAST(properties); });
}

TNode<Object> CodeStubAssembler::LoadJSArgumentsObjectLength(
    TNode<Context> context, TNode<JSArgumentsObject> array) {
  CSA_ASSERT(this, IsJSArgumentsObjectWithLength(context, array));
  constexpr int offset = JSStrictArgumentsObject::kLengthOffset;
  STATIC_ASSERT(offset == JSSloppyArgumentsObject::kLengthOffset);
  return LoadObjectField(array, offset);
}

TNode<Smi> CodeStubAssembler::LoadFastJSArrayLength(TNode<JSArray> array) {
  TNode<Number> length = LoadJSArrayLength(array);
  CSA_ASSERT(this, Word32Or(IsFastElementsKind(LoadElementsKind(array)),
                            IsElementsKindInRange(
                                LoadElementsKind(array),
                                FIRST_ANY_NONEXTENSIBLE_ELEMENTS_KIND,
                                LAST_ANY_NONEXTENSIBLE_ELEMENTS_KIND)));
  // JSArray length is always a positive Smi for fast arrays.
  CSA_SLOW_ASSERT(this, TaggedIsPositiveSmi(length));
  return CAST(length);
}

TNode<Smi> CodeStubAssembler::LoadFixedArrayBaseLength(
    TNode<FixedArrayBase> array) {
  CSA_SLOW_ASSERT(this, IsNotWeakFixedArraySubclass(array));
  return LoadObjectField<Smi>(array, FixedArrayBase::kLengthOffset);
}

TNode<IntPtrT> CodeStubAssembler::LoadAndUntagFixedArrayBaseLength(
    TNode<FixedArrayBase> array) {
  return LoadAndUntagObjectField(array, FixedArrayBase::kLengthOffset);
}

TNode<IntPtrT> CodeStubAssembler::LoadFeedbackVectorLength(
    TNode<FeedbackVector> vector) {
  return ChangeInt32ToIntPtr(
      LoadObjectField<Int32T>(vector, FeedbackVector::kLengthOffset));
}

TNode<Smi> CodeStubAssembler::LoadWeakFixedArrayLength(
    TNode<WeakFixedArray> array) {
  return LoadObjectField<Smi>(array, WeakFixedArray::kLengthOffset);
}

TNode<IntPtrT> CodeStubAssembler::LoadAndUntagWeakFixedArrayLength(
    TNode<WeakFixedArray> array) {
  return LoadAndUntagObjectField(array, WeakFixedArray::kLengthOffset);
}

TNode<Int32T> CodeStubAssembler::LoadNumberOfDescriptors(
    TNode<DescriptorArray> array) {
  return UncheckedCast<Int32T>(LoadObjectField<Int16T>(
      array, DescriptorArray::kNumberOfDescriptorsOffset));
}

TNode<Int32T> CodeStubAssembler::LoadNumberOfOwnDescriptors(TNode<Map> map) {
  TNode<Uint32T> bit_field3 = LoadMapBitField3(map);
  return UncheckedCast<Int32T>(
      DecodeWord32<Map::Bits3::NumberOfOwnDescriptorsBits>(bit_field3));
}

TNode<Int32T> CodeStubAssembler::LoadMapBitField(TNode<Map> map) {
  return UncheckedCast<Int32T>(
      LoadObjectField<Uint8T>(map, Map::kBitFieldOffset));
}

TNode<Int32T> CodeStubAssembler::LoadMapBitField2(TNode<Map> map) {
  return UncheckedCast<Int32T>(
      LoadObjectField<Uint8T>(map, Map::kBitField2Offset));
}

TNode<Uint32T> CodeStubAssembler::LoadMapBitField3(TNode<Map> map) {
  return LoadObjectField<Uint32T>(map, Map::kBitField3Offset);
}

TNode<Uint16T> CodeStubAssembler::LoadMapInstanceType(TNode<Map> map) {
  return LoadObjectField<Uint16T>(map, Map::kInstanceTypeOffset);
}

TNode<Int32T> CodeStubAssembler::LoadMapElementsKind(TNode<Map> map) {
  TNode<Int32T> bit_field2 = LoadMapBitField2(map);
  return Signed(DecodeWord32<Map::Bits2::ElementsKindBits>(bit_field2));
}

TNode<Int32T> CodeStubAssembler::LoadElementsKind(TNode<HeapObject> object) {
  return LoadMapElementsKind(LoadMap(object));
}

TNode<DescriptorArray> CodeStubAssembler::LoadMapDescriptors(TNode<Map> map) {
  return LoadObjectField<DescriptorArray>(map, Map::kInstanceDescriptorsOffset);
}

TNode<HeapObject> CodeStubAssembler::LoadMapPrototype(TNode<Map> map) {
  return LoadObjectField<HeapObject>(map, Map::kPrototypeOffset);
}

TNode<IntPtrT> CodeStubAssembler::LoadMapInstanceSizeInWords(TNode<Map> map) {
  return ChangeInt32ToIntPtr(
      LoadObjectField<Uint8T>(map, Map::kInstanceSizeInWordsOffset));
}

TNode<IntPtrT> CodeStubAssembler::LoadMapInobjectPropertiesStartInWords(
    TNode<Map> map) {
  // See Map::GetInObjectPropertiesStartInWords() for details.
  CSA_ASSERT(this, IsJSObjectMap(map));
  return ChangeInt32ToIntPtr(LoadObjectField<Uint8T>(
      map, Map::kInObjectPropertiesStartOrConstructorFunctionIndexOffset));
}

TNode<IntPtrT> CodeStubAssembler::LoadMapConstructorFunctionIndex(
    TNode<Map> map) {
  // See Map::GetConstructorFunctionIndex() for details.
  CSA_ASSERT(this, IsPrimitiveInstanceType(LoadMapInstanceType(map)));
  return ChangeInt32ToIntPtr(LoadObjectField<Uint8T>(
      map, Map::kInObjectPropertiesStartOrConstructorFunctionIndexOffset));
}

TNode<Object> CodeStubAssembler::LoadMapConstructor(TNode<Map> map) {
  TVARIABLE(Object, result,
            LoadObjectField(
                map, Map::kConstructorOrBackPointerOrNativeContextOffset));

  Label done(this), loop(this, &result);
  Goto(&loop);
  BIND(&loop);
  {
    GotoIf(TaggedIsSmi(result.value()), &done);
    TNode<BoolT> is_map_type =
        InstanceTypeEqual(LoadInstanceType(CAST(result.value())), MAP_TYPE);
    GotoIfNot(is_map_type, &done);
    result =
        LoadObjectField(CAST(result.value()),
                        Map::kConstructorOrBackPointerOrNativeContextOffset);
    Goto(&loop);
  }
  BIND(&done);
  return result.value();
}

TNode<WordT> CodeStubAssembler::LoadMapEnumLength(TNode<Map> map) {
  TNode<Uint32T> bit_field3 = LoadMapBitField3(map);
  return DecodeWordFromWord32<Map::Bits3::EnumLengthBits>(bit_field3);
}

TNode<Object> CodeStubAssembler::LoadMapBackPointer(TNode<Map> map) {
  TNode<HeapObject> object = CAST(LoadObjectField(
      map, Map::kConstructorOrBackPointerOrNativeContextOffset));
  return Select<Object>(
      IsMap(object), [=] { return object; },
      [=] { return UndefinedConstant(); });
}

TNode<Uint32T> CodeStubAssembler::EnsureOnlyHasSimpleProperties(
    TNode<Map> map, TNode<Int32T> instance_type, Label* bailout) {
  // This check can have false positives, since it applies to any
  // JSPrimitiveWrapper type.
  GotoIf(IsCustomElementsReceiverInstanceType(instance_type), bailout);

  TNode<Uint32T> bit_field3 = LoadMapBitField3(map);
  GotoIf(IsSetWord32(bit_field3, Map::Bits3::IsDictionaryMapBit::kMask),
         bailout);

  return bit_field3;
}

TNode<IntPtrT> CodeStubAssembler::LoadJSReceiverIdentityHash(
    SloppyTNode<Object> receiver, Label* if_no_hash) {
  TVARIABLE(IntPtrT, var_hash);
  Label done(this), if_smi(this), if_property_array(this),
      if_property_dictionary(this), if_fixed_array(this);

  TNode<Object> properties_or_hash =
      LoadObjectField(TNode<HeapObject>::UncheckedCast(receiver),
                      JSReceiver::kPropertiesOrHashOffset);
  GotoIf(TaggedIsSmi(properties_or_hash), &if_smi);

  TNode<HeapObject> properties =
      TNode<HeapObject>::UncheckedCast(properties_or_hash);
  TNode<Uint16T> properties_instance_type = LoadInstanceType(properties);

  GotoIf(InstanceTypeEqual(properties_instance_type, PROPERTY_ARRAY_TYPE),
         &if_property_array);
  Branch(InstanceTypeEqual(properties_instance_type, NAME_DICTIONARY_TYPE),
         &if_property_dictionary, &if_fixed_array);

  BIND(&if_fixed_array);
  {
    var_hash = IntPtrConstant(PropertyArray::kNoHashSentinel);
    Goto(&done);
  }

  BIND(&if_smi);
  {
    var_hash = SmiUntag(TNode<Smi>::UncheckedCast(properties_or_hash));
    Goto(&done);
  }

  BIND(&if_property_array);
  {
    TNode<IntPtrT> length_and_hash = LoadAndUntagObjectField(
        properties, PropertyArray::kLengthAndHashOffset);
    var_hash = TNode<IntPtrT>::UncheckedCast(
        DecodeWord<PropertyArray::HashField>(length_and_hash));
    Goto(&done);
  }

  BIND(&if_property_dictionary);
  {
    var_hash = SmiUntag(CAST(LoadFixedArrayElement(
        CAST(properties), NameDictionary::kObjectHashIndex)));
    Goto(&done);
  }

  BIND(&done);
  if (if_no_hash != nullptr) {
    GotoIf(IntPtrEqual(var_hash.value(),
                       IntPtrConstant(PropertyArray::kNoHashSentinel)),
           if_no_hash);
  }
  return var_hash.value();
}

TNode<Uint32T> CodeStubAssembler::LoadNameHashAssumeComputed(TNode<Name> name) {
  TNode<Uint32T> hash_field = LoadNameHashField(name);
  CSA_ASSERT(this, IsClearWord32(hash_field, Name::kHashNotComputedMask));
  return Unsigned(Word32Shr(hash_field, Int32Constant(Name::kHashShift)));
}

TNode<Uint32T> CodeStubAssembler::LoadNameHash(TNode<Name> name,
                                               Label* if_hash_not_computed) {
  TNode<Uint32T> hash_field = LoadNameHashField(name);
  if (if_hash_not_computed != nullptr) {
    GotoIf(IsSetWord32(hash_field, Name::kHashNotComputedMask),
           if_hash_not_computed);
  }
  return Unsigned(Word32Shr(hash_field, Int32Constant(Name::kHashShift)));
}

TNode<Smi> CodeStubAssembler::LoadStringLengthAsSmi(TNode<String> string) {
  return SmiFromIntPtr(LoadStringLengthAsWord(string));
}

TNode<IntPtrT> CodeStubAssembler::LoadStringLengthAsWord(TNode<String> string) {
  return Signed(ChangeUint32ToWord(LoadStringLengthAsWord32(string)));
}

TNode<Uint32T> CodeStubAssembler::LoadStringLengthAsWord32(
    TNode<String> string) {
  return LoadObjectField<Uint32T>(string, String::kLengthOffset);
}

TNode<Object> CodeStubAssembler::LoadJSPrimitiveWrapperValue(
    TNode<JSPrimitiveWrapper> object) {
  return LoadObjectField(object, JSPrimitiveWrapper::kValueOffset);
}

void CodeStubAssembler::DispatchMaybeObject(TNode<MaybeObject> maybe_object,
                                            Label* if_smi, Label* if_cleared,
                                            Label* if_weak, Label* if_strong,
                                            TVariable<Object>* extracted) {
  Label inner_if_smi(this), inner_if_strong(this);

  GotoIf(TaggedIsSmi(maybe_object), &inner_if_smi);

  GotoIf(IsCleared(maybe_object), if_cleared);

  GotoIf(IsStrong(maybe_object), &inner_if_strong);

  *extracted = GetHeapObjectAssumeWeak(maybe_object);
  Goto(if_weak);

  BIND(&inner_if_smi);
  *extracted = CAST(maybe_object);
  Goto(if_smi);

  BIND(&inner_if_strong);
  *extracted = CAST(maybe_object);
  Goto(if_strong);
}

TNode<BoolT> CodeStubAssembler::IsStrong(TNode<MaybeObject> value) {
  return Word32Equal(Word32And(TruncateIntPtrToInt32(
                                   BitcastTaggedToWordForTagAndSmiBits(value)),
                               Int32Constant(kHeapObjectTagMask)),
                     Int32Constant(kHeapObjectTag));
}

TNode<HeapObject> CodeStubAssembler::GetHeapObjectIfStrong(
    TNode<MaybeObject> value, Label* if_not_strong) {
  GotoIfNot(IsStrong(value), if_not_strong);
  return CAST(value);
}

TNode<BoolT> CodeStubAssembler::IsWeakOrCleared(TNode<MaybeObject> value) {
  return Word32Equal(Word32And(TruncateIntPtrToInt32(
                                   BitcastTaggedToWordForTagAndSmiBits(value)),
                               Int32Constant(kHeapObjectTagMask)),
                     Int32Constant(kWeakHeapObjectTag));
}

TNode<BoolT> CodeStubAssembler::IsCleared(TNode<MaybeObject> value) {
  return Word32Equal(TruncateIntPtrToInt32(BitcastMaybeObjectToWord(value)),
                     Int32Constant(kClearedWeakHeapObjectLower32));
}

TNode<HeapObject> CodeStubAssembler::GetHeapObjectAssumeWeak(
    TNode<MaybeObject> value) {
  CSA_ASSERT(this, IsWeakOrCleared(value));
  CSA_ASSERT(this, IsNotCleared(value));
  return UncheckedCast<HeapObject>(BitcastWordToTagged(WordAnd(
      BitcastMaybeObjectToWord(value), IntPtrConstant(~kWeakHeapObjectMask))));
}

TNode<HeapObject> CodeStubAssembler::GetHeapObjectAssumeWeak(
    TNode<MaybeObject> value, Label* if_cleared) {
  GotoIf(IsCleared(value), if_cleared);
  return GetHeapObjectAssumeWeak(value);
}

// This version generates
//   (maybe_object & ~mask) == value
// It works for non-Smi |maybe_object| and for both Smi and HeapObject values
// but requires a big constant for ~mask.
TNode<BoolT> CodeStubAssembler::IsWeakReferenceToObject(
    TNode<MaybeObject> maybe_object, TNode<Object> value) {
  CSA_ASSERT(this, TaggedIsNotSmi(maybe_object));
  if (COMPRESS_POINTERS_BOOL) {
    return Word32Equal(
        Word32And(TruncateWordToInt32(BitcastMaybeObjectToWord(maybe_object)),
                  Uint32Constant(~static_cast<uint32_t>(kWeakHeapObjectMask))),
        TruncateWordToInt32(BitcastTaggedToWord(value)));
  } else {
    return WordEqual(WordAnd(BitcastMaybeObjectToWord(maybe_object),
                             IntPtrConstant(~kWeakHeapObjectMask)),
                     BitcastTaggedToWord(value));
  }
}

// This version generates
//   maybe_object == (heap_object | mask)
// It works for any |maybe_object| values and generates a better code because it
// uses a small constant for mask.
TNode<BoolT> CodeStubAssembler::IsWeakReferenceTo(
    TNode<MaybeObject> maybe_object, TNode<HeapObject> heap_object) {
  if (COMPRESS_POINTERS_BOOL) {
    return Word32Equal(
        TruncateWordToInt32(BitcastMaybeObjectToWord(maybe_object)),
        Word32Or(TruncateWordToInt32(BitcastTaggedToWord(heap_object)),
                 Int32Constant(kWeakHeapObjectMask)));
  } else {
    return WordEqual(BitcastMaybeObjectToWord(maybe_object),
                     WordOr(BitcastTaggedToWord(heap_object),
                            IntPtrConstant(kWeakHeapObjectMask)));
  }
}

TNode<MaybeObject> CodeStubAssembler::MakeWeak(TNode<HeapObject> value) {
  return ReinterpretCast<MaybeObject>(BitcastWordToTagged(
      WordOr(BitcastTaggedToWord(value), IntPtrConstant(kWeakHeapObjectTag))));
}

template <>
TNode<IntPtrT> CodeStubAssembler::LoadArrayLength(TNode<FixedArray> array) {
  return LoadAndUntagFixedArrayBaseLength(array);
}

template <>
TNode<IntPtrT> CodeStubAssembler::LoadArrayLength(TNode<WeakFixedArray> array) {
  return LoadAndUntagWeakFixedArrayLength(array);
}

template <>
TNode<IntPtrT> CodeStubAssembler::LoadArrayLength(TNode<PropertyArray> array) {
  return LoadPropertyArrayLength(array);
}

template <>
TNode<IntPtrT> CodeStubAssembler::LoadArrayLength(
    TNode<DescriptorArray> array) {
  return IntPtrMul(ChangeInt32ToIntPtr(LoadNumberOfDescriptors(array)),
                   IntPtrConstant(DescriptorArray::kEntrySize));
}

template <>
TNode<IntPtrT> CodeStubAssembler::LoadArrayLength(
    TNode<TransitionArray> array) {
  return LoadAndUntagWeakFixedArrayLength(array);
}

template <typename Array, typename TIndex, typename TValue>
TNode<TValue> CodeStubAssembler::LoadArrayElement(
    TNode<Array> array, int array_header_size, TNode<TIndex> index_node,
    int additional_offset, LoadSensitivity needs_poisoning) {
  // TODO(v8:9708): Do we want to keep both IntPtrT and UintPtrT variants?
  static_assert(std::is_same<TIndex, Smi>::value ||
                    std::is_same<TIndex, UintPtrT>::value ||
                    std::is_same<TIndex, IntPtrT>::value,
                "Only Smi, UintPtrT or IntPtrT indices are allowed");
  CSA_ASSERT(this, IntPtrGreaterThanOrEqual(ParameterToIntPtr(index_node),
                                            IntPtrConstant(0)));
  DCHECK(IsAligned(additional_offset, kTaggedSize));
  int32_t header_size = array_header_size + additional_offset - kHeapObjectTag;
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(index_node, HOLEY_ELEMENTS, header_size);
  CSA_ASSERT(this, IsOffsetInBounds(offset, LoadArrayLength(array),
                                    array_header_size));
  constexpr MachineType machine_type = MachineTypeOf<TValue>::value;
  // TODO(gsps): Remove the Load case once LoadFromObject supports poisoning
  if (needs_poisoning == LoadSensitivity::kSafe) {
    return UncheckedCast<TValue>(LoadFromObject(machine_type, array, offset));
  } else {
    return UncheckedCast<TValue>(
        Load(machine_type, array, offset, needs_poisoning));
  }
}

template V8_EXPORT_PRIVATE TNode<MaybeObject>
CodeStubAssembler::LoadArrayElement<TransitionArray, IntPtrT>(
    TNode<TransitionArray>, int, TNode<IntPtrT>, int, LoadSensitivity);

template <typename TIndex>
TNode<Object> CodeStubAssembler::LoadFixedArrayElement(
    TNode<FixedArray> object, TNode<TIndex> index, int additional_offset,
    LoadSensitivity needs_poisoning, CheckBounds check_bounds) {
  // TODO(v8:9708): Do we want to keep both IntPtrT and UintPtrT variants?
  static_assert(std::is_same<TIndex, Smi>::value ||
                    std::is_same<TIndex, UintPtrT>::value ||
                    std::is_same<TIndex, IntPtrT>::value,
                "Only Smi, UintPtrT or IntPtrT indexes are allowed");
  CSA_ASSERT(this, IsFixedArraySubclass(object));
  CSA_ASSERT(this, IsNotWeakFixedArraySubclass(object));

  if (NeedsBoundsCheck(check_bounds)) {
    FixedArrayBoundsCheck(object, index, additional_offset);
  }
  TNode<MaybeObject> element =
      LoadArrayElement(object, FixedArray::kHeaderSize, index,
                       additional_offset, needs_poisoning);
  return CAST(element);
}

template V8_EXPORT_PRIVATE TNode<Object>
CodeStubAssembler::LoadFixedArrayElement<Smi>(TNode<FixedArray>, TNode<Smi>,
                                              int, LoadSensitivity,
                                              CheckBounds);
template V8_EXPORT_PRIVATE TNode<Object>
CodeStubAssembler::LoadFixedArrayElement<UintPtrT>(TNode<FixedArray>,
                                                   TNode<UintPtrT>, int,
                                                   LoadSensitivity,
                                                   CheckBounds);
template V8_EXPORT_PRIVATE TNode<Object>
CodeStubAssembler::LoadFixedArrayElement<IntPtrT>(TNode<FixedArray>,
                                                  TNode<IntPtrT>, int,
                                                  LoadSensitivity, CheckBounds);

void CodeStubAssembler::FixedArrayBoundsCheck(TNode<FixedArrayBase> array,
                                              TNode<Smi> index,
                                              int additional_offset) {
  if (!FLAG_fixed_array_bounds_checks) return;
  DCHECK(IsAligned(additional_offset, kTaggedSize));
  TNode<Smi> effective_index;
  Smi constant_index;
  bool index_is_constant = ToSmiConstant(index, &constant_index);
  if (index_is_constant) {
    effective_index = SmiConstant(Smi::ToInt(constant_index) +
                                  additional_offset / kTaggedSize);
  } else {
    effective_index =
        SmiAdd(index, SmiConstant(additional_offset / kTaggedSize));
  }
  CSA_CHECK(this, SmiBelow(effective_index, LoadFixedArrayBaseLength(array)));
}

void CodeStubAssembler::FixedArrayBoundsCheck(TNode<FixedArrayBase> array,
                                              TNode<IntPtrT> index,
                                              int additional_offset) {
  if (!FLAG_fixed_array_bounds_checks) return;
  DCHECK(IsAligned(additional_offset, kTaggedSize));
  // IntPtrAdd does constant-folding automatically.
  TNode<IntPtrT> effective_index =
      IntPtrAdd(index, IntPtrConstant(additional_offset / kTaggedSize));
  CSA_CHECK(this, UintPtrLessThan(effective_index,
                                  LoadAndUntagFixedArrayBaseLength(array)));
}

TNode<Object> CodeStubAssembler::LoadPropertyArrayElement(
    TNode<PropertyArray> object, SloppyTNode<IntPtrT> index) {
  int additional_offset = 0;
  LoadSensitivity needs_poisoning = LoadSensitivity::kSafe;
  return CAST(LoadArrayElement(object, PropertyArray::kHeaderSize, index,
                               additional_offset, needs_poisoning));
}

TNode<IntPtrT> CodeStubAssembler::LoadPropertyArrayLength(
    TNode<PropertyArray> object) {
  TNode<IntPtrT> value =
      LoadAndUntagObjectField(object, PropertyArray::kLengthAndHashOffset);
  return Signed(DecodeWord<PropertyArray::LengthField>(value));
}

TNode<RawPtrT> CodeStubAssembler::LoadJSTypedArrayDataPtr(
    TNode<JSTypedArray> typed_array) {
  // Data pointer = external_pointer + static_cast<Tagged_t>(base_pointer).
  TNode<RawPtrT> external_pointer =
      LoadJSTypedArrayExternalPointerPtr(typed_array);

  TNode<IntPtrT> base_pointer;
  if (COMPRESS_POINTERS_BOOL) {
    TNode<Int32T> compressed_base =
        LoadObjectField<Int32T>(typed_array, JSTypedArray::kBasePointerOffset);
    // Zero-extend TaggedT to WordT according to current compression scheme
    // so that the addition with |external_pointer| (which already contains
    // compensated offset value) below will decompress the tagged value.
    // See JSTypedArray::ExternalPointerCompensationForOnHeapArray() for
    // details.
    base_pointer = Signed(ChangeUint32ToWord(compressed_base));
  } else {
    base_pointer =
        LoadObjectField<IntPtrT>(typed_array, JSTypedArray::kBasePointerOffset);
  }
  return RawPtrAdd(external_pointer, base_pointer);
}

TNode<BigInt> CodeStubAssembler::LoadFixedBigInt64ArrayElementAsTagged(
    SloppyTNode<RawPtrT> data_pointer, SloppyTNode<IntPtrT> offset) {
  if (Is64()) {
    TNode<IntPtrT> value = Load<IntPtrT>(data_pointer, offset);
    return BigIntFromInt64(value);
  } else {
    DCHECK(!Is64());
#if defined(V8_TARGET_BIG_ENDIAN)
    TNode<IntPtrT> high = Load<IntPtrT>(data_pointer, offset);
    TNode<IntPtrT> low = Load<IntPtrT>(
        data_pointer, IntPtrAdd(offset, IntPtrConstant(kSystemPointerSize)));
#else
    TNode<IntPtrT> low = Load<IntPtrT>(data_pointer, offset);
    TNode<IntPtrT> high = Load<IntPtrT>(
        data_pointer, IntPtrAdd(offset, IntPtrConstant(kSystemPointerSize)));
#endif
    return BigIntFromInt32Pair(low, high);
  }
}

TNode<BigInt> CodeStubAssembler::BigIntFromInt32Pair(TNode<IntPtrT> low,
                                                     TNode<IntPtrT> high) {
  DCHECK(!Is64());
  TVARIABLE(BigInt, var_result);
  TVARIABLE(Word32T, var_sign, Int32Constant(BigInt::SignBits::encode(false)));
  TVARIABLE(IntPtrT, var_high, high);
  TVARIABLE(IntPtrT, var_low, low);
  Label high_zero(this), negative(this), allocate_one_digit(this),
      allocate_two_digits(this), if_zero(this), done(this);

  GotoIf(IntPtrEqual(var_high.value(), IntPtrConstant(0)), &high_zero);
  Branch(IntPtrLessThan(var_high.value(), IntPtrConstant(0)), &negative,
         &allocate_two_digits);

  BIND(&high_zero);
  Branch(IntPtrEqual(var_low.value(), IntPtrConstant(0)), &if_zero,
         &allocate_one_digit);

  BIND(&negative);
  {
    var_sign = Int32Constant(BigInt::SignBits::encode(true));
    // We must negate the value by computing "0 - (high|low)", performing
    // both parts of the subtraction separately and manually taking care
    // of the carry bit (which is 1 iff low != 0).
    var_high = IntPtrSub(IntPtrConstant(0), var_high.value());
    Label carry(this), no_carry(this);
    Branch(IntPtrEqual(var_low.value(), IntPtrConstant(0)), &no_carry, &carry);
    BIND(&carry);
    var_high = IntPtrSub(var_high.value(), IntPtrConstant(1));
    Goto(&no_carry);
    BIND(&no_carry);
    var_low = IntPtrSub(IntPtrConstant(0), var_low.value());
    // var_high was non-zero going into this block, but subtracting the
    // carry bit from it could bring us back onto the "one digit" path.
    Branch(IntPtrEqual(var_high.value(), IntPtrConstant(0)),
           &allocate_one_digit, &allocate_two_digits);
  }

  BIND(&allocate_one_digit);
  {
    var_result = AllocateRawBigInt(IntPtrConstant(1));
    StoreBigIntBitfield(var_result.value(),
                        Word32Or(var_sign.value(),
                                 Int32Constant(BigInt::LengthBits::encode(1))));
    StoreBigIntDigit(var_result.value(), 0, Unsigned(var_low.value()));
    Goto(&done);
  }

  BIND(&allocate_two_digits);
  {
    var_result = AllocateRawBigInt(IntPtrConstant(2));
    StoreBigIntBitfield(var_result.value(),
                        Word32Or(var_sign.value(),
                                 Int32Constant(BigInt::LengthBits::encode(2))));
    StoreBigIntDigit(var_result.value(), 0, Unsigned(var_low.value()));
    StoreBigIntDigit(var_result.value(), 1, Unsigned(var_high.value()));
    Goto(&done);
  }

  BIND(&if_zero);
  var_result = AllocateBigInt(IntPtrConstant(0));
  Goto(&done);

  BIND(&done);
  return var_result.value();
}

TNode<BigInt> CodeStubAssembler::BigIntFromInt64(TNode<IntPtrT> value) {
  DCHECK(Is64());
  TVARIABLE(BigInt, var_result);
  Label done(this), if_positive(this), if_negative(this), if_zero(this);
  GotoIf(IntPtrEqual(value, IntPtrConstant(0)), &if_zero);
  var_result = AllocateRawBigInt(IntPtrConstant(1));
  Branch(IntPtrGreaterThan(value, IntPtrConstant(0)), &if_positive,
         &if_negative);

  BIND(&if_positive);
  {
    StoreBigIntBitfield(var_result.value(),
                        Int32Constant(BigInt::SignBits::encode(false) |
                                      BigInt::LengthBits::encode(1)));
    StoreBigIntDigit(var_result.value(), 0, Unsigned(value));
    Goto(&done);
  }

  BIND(&if_negative);
  {
    StoreBigIntBitfield(var_result.value(),
                        Int32Constant(BigInt::SignBits::encode(true) |
                                      BigInt::LengthBits::encode(1)));
    StoreBigIntDigit(var_result.value(), 0,
                     Unsigned(IntPtrSub(IntPtrConstant(0), value)));
    Goto(&done);
  }

  BIND(&if_zero);
  {
    var_result = AllocateBigInt(IntPtrConstant(0));
    Goto(&done);
  }

  BIND(&done);
  return var_result.value();
}

TNode<BigInt> CodeStubAssembler::LoadFixedBigUint64ArrayElementAsTagged(
    SloppyTNode<RawPtrT> data_pointer, SloppyTNode<IntPtrT> offset) {
  Label if_zero(this), done(this);
  if (Is64()) {
    TNode<UintPtrT> value = Load<UintPtrT>(data_pointer, offset);
    return BigIntFromUint64(value);
  } else {
    DCHECK(!Is64());
#if defined(V8_TARGET_BIG_ENDIAN)
    TNode<UintPtrT> high = Load<UintPtrT>(data_pointer, offset);
    TNode<UintPtrT> low = Load<UintPtrT>(
        data_pointer, IntPtrAdd(offset, IntPtrConstant(kSystemPointerSize)));
#else
    TNode<UintPtrT> low = Load<UintPtrT>(data_pointer, offset);
    TNode<UintPtrT> high = Load<UintPtrT>(
        data_pointer, IntPtrAdd(offset, IntPtrConstant(kSystemPointerSize)));
#endif
    return BigIntFromUint32Pair(low, high);
  }
}

TNode<BigInt> CodeStubAssembler::BigIntFromUint32Pair(TNode<UintPtrT> low,
                                                      TNode<UintPtrT> high) {
  DCHECK(!Is64());
  TVARIABLE(BigInt, var_result);
  Label high_zero(this), if_zero(this), done(this);

  GotoIf(IntPtrEqual(high, IntPtrConstant(0)), &high_zero);
  var_result = AllocateBigInt(IntPtrConstant(2));
  StoreBigIntDigit(var_result.value(), 0, low);
  StoreBigIntDigit(var_result.value(), 1, high);
  Goto(&done);

  BIND(&high_zero);
  GotoIf(IntPtrEqual(low, IntPtrConstant(0)), &if_zero);
  var_result = AllocateBigInt(IntPtrConstant(1));
  StoreBigIntDigit(var_result.value(), 0, low);
  Goto(&done);

  BIND(&if_zero);
  var_result = AllocateBigInt(IntPtrConstant(0));
  Goto(&done);

  BIND(&done);
  return var_result.value();
}

TNode<BigInt> CodeStubAssembler::BigIntFromUint64(TNode<UintPtrT> value) {
  DCHECK(Is64());
  TVARIABLE(BigInt, var_result);
  Label done(this), if_zero(this);
  GotoIf(IntPtrEqual(value, IntPtrConstant(0)), &if_zero);
  var_result = AllocateBigInt(IntPtrConstant(1));
  StoreBigIntDigit(var_result.value(), 0, value);
  Goto(&done);

  BIND(&if_zero);
  var_result = AllocateBigInt(IntPtrConstant(0));
  Goto(&done);
  BIND(&done);
  return var_result.value();
}

TNode<Numeric> CodeStubAssembler::LoadFixedTypedArrayElementAsTagged(
    TNode<RawPtrT> data_pointer, TNode<UintPtrT> index,
    ElementsKind elements_kind) {
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(Signed(index), elements_kind, 0);
  switch (elements_kind) {
    case UINT8_ELEMENTS: /* fall through */
    case UINT8_CLAMPED_ELEMENTS:
      return SmiFromInt32(Load<Uint8T>(data_pointer, offset));
    case INT8_ELEMENTS:
      return SmiFromInt32(Load<Int8T>(data_pointer, offset));
    case UINT16_ELEMENTS:
      return SmiFromInt32(Load<Uint16T>(data_pointer, offset));
    case INT16_ELEMENTS:
      return SmiFromInt32(Load<Int16T>(data_pointer, offset));
    case UINT32_ELEMENTS:
      return ChangeUint32ToTagged(Load<Uint32T>(data_pointer, offset));
    case INT32_ELEMENTS:
      return ChangeInt32ToTagged(Load<Int32T>(data_pointer, offset));
    case FLOAT32_ELEMENTS:
      return AllocateHeapNumberWithValue(
          ChangeFloat32ToFloat64(Load<Float32T>(data_pointer, offset)));
    case FLOAT64_ELEMENTS:
      return AllocateHeapNumberWithValue(Load<Float64T>(data_pointer, offset));
    case BIGINT64_ELEMENTS:
      return LoadFixedBigInt64ArrayElementAsTagged(data_pointer, offset);
    case BIGUINT64_ELEMENTS:
      return LoadFixedBigUint64ArrayElementAsTagged(data_pointer, offset);
    default:
      UNREACHABLE();
  }
}

TNode<Numeric> CodeStubAssembler::LoadFixedTypedArrayElementAsTagged(
    TNode<RawPtrT> data_pointer, TNode<UintPtrT> index,
    TNode<Int32T> elements_kind) {
  TVARIABLE(Numeric, var_result);
  Label done(this), if_unknown_type(this, Label::kDeferred);
  int32_t elements_kinds[] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype) TYPE##_ELEMENTS,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype) Label if_##type##array(this);
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

  Label* elements_kind_labels[] = {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype) &if_##type##array,
      TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE
  };
  STATIC_ASSERT(arraysize(elements_kinds) == arraysize(elements_kind_labels));

  Switch(elements_kind, &if_unknown_type, elements_kinds, elements_kind_labels,
         arraysize(elements_kinds));

  BIND(&if_unknown_type);
  Unreachable();

#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype)                        \
  BIND(&if_##type##array);                                               \
  {                                                                      \
    var_result = LoadFixedTypedArrayElementAsTagged(data_pointer, index, \
                                                    TYPE##_ELEMENTS);    \
    Goto(&done);                                                         \
  }
  TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

  BIND(&done);
  return var_result.value();
}

template <typename TIndex>
TNode<MaybeObject> CodeStubAssembler::LoadFeedbackVectorSlot(
    TNode<FeedbackVector> feedback_vector, TNode<TIndex> slot,
    int additional_offset) {
  int32_t header_size = FeedbackVector::kRawFeedbackSlotsOffset +
                        additional_offset - kHeapObjectTag;
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(slot, HOLEY_ELEMENTS, header_size);
  CSA_SLOW_ASSERT(
      this, IsOffsetInBounds(offset, LoadFeedbackVectorLength(feedback_vector),
                             FeedbackVector::kHeaderSize));
  return Load<MaybeObject>(feedback_vector, offset);
}

template TNode<MaybeObject> CodeStubAssembler::LoadFeedbackVectorSlot(
    TNode<FeedbackVector> feedback_vector, TNode<TaggedIndex> slot,
    int additional_offset);
template TNode<MaybeObject> CodeStubAssembler::LoadFeedbackVectorSlot(
    TNode<FeedbackVector> feedback_vector, TNode<IntPtrT> slot,
    int additional_offset);
template TNode<MaybeObject> CodeStubAssembler::LoadFeedbackVectorSlot(
    TNode<FeedbackVector> feedback_vector, TNode<UintPtrT> slot,
    int additional_offset);

template <typename Array>
TNode<Int32T> CodeStubAssembler::LoadAndUntagToWord32ArrayElement(
    TNode<Array> object, int array_header_size, TNode<IntPtrT> index,
    int additional_offset) {
  DCHECK(IsAligned(additional_offset, kTaggedSize));
  int endian_correction = 0;
#if V8_TARGET_LITTLE_ENDIAN
  if (SmiValuesAre32Bits()) endian_correction = 4;
#endif
  int32_t header_size = array_header_size + additional_offset - kHeapObjectTag +
                        endian_correction;
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(index, HOLEY_ELEMENTS, header_size);
  CSA_ASSERT(this, IsOffsetInBounds(offset, LoadArrayLength(object),
                                    array_header_size + endian_correction));
  if (SmiValuesAre32Bits()) {
    return Load<Int32T>(object, offset);
  } else {
    return SmiToInt32(Load(MachineType::TaggedSigned(), object, offset));
  }
}

TNode<Int32T> CodeStubAssembler::LoadAndUntagToWord32FixedArrayElement(
    TNode<FixedArray> object, TNode<IntPtrT> index, int additional_offset) {
  CSA_SLOW_ASSERT(this, IsFixedArraySubclass(object));
  return LoadAndUntagToWord32ArrayElement(object, FixedArray::kHeaderSize,
                                          index, additional_offset);
}

TNode<MaybeObject> CodeStubAssembler::LoadWeakFixedArrayElement(
    TNode<WeakFixedArray> object, TNode<IntPtrT> index, int additional_offset) {
  return LoadArrayElement(object, WeakFixedArray::kHeaderSize, index,
                          additional_offset, LoadSensitivity::kSafe);
}

TNode<Float64T> CodeStubAssembler::LoadFixedDoubleArrayElement(
    TNode<FixedDoubleArray> object, TNode<IntPtrT> index, Label* if_hole,
    MachineType machine_type) {
  int32_t header_size = FixedDoubleArray::kHeaderSize - kHeapObjectTag;
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(index, HOLEY_DOUBLE_ELEMENTS, header_size);
  CSA_ASSERT(this, IsOffsetInBounds(
                       offset, LoadAndUntagFixedArrayBaseLength(object),
                       FixedDoubleArray::kHeaderSize, HOLEY_DOUBLE_ELEMENTS));
  return LoadDoubleWithHoleCheck(object, offset, if_hole, machine_type);
}

TNode<Object> CodeStubAssembler::LoadFixedArrayBaseElementAsTagged(
    TNode<FixedArrayBase> elements, TNode<IntPtrT> index,
    TNode<Int32T> elements_kind, Label* if_accessor, Label* if_hole) {
  TVARIABLE(Object, var_result);
  Label done(this), if_packed(this), if_holey(this), if_packed_double(this),
      if_holey_double(this), if_dictionary(this, Label::kDeferred);

  int32_t kinds[] = {
      // Handled by if_packed.
      PACKED_SMI_ELEMENTS, PACKED_ELEMENTS, PACKED_NONEXTENSIBLE_ELEMENTS,
      PACKED_SEALED_ELEMENTS, PACKED_FROZEN_ELEMENTS,
      // Handled by if_holey.
      HOLEY_SMI_ELEMENTS, HOLEY_ELEMENTS, HOLEY_NONEXTENSIBLE_ELEMENTS,
      HOLEY_SEALED_ELEMENTS, HOLEY_FROZEN_ELEMENTS,
      // Handled by if_packed_double.
      PACKED_DOUBLE_ELEMENTS,
      // Handled by if_holey_double.
      HOLEY_DOUBLE_ELEMENTS};
  Label* labels[] = {// PACKED_{SMI,}_ELEMENTS
                     &if_packed, &if_packed, &if_packed, &if_packed, &if_packed,
                     // HOLEY_{SMI,}_ELEMENTS
                     &if_holey, &if_holey, &if_holey, &if_holey, &if_holey,
                     // PACKED_DOUBLE_ELEMENTS
                     &if_packed_double,
                     // HOLEY_DOUBLE_ELEMENTS
                     &if_holey_double};
  Switch(elements_kind, &if_dictionary, kinds, labels, arraysize(kinds));

  BIND(&if_packed);
  {
    var_result = LoadFixedArrayElement(CAST(elements), index, 0);
    Goto(&done);
  }

  BIND(&if_holey);
  {
    var_result = LoadFixedArrayElement(CAST(elements), index);
    Branch(TaggedEqual(var_result.value(), TheHoleConstant()), if_hole, &done);
  }

  BIND(&if_packed_double);
  {
    var_result = AllocateHeapNumberWithValue(
        LoadFixedDoubleArrayElement(CAST(elements), index));
    Goto(&done);
  }

  BIND(&if_holey_double);
  {
    var_result = AllocateHeapNumberWithValue(
        LoadFixedDoubleArrayElement(CAST(elements), index, if_hole));
    Goto(&done);
  }

  BIND(&if_dictionary);
  {
    CSA_ASSERT(this, IsDictionaryElementsKind(elements_kind));
    var_result = BasicLoadNumberDictionaryElement(CAST(elements), index,
                                                  if_accessor, if_hole);
    Goto(&done);
  }

  BIND(&done);
  return var_result.value();
}

TNode<BoolT> CodeStubAssembler::IsDoubleHole(TNode<Object> base,
                                             TNode<IntPtrT> offset) {
  // TODO(ishell): Compare only the upper part for the hole once the
  // compiler is able to fold addition of already complex |offset| with
  // |kIeeeDoubleExponentWordOffset| into one addressing mode.
  if (Is64()) {
    TNode<Uint64T> element = Load<Uint64T>(base, offset);
    return Word64Equal(element, Int64Constant(kHoleNanInt64));
  } else {
    TNode<Uint32T> element_upper = Load<Uint32T>(
        base, IntPtrAdd(offset, IntPtrConstant(kIeeeDoubleExponentWordOffset)));
    return Word32Equal(element_upper, Int32Constant(kHoleNanUpper32));
  }
}

TNode<Float64T> CodeStubAssembler::LoadDoubleWithHoleCheck(
    TNode<Object> base, TNode<IntPtrT> offset, Label* if_hole,
    MachineType machine_type) {
  if (if_hole) {
    GotoIf(IsDoubleHole(base, offset), if_hole);
  }
  if (machine_type.IsNone()) {
    // This means the actual value is not needed.
    return TNode<Float64T>();
  }
  return UncheckedCast<Float64T>(Load(machine_type, base, offset));
}

TNode<ScopeInfo> CodeStubAssembler::LoadScopeInfo(TNode<Context> context) {
  return CAST(LoadContextElement(context, Context::SCOPE_INFO_INDEX));
}

TNode<BoolT> CodeStubAssembler::LoadScopeInfoHasExtensionField(
    TNode<ScopeInfo> scope_info) {
  TNode<IntPtrT> value =
      LoadAndUntagObjectField(scope_info, ScopeInfo::kFlagsOffset);
  return IsSetWord<ScopeInfo::HasContextExtensionSlotBit>(value);
}

void CodeStubAssembler::StoreContextElementNoWriteBarrier(
    TNode<Context> context, int slot_index, SloppyTNode<Object> value) {
  int offset = Context::SlotOffset(slot_index);
  StoreNoWriteBarrier(MachineRepresentation::kTagged, context,
                      IntPtrConstant(offset), value);
}

TNode<NativeContext> CodeStubAssembler::LoadNativeContext(
    TNode<Context> context) {
  TNode<Map> map = LoadMap(context);
  return CAST(LoadObjectField(
      map, Map::kConstructorOrBackPointerOrNativeContextOffset));
}

TNode<Context> CodeStubAssembler::LoadModuleContext(TNode<Context> context) {
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Map> module_map = CAST(
      LoadContextElement(native_context, Context::MODULE_CONTEXT_MAP_INDEX));
  TVariable<Object> cur_context(context, this);

  Label context_found(this);

  Label context_search(this, &cur_context);

  // Loop until cur_context->map() is module_map.
  Goto(&context_search);
  BIND(&context_search);
  {
    CSA_ASSERT(this, Word32BinaryNot(
                         TaggedEqual(cur_context.value(), native_context)));
    GotoIf(TaggedEqual(LoadMap(CAST(cur_context.value())), module_map),
           &context_found);

    cur_context =
        LoadContextElement(CAST(cur_context.value()), Context::PREVIOUS_INDEX);
    Goto(&context_search);
  }

  BIND(&context_found);
  return UncheckedCast<Context>(cur_context.value());
}

TNode<Map> CodeStubAssembler::LoadObjectFunctionInitialMap(
    TNode<NativeContext> native_context) {
  TNode<JSFunction> object_function =
      CAST(LoadContextElement(native_context, Context::OBJECT_FUNCTION_INDEX));
  return CAST(LoadJSFunctionPrototypeOrInitialMap(object_function));
}

TNode<Map> CodeStubAssembler::LoadSlowObjectWithNullPrototypeMap(
    TNode<NativeContext> native_context) {
  TNode<Map> map = CAST(LoadContextElement(
      native_context, Context::SLOW_OBJECT_WITH_NULL_PROTOTYPE_MAP));
  return map;
}

TNode<Map> CodeStubAssembler::LoadJSArrayElementsMap(
    SloppyTNode<Int32T> kind, TNode<NativeContext> native_context) {
  CSA_ASSERT(this, IsFastElementsKind(kind));
  TNode<IntPtrT> offset =
      IntPtrAdd(IntPtrConstant(Context::FIRST_JS_ARRAY_MAP_SLOT),
                ChangeInt32ToIntPtr(kind));
  return UncheckedCast<Map>(LoadContextElement(native_context, offset));
}

TNode<Map> CodeStubAssembler::LoadJSArrayElementsMap(
    ElementsKind kind, TNode<NativeContext> native_context) {
  return UncheckedCast<Map>(
      LoadContextElement(native_context, Context::ArrayMapIndex(kind)));
}

TNode<BoolT> CodeStubAssembler::IsGeneratorFunction(
    TNode<JSFunction> function) {
  const TNode<SharedFunctionInfo> shared_function_info =
      LoadObjectField<SharedFunctionInfo>(
          function, JSFunction::kSharedFunctionInfoOffset);

  const TNode<Uint32T> function_kind =
      DecodeWord32<SharedFunctionInfo::FunctionKindBits>(
          LoadObjectField<Uint32T>(shared_function_info,
                                   SharedFunctionInfo::kFlagsOffset));

  // See IsGeneratorFunction(FunctionKind kind).
  return IsInRange(function_kind, FunctionKind::kAsyncConciseGeneratorMethod,
                   FunctionKind::kConciseGeneratorMethod);
}

TNode<BoolT> CodeStubAssembler::IsJSFunctionWithPrototypeSlot(
    TNode<HeapObject> object) {
  // Only JSFunction maps may have HasPrototypeSlotBit set.
  return TNode<BoolT>::UncheckedCast(
      IsSetWord32<Map::Bits1::HasPrototypeSlotBit>(
          LoadMapBitField(LoadMap(object))));
}

void CodeStubAssembler::BranchIfHasPrototypeProperty(
    TNode<JSFunction> function, TNode<Int32T> function_map_bit_field,
    Label* if_true, Label* if_false) {
  // (has_prototype_slot() && IsConstructor()) ||
  // IsGeneratorFunction(shared()->kind())
  uint32_t mask = Map::Bits1::HasPrototypeSlotBit::kMask |
                  Map::Bits1::IsConstructorBit::kMask;

  GotoIf(IsAllSetWord32(function_map_bit_field, mask), if_true);
  Branch(IsGeneratorFunction(function), if_true, if_false);
}

void CodeStubAssembler::GotoIfPrototypeRequiresRuntimeLookup(
    TNode<JSFunction> function, TNode<Map> map, Label* runtime) {
  // !has_prototype_property() || has_non_instance_prototype()
  TNode<Int32T> map_bit_field = LoadMapBitField(map);
  Label next_check(this);
  BranchIfHasPrototypeProperty(function, map_bit_field, &next_check, runtime);
  BIND(&next_check);
  GotoIf(IsSetWord32<Map::Bits1::HasNonInstancePrototypeBit>(map_bit_field),
         runtime);
}

TNode<HeapObject> CodeStubAssembler::LoadJSFunctionPrototype(
    TNode<JSFunction> function, Label* if_bailout) {
  CSA_ASSERT(this, IsFunctionWithPrototypeSlotMap(LoadMap(function)));
  CSA_ASSERT(this, IsClearWord32<Map::Bits1::HasNonInstancePrototypeBit>(
                       LoadMapBitField(LoadMap(function))));
  TNode<HeapObject> proto_or_map = LoadObjectField<HeapObject>(
      function, JSFunction::kPrototypeOrInitialMapOffset);
  GotoIf(IsTheHole(proto_or_map), if_bailout);

  TVARIABLE(HeapObject, var_result, proto_or_map);
  Label done(this, &var_result);
  GotoIfNot(IsMap(proto_or_map), &done);

  var_result = LoadMapPrototype(CAST(proto_or_map));
  Goto(&done);

  BIND(&done);
  return var_result.value();
}

TNode<BytecodeArray> CodeStubAssembler::LoadSharedFunctionInfoBytecodeArray(
    TNode<SharedFunctionInfo> shared) {
  TNode<HeapObject> function_data = LoadObjectField<HeapObject>(
      shared, SharedFunctionInfo::kFunctionDataOffset);

  TVARIABLE(HeapObject, var_result, function_data);
  Label done(this, &var_result);

  GotoIfNot(HasInstanceType(function_data, INTERPRETER_DATA_TYPE), &done);
  TNode<BytecodeArray> bytecode_array = LoadObjectField<BytecodeArray>(
      function_data, InterpreterData::kBytecodeArrayOffset);
  var_result = bytecode_array;
  Goto(&done);

  BIND(&done);
  return CAST(var_result.value());
}

void CodeStubAssembler::StoreObjectByteNoWriteBarrier(TNode<HeapObject> object,
                                                      int offset,
                                                      TNode<Word32T> value) {
  StoreNoWriteBarrier(MachineRepresentation::kWord8, object,
                      IntPtrConstant(offset - kHeapObjectTag), value);
}

void CodeStubAssembler::StoreHeapNumberValue(SloppyTNode<HeapNumber> object,
                                             SloppyTNode<Float64T> value) {
  StoreObjectFieldNoWriteBarrier(object, HeapNumber::kValueOffset, value);
}

void CodeStubAssembler::StoreObjectField(TNode<HeapObject> object, int offset,
                                         TNode<Object> value) {
  DCHECK_NE(HeapObject::kMapOffset, offset);  // Use StoreMap instead.

  OptimizedStoreField(MachineRepresentation::kTagged,
                      UncheckedCast<HeapObject>(object), offset, value);
}

void CodeStubAssembler::StoreObjectField(TNode<HeapObject> object,
                                         TNode<IntPtrT> offset,
                                         TNode<Object> value) {
  int const_offset;
  if (ToInt32Constant(offset, &const_offset)) {
    StoreObjectField(object, const_offset, value);
  } else {
    Store(object, IntPtrSub(offset, IntPtrConstant(kHeapObjectTag)), value);
  }
}

void CodeStubAssembler::UnsafeStoreObjectFieldNoWriteBarrier(
    TNode<HeapObject> object, int offset, TNode<Object> value) {
  OptimizedStoreFieldUnsafeNoWriteBarrier(MachineRepresentation::kTagged,
                                          object, offset, value);
}

void CodeStubAssembler::StoreMap(TNode<HeapObject> object, TNode<Map> map) {
  OptimizedStoreMap(object, map);
}

void CodeStubAssembler::StoreMapNoWriteBarrier(TNode<HeapObject> object,
                                               RootIndex map_root_index) {
  StoreMapNoWriteBarrier(object, CAST(LoadRoot(map_root_index)));
}

void CodeStubAssembler::StoreMapNoWriteBarrier(TNode<HeapObject> object,
                                               TNode<Map> map) {
  OptimizedStoreFieldAssertNoWriteBarrier(MachineRepresentation::kTaggedPointer,
                                          object, HeapObject::kMapOffset, map);
}

void CodeStubAssembler::StoreObjectFieldRoot(TNode<HeapObject> object,
                                             int offset, RootIndex root_index) {
  if (RootsTable::IsImmortalImmovable(root_index)) {
    StoreObjectFieldNoWriteBarrier(object, offset, LoadRoot(root_index));
  } else {
    StoreObjectField(object, offset, LoadRoot(root_index));
  }
}

template <typename TIndex>
void CodeStubAssembler::StoreFixedArrayOrPropertyArrayElement(
    TNode<UnionT<FixedArray, PropertyArray>> object, TNode<TIndex> index_node,
    TNode<Object> value, WriteBarrierMode barrier_mode, int additional_offset) {
  // TODO(v8:9708): Do we want to keep both IntPtrT and UintPtrT variants?
  static_assert(std::is_same<TIndex, Smi>::value ||
                    std::is_same<TIndex, UintPtrT>::value ||
                    std::is_same<TIndex, IntPtrT>::value,
                "Only Smi, UintPtrT or IntPtrT index is allowed");
  DCHECK(barrier_mode == SKIP_WRITE_BARRIER ||
         barrier_mode == UNSAFE_SKIP_WRITE_BARRIER ||
         barrier_mode == UPDATE_WRITE_BARRIER ||
         barrier_mode == UPDATE_EPHEMERON_KEY_WRITE_BARRIER);
  DCHECK(IsAligned(additional_offset, kTaggedSize));
  STATIC_ASSERT(static_cast<int>(FixedArray::kHeaderSize) ==
                static_cast<int>(PropertyArray::kHeaderSize));
  int header_size =
      FixedArray::kHeaderSize + additional_offset - kHeapObjectTag;
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(index_node, HOLEY_ELEMENTS, header_size);
  STATIC_ASSERT(static_cast<int>(FixedArrayBase::kLengthOffset) ==
                static_cast<int>(WeakFixedArray::kLengthOffset));
  STATIC_ASSERT(static_cast<int>(FixedArrayBase::kLengthOffset) ==
                static_cast<int>(PropertyArray::kLengthAndHashOffset));
  // Check that index_node + additional_offset <= object.length.
  // TODO(cbruni): Use proper LoadXXLength helpers
  CSA_ASSERT(
      this,
      IsOffsetInBounds(
          offset,
          Select<IntPtrT>(
              IsPropertyArray(object),
              [=] {
                TNode<IntPtrT> length_and_hash = LoadAndUntagObjectField(
                    object, PropertyArray::kLengthAndHashOffset);
                return TNode<IntPtrT>::UncheckedCast(
                    DecodeWord<PropertyArray::LengthField>(length_and_hash));
              },
              [=] {
                return LoadAndUntagObjectField(object,
                                               FixedArrayBase::kLengthOffset);
              }),
          FixedArray::kHeaderSize));
  if (barrier_mode == SKIP_WRITE_BARRIER) {
    StoreNoWriteBarrier(MachineRepresentation::kTagged, object, offset, value);
  } else if (barrier_mode == UNSAFE_SKIP_WRITE_BARRIER) {
    UnsafeStoreNoWriteBarrier(MachineRepresentation::kTagged, object, offset,
                              value);
  } else if (barrier_mode == UPDATE_EPHEMERON_KEY_WRITE_BARRIER) {
    StoreEphemeronKey(object, offset, value);
  } else {
    Store(object, offset, value);
  }
}

template V8_EXPORT_PRIVATE void
CodeStubAssembler::StoreFixedArrayOrPropertyArrayElement<Smi>(
    TNode<UnionT<FixedArray, PropertyArray>>, TNode<Smi>, TNode<Object>,
    WriteBarrierMode, int);

template V8_EXPORT_PRIVATE void
CodeStubAssembler::StoreFixedArrayOrPropertyArrayElement<IntPtrT>(
    TNode<UnionT<FixedArray, PropertyArray>>, TNode<IntPtrT>, TNode<Object>,
    WriteBarrierMode, int);

template V8_EXPORT_PRIVATE void
CodeStubAssembler::StoreFixedArrayOrPropertyArrayElement<UintPtrT>(
    TNode<UnionT<FixedArray, PropertyArray>>, TNode<UintPtrT>, TNode<Object>,
    WriteBarrierMode, int);

template <typename TIndex>
void CodeStubAssembler::StoreFixedDoubleArrayElement(
    TNode<FixedDoubleArray> object, TNode<TIndex> index, TNode<Float64T> value,
    CheckBounds check_bounds) {
  // TODO(v8:9708): Do we want to keep both IntPtrT and UintPtrT variants?
  static_assert(std::is_same<TIndex, Smi>::value ||
                    std::is_same<TIndex, UintPtrT>::value ||
                    std::is_same<TIndex, IntPtrT>::value,
                "Only Smi, UintPtrT or IntPtrT index is allowed");
  if (NeedsBoundsCheck(check_bounds)) {
    FixedArrayBoundsCheck(object, index, 0);
  }
  TNode<IntPtrT> offset = ElementOffsetFromIndex(
      index, PACKED_DOUBLE_ELEMENTS, FixedArray::kHeaderSize - kHeapObjectTag);
  MachineRepresentation rep = MachineRepresentation::kFloat64;
  // Make sure we do not store signalling NaNs into double arrays.
  TNode<Float64T> value_silenced = Float64SilenceNaN(value);
  StoreNoWriteBarrier(rep, object, offset, value_silenced);
}

// Export the Smi version which is used outside of code-stub-assembler.
template V8_EXPORT_PRIVATE void CodeStubAssembler::StoreFixedDoubleArrayElement<
    Smi>(TNode<FixedDoubleArray>, TNode<Smi>, TNode<Float64T>, CheckBounds);

void CodeStubAssembler::StoreFeedbackVectorSlot(
    TNode<FeedbackVector> feedback_vector, TNode<UintPtrT> slot,
    TNode<AnyTaggedT> value, WriteBarrierMode barrier_mode,
    int additional_offset) {
  DCHECK(IsAligned(additional_offset, kTaggedSize));
  DCHECK(barrier_mode == SKIP_WRITE_BARRIER ||
         barrier_mode == UNSAFE_SKIP_WRITE_BARRIER ||
         barrier_mode == UPDATE_WRITE_BARRIER);
  int header_size = FeedbackVector::kRawFeedbackSlotsOffset +
                    additional_offset - kHeapObjectTag;
  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(Signed(slot), HOLEY_ELEMENTS, header_size);
  // Check that slot <= feedback_vector.length.
  CSA_ASSERT(this,
             IsOffsetInBounds(offset, LoadFeedbackVectorLength(feedback_vector),
                              FeedbackVector::kHeaderSize));
  if (barrier_mode == SKIP_WRITE_BARRIER) {
    StoreNoWriteBarrier(MachineRepresentation::kTagged, feedback_vector, offset,
                        value);
  } else if (barrier_mode == UNSAFE_SKIP_WRITE_BARRIER) {
    UnsafeStoreNoWriteBarrier(MachineRepresentation::kTagged, feedback_vector,
                              offset, value);
  } else {
    Store(feedback_vector, offset, value);
  }
}

TNode<Int32T> CodeStubAssembler::EnsureArrayPushable(TNode<Context> context,
                                                     TNode<Map> map,
                                                     Label* bailout) {
  // Disallow pushing onto prototypes. It might be the JSArray prototype.
  // Disallow pushing onto non-extensible objects.
  Comment("Disallow pushing onto prototypes");
  GotoIfNot(IsExtensibleNonPrototypeMap(map), bailout);

  EnsureArrayLengthWritable(context, map, bailout);

  TNode<Uint32T> kind =
      DecodeWord32<Map::Bits2::ElementsKindBits>(LoadMapBitField2(map));
  return Signed(kind);
}

void CodeStubAssembler::PossiblyGrowElementsCapacity(
    ElementsKind kind, TNode<HeapObject> array, TNode<BInt> length,
    TVariable<FixedArrayBase>* var_elements, TNode<BInt> growth,
    Label* bailout) {
  Label fits(this, var_elements);
  TNode<BInt> capacity =
      TaggedToParameter<BInt>(LoadFixedArrayBaseLength(var_elements->value()));

  TNode<BInt> new_length = IntPtrOrSmiAdd(growth, length);
  GotoIfNot(IntPtrOrSmiGreaterThan(new_length, capacity), &fits);
  TNode<BInt> new_capacity = CalculateNewElementsCapacity(new_length);
  *var_elements = GrowElementsCapacity(array, var_elements->value(), kind, kind,
                                       capacity, new_capacity, bailout);
  Goto(&fits);
  BIND(&fits);
}

TNode<Smi> CodeStubAssembler::BuildAppendJSArray(ElementsKind kind,
                                                 TNode<JSArray> array,
                                                 CodeStubArguments* args,
                                                 TVariable<IntPtrT>* arg_index,
                                                 Label* bailout) {
  Comment("BuildAppendJSArray: ", ElementsKindToString(kind));
  Label pre_bailout(this);
  Label success(this);
  TVARIABLE(Smi, var_tagged_length);
  TVARIABLE(BInt, var_length, SmiToBInt(LoadFastJSArrayLength(array)));
  TVARIABLE(FixedArrayBase, var_elements, LoadElements(array));

  // Resize the capacity of the fixed array if it doesn't fit.
  TNode<IntPtrT> first = arg_index->value();
  TNode<BInt> growth = IntPtrToBInt(IntPtrSub(args->GetLength(), first));
  PossiblyGrowElementsCapacity(kind, array, var_length.value(), &var_elements,
                               growth, &pre_bailout);

  // Push each argument onto the end of the array now that there is enough
  // capacity.
  CodeStubAssembler::VariableList push_vars({&var_length}, zone());
  TNode<FixedArrayBase> elements = var_elements.value();
  args->ForEach(
      push_vars,
      [&](TNode<Object> arg) {
        TryStoreArrayElement(kind, &pre_bailout, elements, var_length.value(),
                             arg);
        Increment(&var_length);
      },
      first);
  {
    TNode<Smi> length = BIntToSmi(var_length.value());
    var_tagged_length = length;
    StoreObjectFieldNoWriteBarrier(array, JSArray::kLengthOffset, length);
    Goto(&success);
  }

  BIND(&pre_bailout);
  {
    TNode<Smi> length = ParameterToTagged(var_length.value());
    var_tagged_length = length;
    TNode<Smi> diff = SmiSub(length, LoadFastJSArrayLength(array));
    StoreObjectFieldNoWriteBarrier(array, JSArray::kLengthOffset, length);
    *arg_index = IntPtrAdd(arg_index->value(), SmiUntag(diff));
    Goto(bailout);
  }

  BIND(&success);
  return var_tagged_length.value();
}

void CodeStubAssembler::TryStoreArrayElement(ElementsKind kind, Label* bailout,
                                             TNode<FixedArrayBase> elements,
                                             TNode<BInt> index,
                                             TNode<Object> value) {
  if (IsSmiElementsKind(kind)) {
    GotoIf(TaggedIsNotSmi(value), bailout);
  } else if (IsDoubleElementsKind(kind)) {
    GotoIfNotNumber(value, bailout);
  }

  if (IsDoubleElementsKind(kind)) {
    StoreElement(elements, kind, index, ChangeNumberToFloat64(CAST(value)));
  } else {
    StoreElement(elements, kind, index, value);
  }
}

void CodeStubAssembler::BuildAppendJSArray(ElementsKind kind,
                                           TNode<JSArray> array,
                                           TNode<Object> value,
                                           Label* bailout) {
  Comment("BuildAppendJSArray: ", ElementsKindToString(kind));
  TVARIABLE(BInt, var_length, SmiToBInt(LoadFastJSArrayLength(array)));
  TVARIABLE(FixedArrayBase, var_elements, LoadElements(array));

  // Resize the capacity of the fixed array if it doesn't fit.
  TNode<BInt> growth = IntPtrOrSmiConstant<BInt>(1);
  PossiblyGrowElementsCapacity(kind, array, var_length.value(), &var_elements,
                               growth, bailout);

  // Push each argument onto the end of the array now that there is enough
  // capacity.
  TryStoreArrayElement(kind, bailout, var_elements.value(), var_length.value(),
                       value);
  Increment(&var_length);

  TNode<Smi> length = BIntToSmi(var_length.value());
  StoreObjectFieldNoWriteBarrier(array, JSArray::kLengthOffset, length);
}

TNode<Cell> CodeStubAssembler::AllocateCellWithValue(TNode<Object> value,
                                                     WriteBarrierMode mode) {
  TNode<HeapObject> result = Allocate(Cell::kSize, kNone);
  StoreMapNoWriteBarrier(result, RootIndex::kCellMap);
  TNode<Cell> cell = CAST(result);
  StoreCellValue(cell, value, mode);
  return cell;
}

TNode<Object> CodeStubAssembler::LoadCellValue(TNode<Cell> cell) {
  return LoadObjectField(cell, Cell::kValueOffset);
}

void CodeStubAssembler::StoreCellValue(TNode<Cell> cell, TNode<Object> value,
                                       WriteBarrierMode mode) {
  DCHECK(mode == SKIP_WRITE_BARRIER || mode == UPDATE_WRITE_BARRIER);

  if (mode == UPDATE_WRITE_BARRIER) {
    StoreObjectField(cell, Cell::kValueOffset, value);
  } else {
    StoreObjectFieldNoWriteBarrier(cell, Cell::kValueOffset, value);
  }
}

TNode<HeapNumber> CodeStubAssembler::AllocateHeapNumber() {
  TNode<HeapObject> result = Allocate(HeapNumber::kSize, kNone);
  RootIndex heap_map_index = RootIndex::kHeapNumberMap;
  StoreMapNoWriteBarrier(result, heap_map_index);
  return UncheckedCast<HeapNumber>(result);
}

TNode<HeapNumber> CodeStubAssembler::AllocateHeapNumberWithValue(
    SloppyTNode<Float64T> value) {
  TNode<HeapNumber> result = AllocateHeapNumber();
  StoreHeapNumberValue(result, value);
  return result;
}

TNode<Object> CodeStubAssembler::CloneIfMutablePrimitive(TNode<Object> object) {
  TVARIABLE(Object, result, object);
  Label done(this);

  GotoIf(TaggedIsSmi(object), &done);
  // TODO(leszeks): Read the field descriptor to decide if this heap number is
  // mutable or not.
  GotoIfNot(IsHeapNumber(UncheckedCast<HeapObject>(object)), &done);
  {
    // Mutable heap number found --- allocate a clone.
    TNode<Float64T> value =
        LoadHeapNumberValue(UncheckedCast<HeapNumber>(object));
    result = AllocateHeapNumberWithValue(value);
    Goto(&done);
  }

  BIND(&done);
  return result.value();
}

TNode<BigInt> CodeStubAssembler::AllocateBigInt(TNode<IntPtrT> length) {
  TNode<BigInt> result = AllocateRawBigInt(length);
  StoreBigIntBitfield(result,
                      Word32Shl(TruncateIntPtrToInt32(length),
                                Int32Constant(BigInt::LengthBits::kShift)));
  return result;
}

TNode<BigInt> CodeStubAssembler::AllocateRawBigInt(TNode<IntPtrT> length) {
  TNode<IntPtrT> size =
      IntPtrAdd(IntPtrConstant(BigInt::kHeaderSize),
                Signed(WordShl(length, kSystemPointerSizeLog2)));
  TNode<HeapObject> raw_result = Allocate(size, kAllowLargeObjectAllocation);
  StoreMapNoWriteBarrier(raw_result, RootIndex::kBigIntMap);
  if (FIELD_SIZE(BigInt::kOptionalPaddingOffset) != 0) {
    DCHECK_EQ(4, FIELD_SIZE(BigInt::kOptionalPaddingOffset));
    StoreObjectFieldNoWriteBarrier(raw_result, BigInt::kOptionalPaddingOffset,
                                   Int32Constant(0));
  }
  return UncheckedCast<BigInt>(raw_result);
}

void CodeStubAssembler::StoreBigIntBitfield(TNode<BigInt> bigint,
                                            TNode<Word32T> bitfield) {
  StoreObjectFieldNoWriteBarrier(bigint, BigInt::kBitfieldOffset, bitfield);
}

void CodeStubAssembler::StoreBigIntDigit(TNode<BigInt> bigint,
                                         intptr_t digit_index,
                                         TNode<UintPtrT> digit) {
  CHECK_LE(0, digit_index);
  CHECK_LT(digit_index, BigInt::kMaxLength);
  StoreObjectFieldNoWriteBarrier(
      bigint,
      BigInt::kDigitsOffset +
          static_cast<int>(digit_index) * kSystemPointerSize,
      digit);
}

void CodeStubAssembler::StoreBigIntDigit(TNode<BigInt> bigint,
                                         TNode<IntPtrT> digit_index,
                                         TNode<UintPtrT> digit) {
  TNode<IntPtrT> offset =
      IntPtrAdd(IntPtrConstant(BigInt::kDigitsOffset),
                IntPtrMul(digit_index, IntPtrConstant(kSystemPointerSize)));
  StoreObjectFieldNoWriteBarrier(bigint, offset, digit);
}

TNode<Word32T> CodeStubAssembler::LoadBigIntBitfield(TNode<BigInt> bigint) {
  return UncheckedCast<Word32T>(
      LoadObjectField<Uint32T>(bigint, BigInt::kBitfieldOffset));
}

TNode<UintPtrT> CodeStubAssembler::LoadBigIntDigit(TNode<BigInt> bigint,
                                                   intptr_t digit_index) {
  CHECK_LE(0, digit_index);
  CHECK_LT(digit_index, BigInt::kMaxLength);
  return LoadObjectField<UintPtrT>(
      bigint, BigInt::kDigitsOffset +
                  static_cast<int>(digit_index) * kSystemPointerSize);
}

TNode<UintPtrT> CodeStubAssembler::LoadBigIntDigit(TNode<BigInt> bigint,
                                                   TNode<IntPtrT> digit_index) {
  TNode<IntPtrT> offset =
      IntPtrAdd(IntPtrConstant(BigInt::kDigitsOffset),
                IntPtrMul(digit_index, IntPtrConstant(kSystemPointerSize)));
  return LoadObjectField<UintPtrT>(bigint, offset);
}

TNode<ByteArray> CodeStubAssembler::AllocateByteArray(TNode<UintPtrT> length,
                                                      AllocationFlags flags) {
  Comment("AllocateByteArray");
  TVARIABLE(Object, var_result);

  // Compute the ByteArray size and check if it fits into new space.
  Label if_lengthiszero(this), if_sizeissmall(this),
      if_notsizeissmall(this, Label::kDeferred), if_join(this);
  GotoIf(WordEqual(length, UintPtrConstant(0)), &if_lengthiszero);

  TNode<IntPtrT> raw_size =
      GetArrayAllocationSize(Signed(length), UINT8_ELEMENTS,
                             ByteArray::kHeaderSize + kObjectAlignmentMask);
  TNode<IntPtrT> size =
      WordAnd(raw_size, IntPtrConstant(~kObjectAlignmentMask));
  Branch(IntPtrLessThanOrEqual(size, IntPtrConstant(kMaxRegularHeapObjectSize)),
         &if_sizeissmall, &if_notsizeissmall);

  BIND(&if_sizeissmall);
  {
    // Just allocate the ByteArray in new space.
    TNode<HeapObject> result =
        AllocateInNewSpace(UncheckedCast<IntPtrT>(size), flags);
    DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kByteArrayMap));
    StoreMapNoWriteBarrier(result, RootIndex::kByteArrayMap);
    StoreObjectFieldNoWriteBarrier(result, ByteArray::kLengthOffset,
                                   SmiTag(Signed(length)));
    var_result = result;
    Goto(&if_join);
  }

  BIND(&if_notsizeissmall);
  {
    // We might need to allocate in large object space, go to the runtime.
    TNode<Object> result =
        CallRuntime(Runtime::kAllocateByteArray, NoContextConstant(),
                    ChangeUintPtrToTagged(length));
    var_result = result;
    Goto(&if_join);
  }

  BIND(&if_lengthiszero);
  {
    var_result = EmptyByteArrayConstant();
    Goto(&if_join);
  }

  BIND(&if_join);
  return CAST(var_result.value());
}

TNode<String> CodeStubAssembler::AllocateSeqOneByteString(
    uint32_t length, AllocationFlags flags) {
  Comment("AllocateSeqOneByteString");
  if (length == 0) {
    return EmptyStringConstant();
  }
  TNode<HeapObject> result = Allocate(SeqOneByteString::SizeFor(length), flags);
  DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kOneByteStringMap));
  StoreMapNoWriteBarrier(result, RootIndex::kOneByteStringMap);
  StoreObjectFieldNoWriteBarrier(result, SeqOneByteString::kLengthOffset,
                                 Uint32Constant(length));
  StoreObjectFieldNoWriteBarrier(result, SeqOneByteString::kHashFieldOffset,
                                 Int32Constant(String::kEmptyHashField));
  return CAST(result);
}

TNode<BoolT> CodeStubAssembler::IsZeroOrContext(SloppyTNode<Object> object) {
  return Select<BoolT>(
      TaggedEqual(object, SmiConstant(0)), [=] { return Int32TrueConstant(); },
      [=] { return IsContext(CAST(object)); });
}

TNode<String> CodeStubAssembler::AllocateSeqTwoByteString(
    uint32_t length, AllocationFlags flags) {
  Comment("AllocateSeqTwoByteString");
  if (length == 0) {
    return EmptyStringConstant();
  }
  TNode<HeapObject> result = Allocate(SeqTwoByteString::SizeFor(length), flags);
  DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kStringMap));
  StoreMapNoWriteBarrier(result, RootIndex::kStringMap);
  StoreObjectFieldNoWriteBarrier(result, SeqTwoByteString::kLengthOffset,
                                 Uint32Constant(length));
  StoreObjectFieldNoWriteBarrier(result, SeqTwoByteString::kHashFieldOffset,
                                 Int32Constant(String::kEmptyHashField));
  return CAST(result);
}

TNode<String> CodeStubAssembler::AllocateSlicedString(RootIndex map_root_index,
                                                      TNode<Uint32T> length,
                                                      TNode<String> parent,
                                                      TNode<Smi> offset) {
  DCHECK(map_root_index == RootIndex::kSlicedOneByteStringMap ||
         map_root_index == RootIndex::kSlicedStringMap);
  TNode<HeapObject> result = Allocate(SlicedString::kSize);
  DCHECK(RootsTable::IsImmortalImmovable(map_root_index));
  StoreMapNoWriteBarrier(result, map_root_index);
  StoreObjectFieldNoWriteBarrier(result, SlicedString::kHashFieldOffset,
                                 Int32Constant(String::kEmptyHashField));
  StoreObjectFieldNoWriteBarrier(result, SlicedString::kLengthOffset, length);
  StoreObjectFieldNoWriteBarrier(result, SlicedString::kParentOffset, parent);
  StoreObjectFieldNoWriteBarrier(result, SlicedString::kOffsetOffset, offset);
  return CAST(result);
}

TNode<String> CodeStubAssembler::AllocateSlicedOneByteString(
    TNode<Uint32T> length, TNode<String> parent, TNode<Smi> offset) {
  return AllocateSlicedString(RootIndex::kSlicedOneByteStringMap, length,
                              parent, offset);
}

TNode<String> CodeStubAssembler::AllocateSlicedTwoByteString(
    TNode<Uint32T> length, TNode<String> parent, TNode<Smi> offset) {
  return AllocateSlicedString(RootIndex::kSlicedStringMap, length, parent,
                              offset);
}

TNode<NameDictionary> CodeStubAssembler::AllocateNameDictionary(
    int at_least_space_for) {
  return AllocateNameDictionary(IntPtrConstant(at_least_space_for));
}

TNode<NameDictionary> CodeStubAssembler::AllocateNameDictionary(
    TNode<IntPtrT> at_least_space_for, AllocationFlags flags) {
  CSA_ASSERT(this, UintPtrLessThanOrEqual(
                       at_least_space_for,
                       IntPtrConstant(NameDictionary::kMaxCapacity)));
  TNode<IntPtrT> capacity = HashTableComputeCapacity(at_least_space_for);
  return AllocateNameDictionaryWithCapacity(capacity, flags);
}

TNode<NameDictionary> CodeStubAssembler::AllocateNameDictionaryWithCapacity(
    TNode<IntPtrT> capacity, AllocationFlags flags) {
  CSA_ASSERT(this, WordIsPowerOfTwo(capacity));
  CSA_ASSERT(this, IntPtrGreaterThan(capacity, IntPtrConstant(0)));
  TNode<IntPtrT> length = EntryToIndex<NameDictionary>(capacity);
  TNode<IntPtrT> store_size = IntPtrAdd(
      TimesTaggedSize(length), IntPtrConstant(NameDictionary::kHeaderSize));

  TNode<NameDictionary> result =
      UncheckedCast<NameDictionary>(Allocate(store_size, flags));

  // Initialize FixedArray fields.
  {
    DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kNameDictionaryMap));
    StoreMapNoWriteBarrier(result, RootIndex::kNameDictionaryMap);
    StoreObjectFieldNoWriteBarrier(result, FixedArray::kLengthOffset,
                                   SmiFromIntPtr(length));
  }

  // Initialized HashTable fields.
  {
    TNode<Smi> zero = SmiConstant(0);
    StoreFixedArrayElement(result, NameDictionary::kNumberOfElementsIndex, zero,
                           SKIP_WRITE_BARRIER);
    StoreFixedArrayElement(result,
                           NameDictionary::kNumberOfDeletedElementsIndex, zero,
                           SKIP_WRITE_BARRIER);
    StoreFixedArrayElement(result, NameDictionary::kCapacityIndex,
                           SmiTag(capacity), SKIP_WRITE_BARRIER);
    // Initialize Dictionary fields.
    StoreFixedArrayElement(result, NameDictionary::kNextEnumerationIndexIndex,
                           SmiConstant(PropertyDetails::kInitialIndex),
                           SKIP_WRITE_BARRIER);
    StoreFixedArrayElement(result, NameDictionary::kObjectHashIndex,
                           SmiConstant(PropertyArray::kNoHashSentinel),
                           SKIP_WRITE_BARRIER);
  }

  // Initialize NameDictionary elements.
  {
    TNode<IntPtrT> result_word = BitcastTaggedToWord(result);
    TNode<IntPtrT> start_address = IntPtrAdd(
        result_word, IntPtrConstant(NameDictionary::OffsetOfElementAt(
                                        NameDictionary::kElementsStartIndex) -
                                    kHeapObjectTag));
    TNode<IntPtrT> end_address = IntPtrAdd(
        result_word, IntPtrSub(store_size, IntPtrConstant(kHeapObjectTag)));

    TNode<Oddball> filler = UndefinedConstant();
    DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kUndefinedValue));

    StoreFieldsNoWriteBarrier(start_address, end_address, filler);
  }

  return result;
}

TNode<NameDictionary> CodeStubAssembler::CopyNameDictionary(
    TNode<NameDictionary> dictionary, Label* large_object_fallback) {
  Comment("Copy boilerplate property dict");
  TNode<IntPtrT> capacity = SmiUntag(GetCapacity<NameDictionary>(dictionary));
  CSA_ASSERT(this, IntPtrGreaterThanOrEqual(capacity, IntPtrConstant(0)));
  GotoIf(UintPtrGreaterThan(
             capacity, IntPtrConstant(NameDictionary::kMaxRegularCapacity)),
         large_object_fallback);
  TNode<NameDictionary> properties =
      AllocateNameDictionaryWithCapacity(capacity);
  TNode<IntPtrT> length = SmiUntag(LoadFixedArrayBaseLength(dictionary));
  CopyFixedArrayElements(PACKED_ELEMENTS, dictionary, properties, length,
                         SKIP_WRITE_BARRIER);
  return properties;
}

template <typename CollectionType>
TNode<CollectionType> CodeStubAssembler::AllocateOrderedHashTable() {
  static const int kCapacity = CollectionType::kInitialCapacity;
  static const int kBucketCount = kCapacity / CollectionType::kLoadFactor;
  static const int kDataTableLength = kCapacity * CollectionType::kEntrySize;
  static const int kFixedArrayLength =
      CollectionType::HashTableStartIndex() + kBucketCount + kDataTableLength;
  static const int kDataTableStartIndex =
      CollectionType::HashTableStartIndex() + kBucketCount;

  STATIC_ASSERT(base::bits::IsPowerOfTwo(kCapacity));
  STATIC_ASSERT(kCapacity <= CollectionType::MaxCapacity());

  // Allocate the table and add the proper map.
  const ElementsKind elements_kind = HOLEY_ELEMENTS;
  TNode<IntPtrT> length_intptr = IntPtrConstant(kFixedArrayLength);
  TNode<Map> fixed_array_map =
      HeapConstant(CollectionType::GetMap(ReadOnlyRoots(isolate())));
  TNode<CollectionType> table =
      CAST(AllocateFixedArray(elements_kind, length_intptr,
                              kAllowLargeObjectAllocation, fixed_array_map));

  // Initialize the OrderedHashTable fields.
  const WriteBarrierMode barrier_mode = SKIP_WRITE_BARRIER;
  StoreFixedArrayElement(table, CollectionType::NumberOfElementsIndex(),
                         SmiConstant(0), barrier_mode);
  StoreFixedArrayElement(table, CollectionType::NumberOfDeletedElementsIndex(),
                         SmiConstant(0), barrier_mode);
  StoreFixedArrayElement(table, CollectionType::NumberOfBucketsIndex(),
                         SmiConstant(kBucketCount), barrier_mode);

  // Fill the buckets with kNotFound.
  TNode<Smi> not_found = SmiConstant(CollectionType::kNotFound);
  STATIC_ASSERT(CollectionType::HashTableStartIndex() ==
                CollectionType::NumberOfBucketsIndex() + 1);
  STATIC_ASSERT((CollectionType::HashTableStartIndex() + kBucketCount) ==
                kDataTableStartIndex);
  for (int i = 0; i < kBucketCount; i++) {
    StoreFixedArrayElement(table, CollectionType::HashTableStartIndex() + i,
                           not_found, barrier_mode);
  }

  // Fill the data table with undefined.
  STATIC_ASSERT(kDataTableStartIndex + kDataTableLength == kFixedArrayLength);
  for (int i = 0; i < kDataTableLength; i++) {
    StoreFixedArrayElement(table, kDataTableStartIndex + i, UndefinedConstant(),
                           barrier_mode);
  }

  return table;
}

template TNode<OrderedHashMap>
CodeStubAssembler::AllocateOrderedHashTable<OrderedHashMap>();
template TNode<OrderedHashSet>
CodeStubAssembler::AllocateOrderedHashTable<OrderedHashSet>();

TNode<JSObject> CodeStubAssembler::AllocateJSObjectFromMap(
    TNode<Map> map, base::Optional<TNode<HeapObject>> properties,
    base::Optional<TNode<FixedArray>> elements, AllocationFlags flags,
    SlackTrackingMode slack_tracking_mode) {
  CSA_ASSERT(this, Word32BinaryNot(IsJSFunctionMap(map)));
  CSA_ASSERT(this, Word32BinaryNot(InstanceTypeEqual(LoadMapInstanceType(map),
                                                     JS_GLOBAL_OBJECT_TYPE)));
  TNode<IntPtrT> instance_size =
      TimesTaggedSize(LoadMapInstanceSizeInWords(map));
  TNode<HeapObject> object = AllocateInNewSpace(instance_size, flags);
  StoreMapNoWriteBarrier(object, map);
  InitializeJSObjectFromMap(object, map, instance_size, properties, elements,
                            slack_tracking_mode);
  return CAST(object);
}

void CodeStubAssembler::InitializeJSObjectFromMap(
    TNode<HeapObject> object, TNode<Map> map, TNode<IntPtrT> instance_size,
    base::Optional<TNode<HeapObject>> properties,
    base::Optional<TNode<FixedArray>> elements,
    SlackTrackingMode slack_tracking_mode) {
  // This helper assumes that the object is in new-space, as guarded by the
  // check in AllocatedJSObjectFromMap.
  if (!properties) {
    CSA_ASSERT(this, Word32BinaryNot(IsDictionaryMap((map))));
    StoreObjectFieldRoot(object, JSObject::kPropertiesOrHashOffset,
                         RootIndex::kEmptyFixedArray);
  } else {
    CSA_ASSERT(this, Word32Or(Word32Or(IsPropertyArray(*properties),
                                       IsNameDictionary(*properties)),
                              IsEmptyFixedArray(*properties)));
    StoreObjectFieldNoWriteBarrier(object, JSObject::kPropertiesOrHashOffset,
                                   *properties);
  }
  if (!elements) {
    StoreObjectFieldRoot(object, JSObject::kElementsOffset,
                         RootIndex::kEmptyFixedArray);
  } else {
    StoreObjectFieldNoWriteBarrier(object, JSObject::kElementsOffset,
                                   *elements);
  }
  if (slack_tracking_mode == kNoSlackTracking) {
    InitializeJSObjectBodyNoSlackTracking(object, map, instance_size);
  } else {
    DCHECK_EQ(slack_tracking_mode, kWithSlackTracking);
    InitializeJSObjectBodyWithSlackTracking(object, map, instance_size);
  }
}

void CodeStubAssembler::InitializeJSObjectBodyNoSlackTracking(
    TNode<HeapObject> object, TNode<Map> map,
    SloppyTNode<IntPtrT> instance_size, int start_offset) {
  STATIC_ASSERT(Map::kNoSlackTracking == 0);
  CSA_ASSERT(this, IsClearWord32<Map::Bits3::ConstructionCounterBits>(
                       LoadMapBitField3(map)));
  InitializeFieldsWithRoot(object, IntPtrConstant(start_offset), instance_size,
                           RootIndex::kUndefinedValue);
}

void CodeStubAssembler::InitializeJSObjectBodyWithSlackTracking(
    TNode<HeapObject> object, TNode<Map> map,
    SloppyTNode<IntPtrT> instance_size) {
  Comment("InitializeJSObjectBodyNoSlackTracking");

  // Perform in-object slack tracking if requested.
  int start_offset = JSObject::kHeaderSize;
  TNode<Uint32T> bit_field3 = LoadMapBitField3(map);
  Label end(this), slack_tracking(this), complete(this, Label::kDeferred);
  STATIC_ASSERT(Map::kNoSlackTracking == 0);
  GotoIf(IsSetWord32<Map::Bits3::ConstructionCounterBits>(bit_field3),
         &slack_tracking);
  Comment("No slack tracking");
  InitializeJSObjectBodyNoSlackTracking(object, map, instance_size);
  Goto(&end);

  BIND(&slack_tracking);
  {
    Comment("Decrease construction counter");
    // Slack tracking is only done on initial maps.
    CSA_ASSERT(this, IsUndefined(LoadMapBackPointer(map)));
    STATIC_ASSERT(Map::Bits3::ConstructionCounterBits::kLastUsedBit == 31);
    TNode<Word32T> new_bit_field3 = Int32Sub(
        bit_field3,
        Int32Constant(1 << Map::Bits3::ConstructionCounterBits::kShift));
    StoreObjectFieldNoWriteBarrier(map, Map::kBitField3Offset, new_bit_field3);
    STATIC_ASSERT(Map::kSlackTrackingCounterEnd == 1);

    // The object still has in-object slack therefore the |unsed_or_unused|
    // field contain the "used" value.
    TNode<IntPtrT> used_size =
        Signed(TimesTaggedSize(ChangeUint32ToWord(LoadObjectField<Uint8T>(
            map, Map::kUsedOrUnusedInstanceSizeInWordsOffset))));

    Comment("iInitialize filler fields");
    InitializeFieldsWithRoot(object, used_size, instance_size,
                             RootIndex::kOnePointerFillerMap);

    Comment("Initialize undefined fields");
    InitializeFieldsWithRoot(object, IntPtrConstant(start_offset), used_size,
                             RootIndex::kUndefinedValue);

    STATIC_ASSERT(Map::kNoSlackTracking == 0);
    GotoIf(IsClearWord32<Map::Bits3::ConstructionCounterBits>(new_bit_field3),
           &complete);
    Goto(&end);
  }

  // Finalize the instance size.
  BIND(&complete);
  {
    // ComplextInobjectSlackTracking doesn't allocate and thus doesn't need a
    // context.
    CallRuntime(Runtime::kCompleteInobjectSlackTrackingForMap,
                NoContextConstant(), map);
    Goto(&end);
  }

  BIND(&end);
}

void CodeStubAssembler::StoreFieldsNoWriteBarrier(TNode<IntPtrT> start_address,
                                                  TNode<IntPtrT> end_address,
                                                  TNode<Object> value) {
  Comment("StoreFieldsNoWriteBarrier");
  CSA_ASSERT(this, WordIsAligned(start_address, kTaggedSize));
  CSA_ASSERT(this, WordIsAligned(end_address, kTaggedSize));
  BuildFastLoop<IntPtrT>(
      start_address, end_address,
      [=](TNode<IntPtrT> current) {
        UnsafeStoreNoWriteBarrier(MachineRepresentation::kTagged, current,
                                  value);
      },
      kTaggedSize, IndexAdvanceMode::kPost);
}

void CodeStubAssembler::MakeFixedArrayCOW(TNode<FixedArray> array) {
  CSA_ASSERT(this, IsFixedArrayMap(LoadMap(array)));
  Label done(this);
  // The empty fixed array is not modifiable anyway. And we shouldn't change its
  // Map.
  GotoIf(TaggedEqual(array, EmptyFixedArrayConstant()), &done);
  StoreMap(array, FixedCOWArrayMapConstant());
  Goto(&done);
  BIND(&done);
}

TNode<BoolT> CodeStubAssembler::IsValidFastJSArrayCapacity(
    TNode<IntPtrT> capacity) {
  return UintPtrLessThanOrEqual(capacity,
                                UintPtrConstant(JSArray::kMaxFastArrayLength));
}

TNode<JSArray> CodeStubAssembler::AllocateJSArray(
    TNode<Map> array_map, TNode<FixedArrayBase> elements, TNode<Smi> length,
    base::Optional<TNode<AllocationSite>> allocation_site,
    int array_header_size) {
  Comment("begin allocation of JSArray passing in elements");
  CSA_SLOW_ASSERT(this, TaggedIsPositiveSmi(length));

  int base_size = array_header_size;
  if (allocation_site) {
    base_size += AllocationMemento::kSize;
  }

  TNode<IntPtrT> size = IntPtrConstant(base_size);
  TNode<JSArray> result =
      AllocateUninitializedJSArray(array_map, length, allocation_site, size);
  StoreObjectFieldNoWriteBarrier(result, JSArray::kElementsOffset, elements);
  return result;
}

std::pair<TNode<JSArray>, TNode<FixedArrayBase>>
CodeStubAssembler::AllocateUninitializedJSArrayWithElements(
    ElementsKind kind, TNode<Map> array_map, TNode<Smi> length,
    base::Optional<TNode<AllocationSite>> allocation_site,
    TNode<IntPtrT> capacity, AllocationFlags allocation_flags,
    int array_header_size) {
  Comment("begin allocation of JSArray with elements");
  CHECK_EQ(allocation_flags & ~kAllowLargeObjectAllocation, 0);
  CSA_SLOW_ASSERT(this, TaggedIsPositiveSmi(length));

  TVARIABLE(JSArray, array);
  TVARIABLE(FixedArrayBase, elements);

  Label out(this), empty(this), nonempty(this);

  int capacity_int;
  if (ToInt32Constant(capacity, &capacity_int)) {
    if (capacity_int == 0) {
      TNode<FixedArray> empty_array = EmptyFixedArrayConstant();
      array = AllocateJSArray(array_map, empty_array, length, allocation_site,
                              array_header_size);
      return {array.value(), empty_array};
    } else {
      Goto(&nonempty);
    }
  } else {
    Branch(WordEqual(capacity, IntPtrConstant(0)), &empty, &nonempty);

    BIND(&empty);
    {
      TNode<FixedArray> empty_array = EmptyFixedArrayConstant();
      array = AllocateJSArray(array_map, empty_array, length, allocation_site,
                              array_header_size);
      elements = empty_array;
      Goto(&out);
    }
  }

  BIND(&nonempty);
  {
    int base_size = array_header_size;
    if (allocation_site) {
      base_size += AllocationMemento::kSize;
    }

    const int elements_offset = base_size;

    // Compute space for elements
    base_size += FixedArray::kHeaderSize;
    TNode<IntPtrT> size = ElementOffsetFromIndex(capacity, kind, base_size);

    // For very large arrays in which the requested allocation exceeds the
    // maximal size of a regular heap object, we cannot use the allocation
    // folding trick. Instead, we first allocate the elements in large object
    // space, and then allocate the JSArray (and possibly the allocation
    // memento) in new space.
    if (allocation_flags & kAllowLargeObjectAllocation) {
      Label next(this);
      GotoIf(IsRegularHeapObjectSize(size), &next);

      CSA_CHECK(this, IsValidFastJSArrayCapacity(capacity));

      // Allocate and initialize the elements first. Full initialization is
      // needed because the upcoming JSArray allocation could trigger GC.
      elements = AllocateFixedArray(kind, capacity, allocation_flags);

      if (IsDoubleElementsKind(kind)) {
        FillFixedDoubleArrayWithZero(CAST(elements.value()), capacity);
      } else {
        FillFixedArrayWithSmiZero(CAST(elements.value()), capacity);
      }

      // The JSArray and possibly allocation memento next. Note that
      // allocation_flags are *not* passed on here and the resulting JSArray
      // will always be in new space.
      array = AllocateJSArray(array_map, elements.value(), length,
                              allocation_site, array_header_size);

      Goto(&out);

      BIND(&next);
    }

    // Fold all objects into a single new space allocation.
    array =
        AllocateUninitializedJSArray(array_map, length, allocation_site, size);
    elements = UncheckedCast<FixedArrayBase>(
        InnerAllocate(array.value(), elements_offset));

    StoreObjectFieldNoWriteBarrier(array.value(), JSObject::kElementsOffset,
                                   elements.value());

    // Setup elements object.
    STATIC_ASSERT(FixedArrayBase::kHeaderSize == 2 * kTaggedSize);
    RootIndex elements_map_index = IsDoubleElementsKind(kind)
                                       ? RootIndex::kFixedDoubleArrayMap
                                       : RootIndex::kFixedArrayMap;
    DCHECK(RootsTable::IsImmortalImmovable(elements_map_index));
    StoreMapNoWriteBarrier(elements.value(), elements_map_index);

    CSA_ASSERT(this, WordNotEqual(capacity, IntPtrConstant(0)));
    TNode<Smi> capacity_smi = SmiTag(capacity);
    StoreObjectFieldNoWriteBarrier(elements.value(), FixedArray::kLengthOffset,
                                   capacity_smi);
    Goto(&out);
  }

  BIND(&out);
  return {array.value(), elements.value()};
}

TNode<JSArray> CodeStubAssembler::AllocateUninitializedJSArray(
    TNode<Map> array_map, TNode<Smi> length,
    base::Optional<TNode<AllocationSite>> allocation_site,
    TNode<IntPtrT> size_in_bytes) {
  CSA_SLOW_ASSERT(this, TaggedIsPositiveSmi(length));

  // Allocate space for the JSArray and the elements FixedArray in one go.
  TNode<HeapObject> array = AllocateInNewSpace(size_in_bytes);

  StoreMapNoWriteBarrier(array, array_map);
  StoreObjectFieldNoWriteBarrier(array, JSArray::kLengthOffset, length);
  StoreObjectFieldRoot(array, JSArray::kPropertiesOrHashOffset,
                       RootIndex::kEmptyFixedArray);

  if (allocation_site) {
    InitializeAllocationMemento(array, IntPtrConstant(JSArray::kHeaderSize),
                                *allocation_site);
  }

  return CAST(array);
}

TNode<JSArray> CodeStubAssembler::AllocateJSArray(
    ElementsKind kind, TNode<Map> array_map, TNode<IntPtrT> capacity,
    TNode<Smi> length, base::Optional<TNode<AllocationSite>> allocation_site,
    AllocationFlags allocation_flags) {
  CSA_SLOW_ASSERT(this, TaggedIsPositiveSmi(length));

  TNode<JSArray> array;
  TNode<FixedArrayBase> elements;

  std::tie(array, elements) = AllocateUninitializedJSArrayWithElements(
      kind, array_map, length, allocation_site, capacity, allocation_flags);

  Label out(this), nonempty(this);

  Branch(WordEqual(capacity, IntPtrConstant(0)), &out, &nonempty);

  BIND(&nonempty);
  {
    FillFixedArrayWithValue(kind, elements, IntPtrConstant(0), capacity,
                            RootIndex::kTheHoleValue);
    Goto(&out);
  }

  BIND(&out);
  return array;
}

TNode<JSArray> CodeStubAssembler::ExtractFastJSArray(TNode<Context> context,
                                                     TNode<JSArray> array,
                                                     TNode<BInt> begin,
                                                     TNode<BInt> count) {
  TNode<Map> original_array_map = LoadMap(array);
  TNode<Int32T> elements_kind = LoadMapElementsKind(original_array_map);

  // Use the canonical map for the Array's ElementsKind
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Map> array_map = LoadJSArrayElementsMap(elements_kind, native_context);

  TNode<FixedArrayBase> new_elements = ExtractFixedArray(
      LoadElements(array), base::Optional<TNode<BInt>>(begin),
      base::Optional<TNode<BInt>>(count),
      base::Optional<TNode<BInt>>(base::nullopt),
      ExtractFixedArrayFlag::kAllFixedArrays, nullptr, elements_kind);

  TNode<JSArray> result = AllocateJSArray(
      array_map, new_elements, ParameterToTagged(count), base::nullopt);
  return result;
}

TNode<JSArray> CodeStubAssembler::CloneFastJSArray(
    TNode<Context> context, TNode<JSArray> array,
    base::Optional<TNode<AllocationSite>> allocation_site,
    HoleConversionMode convert_holes) {
  // TODO(dhai): we should be able to assert IsFastJSArray(array) here, but this
  // function is also used to copy boilerplates even when the no-elements
  // protector is invalid. This function should be renamed to reflect its uses.

  TNode<Number> length = LoadJSArrayLength(array);
  TNode<FixedArrayBase> new_elements;
  TVARIABLE(FixedArrayBase, var_new_elements);
  TVARIABLE(Int32T, var_elements_kind, LoadMapElementsKind(LoadMap(array)));

  Label allocate_jsarray(this), holey_extract(this),
      allocate_jsarray_main(this);

  bool need_conversion =
      convert_holes == HoleConversionMode::kConvertToUndefined;
  if (need_conversion) {
    // We need to take care of holes, if the array is of holey elements kind.
    GotoIf(IsHoleyFastElementsKindForRead(var_elements_kind.value()),
           &holey_extract);
  }

  // Simple extraction that preserves holes.
  new_elements = ExtractFixedArray(
      LoadElements(array),
      base::Optional<TNode<BInt>>(IntPtrOrSmiConstant<BInt>(0)),
      base::Optional<TNode<BInt>>(TaggedToParameter<BInt>(CAST(length))),
      base::Optional<TNode<BInt>>(base::nullopt),
      ExtractFixedArrayFlag::kAllFixedArraysDontCopyCOW, nullptr,
      var_elements_kind.value());
  var_new_elements = new_elements;
  Goto(&allocate_jsarray);

  if (need_conversion) {
    BIND(&holey_extract);
    // Convert holes to undefined.
    TVARIABLE(BoolT, var_holes_converted, Int32FalseConstant());
    // Copy |array|'s elements store. The copy will be compatible with the
    // original elements kind unless there are holes in the source. Any holes
    // get converted to undefined, hence in that case the copy is compatible
    // only with PACKED_ELEMENTS and HOLEY_ELEMENTS, and we will choose
    // PACKED_ELEMENTS. Also, if we want to replace holes, we must not use
    // ExtractFixedArrayFlag::kDontCopyCOW.
    new_elements = ExtractFixedArray(
        LoadElements(array),
        base::Optional<TNode<BInt>>(IntPtrOrSmiConstant<BInt>(0)),
        base::Optional<TNode<BInt>>(TaggedToParameter<BInt>(CAST(length))),
        base::Optional<TNode<BInt>>(base::nullopt),
        ExtractFixedArrayFlag::kAllFixedArrays, &var_holes_converted);
    var_new_elements = new_elements;
    // If the array type didn't change, use the original elements kind.
    GotoIfNot(var_holes_converted.value(), &allocate_jsarray);
    // Otherwise use PACKED_ELEMENTS for the target's elements kind.
    var_elements_kind = Int32Constant(PACKED_ELEMENTS);
    Goto(&allocate_jsarray);
  }

  BIND(&allocate_jsarray);

  // Handle any nonextensible elements kinds
  CSA_ASSERT(this, IsElementsKindLessThanOrEqual(
                       var_elements_kind.value(),
                       LAST_ANY_NONEXTENSIBLE_ELEMENTS_KIND));
  GotoIf(IsElementsKindLessThanOrEqual(var_elements_kind.value(),
                                       LAST_FAST_ELEMENTS_KIND),
         &allocate_jsarray_main);
  var_elements_kind = Int32Constant(PACKED_ELEMENTS);
  Goto(&allocate_jsarray_main);

  BIND(&allocate_jsarray_main);
  // Use the cannonical map for the chosen elements kind.
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Map> array_map =
      LoadJSArrayElementsMap(var_elements_kind.value(), native_context);

  TNode<JSArray> result = AllocateJSArray(array_map, var_new_elements.value(),
                                          CAST(length), allocation_site);
  return result;
}

template <typename TIndex>
TNode<FixedArrayBase> CodeStubAssembler::AllocateFixedArray(
    ElementsKind kind, TNode<TIndex> capacity, AllocationFlags flags,
    base::Optional<TNode<Map>> fixed_array_map) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT capacity is allowed");
  Comment("AllocateFixedArray");
  CSA_ASSERT(this,
             IntPtrOrSmiGreaterThan(capacity, IntPtrOrSmiConstant<TIndex>(0)));

  const intptr_t kMaxLength = IsDoubleElementsKind(kind)
                                  ? FixedDoubleArray::kMaxLength
                                  : FixedArray::kMaxLength;
  intptr_t capacity_constant;
  if (ToParameterConstant(capacity, &capacity_constant)) {
    CHECK_LE(capacity_constant, kMaxLength);
  } else {
    Label if_out_of_memory(this, Label::kDeferred), next(this);
    Branch(IntPtrOrSmiGreaterThan(capacity, IntPtrOrSmiConstant<TIndex>(
                                                static_cast<int>(kMaxLength))),
           &if_out_of_memory, &next);

    BIND(&if_out_of_memory);
    CallRuntime(Runtime::kFatalProcessOutOfMemoryInvalidArrayLength,
                NoContextConstant());
    Unreachable();

    BIND(&next);
  }

  TNode<IntPtrT> total_size = GetFixedArrayAllocationSize(capacity, kind);

  if (IsDoubleElementsKind(kind)) flags |= kDoubleAlignment;
  // Allocate both array and elements object, and initialize the JSArray.
  TNode<HeapObject> array = Allocate(total_size, flags);
  if (fixed_array_map) {
    // Conservatively only skip the write barrier if there are no allocation
    // flags, this ensures that the object hasn't ended up in LOS. Note that the
    // fixed array map is currently always immortal and technically wouldn't
    // need the write barrier even in LOS, but it's better to not take chances
    // in case this invariant changes later, since it's difficult to enforce
    // locally here.
    if (flags == CodeStubAssembler::kNone) {
      StoreMapNoWriteBarrier(array, *fixed_array_map);
    } else {
      StoreMap(array, *fixed_array_map);
    }
  } else {
    RootIndex map_index = IsDoubleElementsKind(kind)
                              ? RootIndex::kFixedDoubleArrayMap
                              : RootIndex::kFixedArrayMap;
    DCHECK(RootsTable::IsImmortalImmovable(map_index));
    StoreMapNoWriteBarrier(array, map_index);
  }
  StoreObjectFieldNoWriteBarrier(array, FixedArrayBase::kLengthOffset,
                                 ParameterToTagged(capacity));
  return UncheckedCast<FixedArrayBase>(array);
}

// There is no need to export the Smi version since it is only used inside
// code-stub-assembler.
template V8_EXPORT_PRIVATE TNode<FixedArrayBase>
    CodeStubAssembler::AllocateFixedArray<IntPtrT>(ElementsKind, TNode<IntPtrT>,
                                                   AllocationFlags,
                                                   base::Optional<TNode<Map>>);

template <typename TIndex>
TNode<FixedArray> CodeStubAssembler::ExtractToFixedArray(
    TNode<FixedArrayBase> source, TNode<TIndex> first, TNode<TIndex> count,
    TNode<TIndex> capacity, TNode<Map> source_map, ElementsKind from_kind,
    AllocationFlags allocation_flags, ExtractFixedArrayFlags extract_flags,
    HoleConversionMode convert_holes, TVariable<BoolT>* var_holes_converted,
    base::Optional<TNode<Int32T>> source_elements_kind) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT first, count, and capacity are allowed");

  DCHECK(extract_flags & ExtractFixedArrayFlag::kFixedArrays);
  CSA_ASSERT(this,
             IntPtrOrSmiNotEqual(IntPtrOrSmiConstant<TIndex>(0), capacity));
  CSA_ASSERT(this, TaggedEqual(source_map, LoadMap(source)));

  TVARIABLE(FixedArrayBase, var_result);
  TVARIABLE(Map, var_target_map, source_map);

  Label done(this, {&var_result}), is_cow(this),
      new_space_check(this, {&var_target_map});

  // If source_map is either FixedDoubleArrayMap, or FixedCOWArrayMap but
  // we can't just use COW, use FixedArrayMap as the target map. Otherwise, use
  // source_map as the target map.
  if (IsDoubleElementsKind(from_kind)) {
    CSA_ASSERT(this, IsFixedDoubleArrayMap(source_map));
    var_target_map = FixedArrayMapConstant();
    Goto(&new_space_check);
  } else {
    CSA_ASSERT(this, Word32BinaryNot(IsFixedDoubleArrayMap(source_map)));
    Branch(TaggedEqual(var_target_map.value(), FixedCOWArrayMapConstant()),
           &is_cow, &new_space_check);

    BIND(&is_cow);
    {
      // |source| is a COW array, so we don't actually need to allocate a new
      // array unless:
      // 1) |extract_flags| forces us to, or
      // 2) we're asked to extract only part of the |source| (|first| != 0).
      if (extract_flags & ExtractFixedArrayFlag::kDontCopyCOW) {
        Branch(IntPtrOrSmiNotEqual(IntPtrOrSmiConstant<TIndex>(0), first),
               &new_space_check, [&] {
                 var_result = source;
                 Goto(&done);
               });
      } else {
        var_target_map = FixedArrayMapConstant();
        Goto(&new_space_check);
      }
    }
  }

  BIND(&new_space_check);
  {
    bool handle_old_space = !FLAG_young_generation_large_objects;
    if (handle_old_space) {
      if (extract_flags & ExtractFixedArrayFlag::kNewSpaceAllocationOnly) {
        handle_old_space = false;
        CSA_ASSERT(this, Word32BinaryNot(FixedArraySizeDoesntFitInNewSpace(
                             count, FixedArray::kHeaderSize)));
      } else {
        int constant_count;
        handle_old_space =
            !TryGetIntPtrOrSmiConstantValue(count, &constant_count) ||
            (constant_count >
             FixedArray::GetMaxLengthForNewSpaceAllocation(PACKED_ELEMENTS));
      }
    }

    Label old_space(this, Label::kDeferred);
    if (handle_old_space) {
      GotoIfFixedArraySizeDoesntFitInNewSpace(capacity, &old_space,
                                              FixedArray::kHeaderSize);
    }

    Comment("Copy FixedArray in young generation");
    // We use PACKED_ELEMENTS to tell AllocateFixedArray and
    // CopyFixedArrayElements that we want a FixedArray.
    const ElementsKind to_kind = PACKED_ELEMENTS;
    TNode<FixedArrayBase> to_elements = AllocateFixedArray(
        to_kind, capacity, allocation_flags, var_target_map.value());
    var_result = to_elements;

#ifndef V8_ENABLE_SINGLE_GENERATION
#ifdef DEBUG
    TNode<IntPtrT> object_word = BitcastTaggedToWord(to_elements);
    TNode<IntPtrT> object_page = PageFromAddress(object_word);
    TNode<IntPtrT> page_flags =
        Load<IntPtrT>(object_page, IntPtrConstant(Page::kFlagsOffset));
    CSA_ASSERT(
        this,
        WordNotEqual(
            WordAnd(page_flags,
                    IntPtrConstant(MemoryChunk::kIsInYoungGenerationMask)),
            IntPtrConstant(0)));
#endif
#endif

    if (convert_holes == HoleConversionMode::kDontConvert &&
        !IsDoubleElementsKind(from_kind)) {
      // We can use CopyElements (memcpy) because we don't need to replace or
      // convert any values. Since {to_elements} is in new-space, CopyElements
      // will efficiently use memcpy.
      FillFixedArrayWithValue(to_kind, to_elements, count, capacity,
                              RootIndex::kTheHoleValue);
      CopyElements(to_kind, to_elements, IntPtrConstant(0), source,
                   ParameterToIntPtr(first), ParameterToIntPtr(count),
                   SKIP_WRITE_BARRIER);
    } else {
      CopyFixedArrayElements(from_kind, source, to_kind, to_elements, first,
                             count, capacity, SKIP_WRITE_BARRIER, convert_holes,
                             var_holes_converted);
    }
    Goto(&done);

    if (handle_old_space) {
      BIND(&old_space);
      {
        Comment("Copy FixedArray in old generation");
        Label copy_one_by_one(this);

        // Try to use memcpy if we don't need to convert holes to undefined.
        if (convert_holes == HoleConversionMode::kDontConvert &&
            source_elements_kind) {
          // Only try memcpy if we're not copying object pointers.
          GotoIfNot(IsFastSmiElementsKind(*source_elements_kind),
                    &copy_one_by_one);

          const ElementsKind to_smi_kind = PACKED_SMI_ELEMENTS;
          to_elements = AllocateFixedArray(
              to_smi_kind, capacity, allocation_flags, var_target_map.value());
          var_result = to_elements;

          FillFixedArrayWithValue(to_smi_kind, to_elements, count, capacity,
                                  RootIndex::kTheHoleValue);
          // CopyElements will try to use memcpy if it's not conflicting with
          // GC. Otherwise it will copy elements by elements, but skip write
          // barriers (since we're copying smis to smis).
          CopyElements(to_smi_kind, to_elements, IntPtrConstant(0), source,
                       ParameterToIntPtr(first), ParameterToIntPtr(count),
                       SKIP_WRITE_BARRIER);
          Goto(&done);
        } else {
          Goto(&copy_one_by_one);
        }

        BIND(&copy_one_by_one);
        {
          to_elements = AllocateFixedArray(to_kind, capacity, allocation_flags,
                                           var_target_map.value());
          var_result = to_elements;
          CopyFixedArrayElements(from_kind, source, to_kind, to_elements, first,
                                 count, capacity, UPDATE_WRITE_BARRIER,
                                 convert_holes, var_holes_converted);
          Goto(&done);
        }
      }
    }
  }

  BIND(&done);
  return UncheckedCast<FixedArray>(var_result.value());
}

template <typename TIndex>
TNode<FixedArrayBase> CodeStubAssembler::ExtractFixedDoubleArrayFillingHoles(
    TNode<FixedArrayBase> from_array, TNode<TIndex> first, TNode<TIndex> count,
    TNode<TIndex> capacity, TNode<Map> fixed_array_map,
    TVariable<BoolT>* var_holes_converted, AllocationFlags allocation_flags,
    ExtractFixedArrayFlags extract_flags) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT first, count, and capacity are allowed");

  DCHECK_NE(var_holes_converted, nullptr);
  CSA_ASSERT(this, IsFixedDoubleArrayMap(fixed_array_map));

  TVARIABLE(FixedArrayBase, var_result);
  const ElementsKind kind = PACKED_DOUBLE_ELEMENTS;
  TNode<FixedArrayBase> to_elements =
      AllocateFixedArray(kind, capacity, allocation_flags, fixed_array_map);
  var_result = to_elements;
  // We first try to copy the FixedDoubleArray to a new FixedDoubleArray.
  // |var_holes_converted| is set to False preliminarily.
  *var_holes_converted = Int32FalseConstant();

  // The construction of the loop and the offsets for double elements is
  // extracted from CopyFixedArrayElements.
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKindOrEmpty(from_array, kind));
  STATIC_ASSERT(FixedArray::kHeaderSize == FixedDoubleArray::kHeaderSize);

  Comment("[ ExtractFixedDoubleArrayFillingHoles");

  // This copy can trigger GC, so we pre-initialize the array with holes.
  FillFixedArrayWithValue(kind, to_elements, IntPtrOrSmiConstant<TIndex>(0),
                          capacity, RootIndex::kTheHoleValue);

  const int first_element_offset = FixedArray::kHeaderSize - kHeapObjectTag;
  TNode<IntPtrT> first_from_element_offset =
      ElementOffsetFromIndex(first, kind, 0);
  TNode<IntPtrT> limit_offset = IntPtrAdd(first_from_element_offset,
                                          IntPtrConstant(first_element_offset));
  TVARIABLE(IntPtrT, var_from_offset,
            ElementOffsetFromIndex(IntPtrOrSmiAdd(first, count), kind,
                                   first_element_offset));

  Label decrement(this, {&var_from_offset}), done(this);
  TNode<IntPtrT> to_array_adjusted =
      IntPtrSub(BitcastTaggedToWord(to_elements), first_from_element_offset);

  Branch(WordEqual(var_from_offset.value(), limit_offset), &done, &decrement);

  BIND(&decrement);
  {
    TNode<IntPtrT> from_offset =
        IntPtrSub(var_from_offset.value(), IntPtrConstant(kDoubleSize));
    var_from_offset = from_offset;

    TNode<IntPtrT> to_offset = from_offset;

    Label if_hole(this);

    TNode<Float64T> value = LoadDoubleWithHoleCheck(
        from_array, var_from_offset.value(), &if_hole, MachineType::Float64());

    StoreNoWriteBarrier(MachineRepresentation::kFloat64, to_array_adjusted,
                        to_offset, value);

    TNode<BoolT> compare = WordNotEqual(from_offset, limit_offset);
    Branch(compare, &decrement, &done);

    BIND(&if_hole);
    // We are unlucky: there are holes! We need to restart the copy, this time
    // we will copy the FixedDoubleArray to a new FixedArray with undefined
    // replacing holes. We signal this to the caller through
    // |var_holes_converted|.
    *var_holes_converted = Int32TrueConstant();
    to_elements =
        ExtractToFixedArray(from_array, first, count, capacity, fixed_array_map,
                            kind, allocation_flags, extract_flags,
                            HoleConversionMode::kConvertToUndefined);
    var_result = to_elements;
    Goto(&done);
  }

  BIND(&done);
  Comment("] ExtractFixedDoubleArrayFillingHoles");
  return var_result.value();
}

template <typename TIndex>
TNode<FixedArrayBase> CodeStubAssembler::ExtractFixedArray(
    TNode<FixedArrayBase> source, base::Optional<TNode<TIndex>> first,
    base::Optional<TNode<TIndex>> count, base::Optional<TNode<TIndex>> capacity,
    ExtractFixedArrayFlags extract_flags, TVariable<BoolT>* var_holes_converted,
    base::Optional<TNode<Int32T>> source_elements_kind) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT first, count, and capacity are allowed");
  DCHECK(extract_flags & ExtractFixedArrayFlag::kFixedArrays ||
         extract_flags & ExtractFixedArrayFlag::kFixedDoubleArrays);
  // If we want to replace holes, ExtractFixedArrayFlag::kDontCopyCOW should
  // not be used, because that disables the iteration which detects holes.
  DCHECK_IMPLIES(var_holes_converted != nullptr,
                 !(extract_flags & ExtractFixedArrayFlag::kDontCopyCOW));
  HoleConversionMode convert_holes =
      var_holes_converted != nullptr ? HoleConversionMode::kConvertToUndefined
                                     : HoleConversionMode::kDontConvert;
  TVARIABLE(FixedArrayBase, var_result);
  const AllocationFlags allocation_flags =
      (extract_flags & ExtractFixedArrayFlag::kNewSpaceAllocationOnly)
          ? CodeStubAssembler::kNone
          : CodeStubAssembler::kAllowLargeObjectAllocation;
  if (!first) {
    first = IntPtrOrSmiConstant<TIndex>(0);
  }
  if (!count) {
    count = IntPtrOrSmiSub(
        TaggedToParameter<TIndex>(LoadFixedArrayBaseLength(source)), *first);

    CSA_ASSERT(this, IntPtrOrSmiLessThanOrEqual(IntPtrOrSmiConstant<TIndex>(0),
                                                *count));
  }
  if (!capacity) {
    capacity = *count;
  } else {
    CSA_ASSERT(this, Word32BinaryNot(IntPtrOrSmiGreaterThan(
                         IntPtrOrSmiAdd(*first, *count), *capacity)));
  }

  Label if_fixed_double_array(this), empty(this), done(this, &var_result);
  TNode<Map> source_map = LoadMap(source);
  GotoIf(IntPtrOrSmiEqual(IntPtrOrSmiConstant<TIndex>(0), *capacity), &empty);

  if (extract_flags & ExtractFixedArrayFlag::kFixedDoubleArrays) {
    if (extract_flags & ExtractFixedArrayFlag::kFixedArrays) {
      GotoIf(IsFixedDoubleArrayMap(source_map), &if_fixed_double_array);
    } else {
      CSA_ASSERT(this, IsFixedDoubleArrayMap(source_map));
    }
  }

  if (extract_flags & ExtractFixedArrayFlag::kFixedArrays) {
    // Here we can only get |source| as FixedArray, never FixedDoubleArray.
    // PACKED_ELEMENTS is used to signify that the source is a FixedArray.
    TNode<FixedArray> to_elements = ExtractToFixedArray(
        source, *first, *count, *capacity, source_map, PACKED_ELEMENTS,
        allocation_flags, extract_flags, convert_holes, var_holes_converted,
        source_elements_kind);
    var_result = to_elements;
    Goto(&done);
  }

  if (extract_flags & ExtractFixedArrayFlag::kFixedDoubleArrays) {
    BIND(&if_fixed_double_array);
    Comment("Copy FixedDoubleArray");

    if (convert_holes == HoleConversionMode::kConvertToUndefined) {
      TNode<FixedArrayBase> to_elements = ExtractFixedDoubleArrayFillingHoles(
          source, *first, *count, *capacity, source_map, var_holes_converted,
          allocation_flags, extract_flags);
      var_result = to_elements;
    } else {
      // We use PACKED_DOUBLE_ELEMENTS to signify that both the source and
      // the target are FixedDoubleArray. That it is PACKED or HOLEY does not
      // matter.
      ElementsKind kind = PACKED_DOUBLE_ELEMENTS;
      TNode<FixedArrayBase> to_elements =
          AllocateFixedArray(kind, *capacity, allocation_flags, source_map);
      FillFixedArrayWithValue(kind, to_elements, *count, *capacity,
                              RootIndex::kTheHoleValue);
      CopyElements(kind, to_elements, IntPtrConstant(0), source,
                   ParameterToIntPtr(*first), ParameterToIntPtr(*count));
      var_result = to_elements;
    }

    Goto(&done);
  }

  BIND(&empty);
  {
    Comment("Copy empty array");

    var_result = EmptyFixedArrayConstant();
    Goto(&done);
  }

  BIND(&done);
  return var_result.value();
}

template V8_EXPORT_PRIVATE TNode<FixedArrayBase>
CodeStubAssembler::ExtractFixedArray<Smi>(
    TNode<FixedArrayBase>, base::Optional<TNode<Smi>>,
    base::Optional<TNode<Smi>>, base::Optional<TNode<Smi>>,
    ExtractFixedArrayFlags, TVariable<BoolT>*, base::Optional<TNode<Int32T>>);

template V8_EXPORT_PRIVATE TNode<FixedArrayBase>
CodeStubAssembler::ExtractFixedArray<IntPtrT>(
    TNode<FixedArrayBase>, base::Optional<TNode<IntPtrT>>,
    base::Optional<TNode<IntPtrT>>, base::Optional<TNode<IntPtrT>>,
    ExtractFixedArrayFlags, TVariable<BoolT>*, base::Optional<TNode<Int32T>>);

void CodeStubAssembler::InitializePropertyArrayLength(
    TNode<PropertyArray> property_array, TNode<IntPtrT> length) {
  CSA_ASSERT(this, IntPtrGreaterThan(length, IntPtrConstant(0)));
  CSA_ASSERT(this,
             IntPtrLessThanOrEqual(
                 length, IntPtrConstant(PropertyArray::LengthField::kMax)));
  StoreObjectFieldNoWriteBarrier(
      property_array, PropertyArray::kLengthAndHashOffset, SmiTag(length));
}

TNode<PropertyArray> CodeStubAssembler::AllocatePropertyArray(
    TNode<IntPtrT> capacity) {
  CSA_ASSERT(this, IntPtrGreaterThan(capacity, IntPtrConstant(0)));
  TNode<IntPtrT> total_size = GetPropertyArrayAllocationSize(capacity);

  TNode<HeapObject> array = Allocate(total_size, kNone);
  RootIndex map_index = RootIndex::kPropertyArrayMap;
  DCHECK(RootsTable::IsImmortalImmovable(map_index));
  StoreMapNoWriteBarrier(array, map_index);
  TNode<PropertyArray> property_array = CAST(array);
  InitializePropertyArrayLength(property_array, capacity);
  return property_array;
}

void CodeStubAssembler::FillPropertyArrayWithUndefined(
    TNode<PropertyArray> array, TNode<IntPtrT> from_index,
    TNode<IntPtrT> to_index) {
  ElementsKind kind = PACKED_ELEMENTS;
  TNode<Oddball> value = UndefinedConstant();
  BuildFastArrayForEach(
      array, kind, from_index, to_index,
      [this, value](TNode<HeapObject> array, TNode<IntPtrT> offset) {
        StoreNoWriteBarrier(MachineRepresentation::kTagged, array, offset,
                            value);
      });
}

template <typename TIndex>
void CodeStubAssembler::FillFixedArrayWithValue(ElementsKind kind,
                                                TNode<FixedArrayBase> array,
                                                TNode<TIndex> from_index,
                                                TNode<TIndex> to_index,
                                                RootIndex value_root_index) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT from and to are allowed");
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKind(array, kind));
  DCHECK(value_root_index == RootIndex::kTheHoleValue ||
         value_root_index == RootIndex::kUndefinedValue);

  // Determine the value to initialize the {array} based
  // on the {value_root_index} and the elements {kind}.
  TNode<Object> value = LoadRoot(value_root_index);
  TNode<Float64T> float_value;
  if (IsDoubleElementsKind(kind)) {
    float_value = LoadHeapNumberValue(CAST(value));
  }

  BuildFastArrayForEach(
      array, kind, from_index, to_index,
      [this, value, float_value, kind](TNode<HeapObject> array,
                                       TNode<IntPtrT> offset) {
        if (IsDoubleElementsKind(kind)) {
          StoreNoWriteBarrier(MachineRepresentation::kFloat64, array, offset,
                              float_value);
        } else {
          StoreNoWriteBarrier(MachineRepresentation::kTagged, array, offset,
                              value);
        }
      });
}

template V8_EXPORT_PRIVATE void
    CodeStubAssembler::FillFixedArrayWithValue<IntPtrT>(ElementsKind,
                                                        TNode<FixedArrayBase>,
                                                        TNode<IntPtrT>,
                                                        TNode<IntPtrT>,
                                                        RootIndex);
template V8_EXPORT_PRIVATE void CodeStubAssembler::FillFixedArrayWithValue<Smi>(
    ElementsKind, TNode<FixedArrayBase>, TNode<Smi>, TNode<Smi>, RootIndex);

void CodeStubAssembler::StoreDoubleHole(TNode<HeapObject> object,
                                        TNode<IntPtrT> offset) {
  TNode<UintPtrT> double_hole =
      Is64() ? ReinterpretCast<UintPtrT>(Int64Constant(kHoleNanInt64))
             : ReinterpretCast<UintPtrT>(Int32Constant(kHoleNanLower32));
  // TODO(danno): When we have a Float32/Float64 wrapper class that
  // preserves double bits during manipulation, remove this code/change
  // this to an indexed Float64 store.
  if (Is64()) {
    StoreNoWriteBarrier(MachineRepresentation::kWord64, object, offset,
                        double_hole);
  } else {
    StoreNoWriteBarrier(MachineRepresentation::kWord32, object, offset,
                        double_hole);
    StoreNoWriteBarrier(MachineRepresentation::kWord32, object,
                        IntPtrAdd(offset, IntPtrConstant(kInt32Size)),
                        double_hole);
  }
}

void CodeStubAssembler::StoreFixedDoubleArrayHole(TNode<FixedDoubleArray> array,
                                                  TNode<IntPtrT> index) {
  TNode<IntPtrT> offset = ElementOffsetFromIndex(
      index, PACKED_DOUBLE_ELEMENTS, FixedArray::kHeaderSize - kHeapObjectTag);
  CSA_ASSERT(this, IsOffsetInBounds(
                       offset, LoadAndUntagFixedArrayBaseLength(array),
                       FixedDoubleArray::kHeaderSize, PACKED_DOUBLE_ELEMENTS));
  StoreDoubleHole(array, offset);
}

void CodeStubAssembler::FillFixedArrayWithSmiZero(TNode<FixedArray> array,
                                                  TNode<IntPtrT> length) {
  CSA_ASSERT(this, WordEqual(length, LoadAndUntagFixedArrayBaseLength(array)));

  TNode<IntPtrT> byte_length = TimesTaggedSize(length);
  CSA_ASSERT(this, UintPtrLessThan(length, byte_length));

  static const int32_t fa_base_data_offset =
      FixedArray::kHeaderSize - kHeapObjectTag;
  TNode<IntPtrT> backing_store = IntPtrAdd(BitcastTaggedToWord(array),
                                           IntPtrConstant(fa_base_data_offset));

  // Call out to memset to perform initialization.
  TNode<ExternalReference> memset =
      ExternalConstant(ExternalReference::libc_memset_function());
  STATIC_ASSERT(kSizetSize == kIntptrSize);
  CallCFunction(memset, MachineType::Pointer(),
                std::make_pair(MachineType::Pointer(), backing_store),
                std::make_pair(MachineType::IntPtr(), IntPtrConstant(0)),
                std::make_pair(MachineType::UintPtr(), byte_length));
}

void CodeStubAssembler::FillFixedDoubleArrayWithZero(
    TNode<FixedDoubleArray> array, TNode<IntPtrT> length) {
  CSA_ASSERT(this, WordEqual(length, LoadAndUntagFixedArrayBaseLength(array)));

  TNode<IntPtrT> byte_length = TimesDoubleSize(length);
  CSA_ASSERT(this, UintPtrLessThan(length, byte_length));

  static const int32_t fa_base_data_offset =
      FixedDoubleArray::kHeaderSize - kHeapObjectTag;
  TNode<IntPtrT> backing_store = IntPtrAdd(BitcastTaggedToWord(array),
                                           IntPtrConstant(fa_base_data_offset));

  // Call out to memset to perform initialization.
  TNode<ExternalReference> memset =
      ExternalConstant(ExternalReference::libc_memset_function());
  STATIC_ASSERT(kSizetSize == kIntptrSize);
  CallCFunction(memset, MachineType::Pointer(),
                std::make_pair(MachineType::Pointer(), backing_store),
                std::make_pair(MachineType::IntPtr(), IntPtrConstant(0)),
                std::make_pair(MachineType::UintPtr(), byte_length));
}

void CodeStubAssembler::JumpIfPointersFromHereAreInteresting(
    TNode<Object> object, Label* interesting) {
  Label finished(this);
  TNode<IntPtrT> object_word = BitcastTaggedToWord(object);
  TNode<IntPtrT> object_page = PageFromAddress(object_word);
  TNode<IntPtrT> page_flags = UncheckedCast<IntPtrT>(Load(
      MachineType::IntPtr(), object_page, IntPtrConstant(Page::kFlagsOffset)));
  Branch(
      WordEqual(WordAnd(page_flags,
                        IntPtrConstant(
                            MemoryChunk::kPointersFromHereAreInterestingMask)),
                IntPtrConstant(0)),
      &finished, interesting);
  BIND(&finished);
}

void CodeStubAssembler::MoveElements(ElementsKind kind,
                                     TNode<FixedArrayBase> elements,
                                     TNode<IntPtrT> dst_index,
                                     TNode<IntPtrT> src_index,
                                     TNode<IntPtrT> length) {
  Label finished(this);
  Label needs_barrier(this);
  const bool needs_barrier_check = !IsDoubleElementsKind(kind);

  DCHECK(IsFastElementsKind(kind));
  CSA_ASSERT(this, IsFixedArrayWithKind(elements, kind));
  CSA_ASSERT(this,
             IntPtrLessThanOrEqual(IntPtrAdd(dst_index, length),
                                   LoadAndUntagFixedArrayBaseLength(elements)));
  CSA_ASSERT(this,
             IntPtrLessThanOrEqual(IntPtrAdd(src_index, length),
                                   LoadAndUntagFixedArrayBaseLength(elements)));

  // The write barrier can be ignored if {dst_elements} is in new space, or if
  // the elements pointer is FixedDoubleArray.
  if (needs_barrier_check) {
    JumpIfPointersFromHereAreInteresting(elements, &needs_barrier);
  }

  const TNode<IntPtrT> source_byte_length =
      IntPtrMul(length, IntPtrConstant(ElementsKindToByteSize(kind)));
  static const int32_t fa_base_data_offset =
      FixedArrayBase::kHeaderSize - kHeapObjectTag;
  TNode<IntPtrT> elements_intptr = BitcastTaggedToWord(elements);
  TNode<IntPtrT> target_data_ptr =
      IntPtrAdd(elements_intptr,
                ElementOffsetFromIndex(dst_index, kind, fa_base_data_offset));
  TNode<IntPtrT> source_data_ptr =
      IntPtrAdd(elements_intptr,
                ElementOffsetFromIndex(src_index, kind, fa_base_data_offset));
  TNode<ExternalReference> memmove =
      ExternalConstant(ExternalReference::libc_memmove_function());
  CallCFunction(memmove, MachineType::Pointer(),
                std::make_pair(MachineType::Pointer(), target_data_ptr),
                std::make_pair(MachineType::Pointer(), source_data_ptr),
                std::make_pair(MachineType::UintPtr(), source_byte_length));

  if (needs_barrier_check) {
    Goto(&finished);

    BIND(&needs_barrier);
    {
      const TNode<IntPtrT> begin = src_index;
      const TNode<IntPtrT> end = IntPtrAdd(begin, length);

      // If dst_index is less than src_index, then walk forward.
      const TNode<IntPtrT> delta =
          IntPtrMul(IntPtrSub(dst_index, begin),
                    IntPtrConstant(ElementsKindToByteSize(kind)));
      auto loop_body = [&](TNode<HeapObject> array, TNode<IntPtrT> offset) {
        const TNode<AnyTaggedT> element = Load<AnyTaggedT>(array, offset);
        const TNode<WordT> delta_offset = IntPtrAdd(offset, delta);
        Store(array, delta_offset, element);
      };

      Label iterate_forward(this);
      Label iterate_backward(this);
      Branch(IntPtrLessThan(delta, IntPtrConstant(0)), &iterate_forward,
             &iterate_backward);
      BIND(&iterate_forward);
      {
        // Make a loop for the stores.
        BuildFastArrayForEach(elements, kind, begin, end, loop_body,
                              ForEachDirection::kForward);
        Goto(&finished);
      }

      BIND(&iterate_backward);
      {
        BuildFastArrayForEach(elements, kind, begin, end, loop_body,
                              ForEachDirection::kReverse);
        Goto(&finished);
      }
    }
    BIND(&finished);
  }
}

void CodeStubAssembler::CopyElements(ElementsKind kind,
                                     TNode<FixedArrayBase> dst_elements,
                                     TNode<IntPtrT> dst_index,
                                     TNode<FixedArrayBase> src_elements,
                                     TNode<IntPtrT> src_index,
                                     TNode<IntPtrT> length,
                                     WriteBarrierMode write_barrier) {
  Label finished(this);
  Label needs_barrier(this);
  const bool needs_barrier_check = !IsDoubleElementsKind(kind);

  DCHECK(IsFastElementsKind(kind));
  CSA_ASSERT(this, IsFixedArrayWithKind(dst_elements, kind));
  CSA_ASSERT(this, IsFixedArrayWithKind(src_elements, kind));
  CSA_ASSERT(this, IntPtrLessThanOrEqual(
                       IntPtrAdd(dst_index, length),
                       LoadAndUntagFixedArrayBaseLength(dst_elements)));
  CSA_ASSERT(this, IntPtrLessThanOrEqual(
                       IntPtrAdd(src_index, length),
                       LoadAndUntagFixedArrayBaseLength(src_elements)));
  CSA_ASSERT(this, Word32Or(TaggedNotEqual(dst_elements, src_elements),
                            IntPtrEqual(length, IntPtrConstant(0))));

  // The write barrier can be ignored if {dst_elements} is in new space, or if
  // the elements pointer is FixedDoubleArray.
  if (needs_barrier_check) {
    JumpIfPointersFromHereAreInteresting(dst_elements, &needs_barrier);
  }

  TNode<IntPtrT> source_byte_length =
      IntPtrMul(length, IntPtrConstant(ElementsKindToByteSize(kind)));
  static const int32_t fa_base_data_offset =
      FixedArrayBase::kHeaderSize - kHeapObjectTag;
  TNode<IntPtrT> src_offset_start =
      ElementOffsetFromIndex(src_index, kind, fa_base_data_offset);
  TNode<IntPtrT> dst_offset_start =
      ElementOffsetFromIndex(dst_index, kind, fa_base_data_offset);
  TNode<IntPtrT> src_elements_intptr = BitcastTaggedToWord(src_elements);
  TNode<IntPtrT> source_data_ptr =
      IntPtrAdd(src_elements_intptr, src_offset_start);
  TNode<IntPtrT> dst_elements_intptr = BitcastTaggedToWord(dst_elements);
  TNode<IntPtrT> dst_data_ptr =
      IntPtrAdd(dst_elements_intptr, dst_offset_start);
  TNode<ExternalReference> memcpy =
      ExternalConstant(ExternalReference::libc_memcpy_function());
  CallCFunction(memcpy, MachineType::Pointer(),
                std::make_pair(MachineType::Pointer(), dst_data_ptr),
                std::make_pair(MachineType::Pointer(), source_data_ptr),
                std::make_pair(MachineType::UintPtr(), source_byte_length));

  if (needs_barrier_check) {
    Goto(&finished);

    BIND(&needs_barrier);
    {
      const TNode<IntPtrT> begin = src_index;
      const TNode<IntPtrT> end = IntPtrAdd(begin, length);
      const TNode<IntPtrT> delta =
          IntPtrMul(IntPtrSub(dst_index, src_index),
                    IntPtrConstant(ElementsKindToByteSize(kind)));
      BuildFastArrayForEach(
          src_elements, kind, begin, end,
          [&](TNode<HeapObject> array, TNode<IntPtrT> offset) {
            const TNode<AnyTaggedT> element = Load<AnyTaggedT>(array, offset);
            const TNode<WordT> delta_offset = IntPtrAdd(offset, delta);
            if (write_barrier == SKIP_WRITE_BARRIER) {
              StoreNoWriteBarrier(MachineRepresentation::kTagged, dst_elements,
                                  delta_offset, element);
            } else {
              Store(dst_elements, delta_offset, element);
            }
          },
          ForEachDirection::kForward);
      Goto(&finished);
    }
    BIND(&finished);
  }
}

template <typename TIndex>
void CodeStubAssembler::CopyFixedArrayElements(
    ElementsKind from_kind, TNode<FixedArrayBase> from_array,
    ElementsKind to_kind, TNode<FixedArrayBase> to_array,
    TNode<TIndex> first_element, TNode<TIndex> element_count,
    TNode<TIndex> capacity, WriteBarrierMode barrier_mode,
    HoleConversionMode convert_holes, TVariable<BoolT>* var_holes_converted) {
  DCHECK_IMPLIES(var_holes_converted != nullptr,
                 convert_holes == HoleConversionMode::kConvertToUndefined);
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKindOrEmpty(from_array, from_kind));
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKindOrEmpty(to_array, to_kind));
  STATIC_ASSERT(FixedArray::kHeaderSize == FixedDoubleArray::kHeaderSize);
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT indices are allowed");

  const int first_element_offset = FixedArray::kHeaderSize - kHeapObjectTag;
  Comment("[ CopyFixedArrayElements");

  // Typed array elements are not supported.
  DCHECK(!IsTypedArrayElementsKind(from_kind));
  DCHECK(!IsTypedArrayElementsKind(to_kind));

  Label done(this);
  bool from_double_elements = IsDoubleElementsKind(from_kind);
  bool to_double_elements = IsDoubleElementsKind(to_kind);
  bool doubles_to_objects_conversion =
      IsDoubleElementsKind(from_kind) && IsObjectElementsKind(to_kind);
  bool needs_write_barrier =
      doubles_to_objects_conversion ||
      (barrier_mode == UPDATE_WRITE_BARRIER && IsObjectElementsKind(to_kind));
  bool element_offset_matches =
      !needs_write_barrier &&
      (kTaggedSize == kDoubleSize ||
       IsDoubleElementsKind(from_kind) == IsDoubleElementsKind(to_kind));
  TNode<UintPtrT> double_hole =
      Is64() ? ReinterpretCast<UintPtrT>(Int64Constant(kHoleNanInt64))
             : ReinterpretCast<UintPtrT>(Int32Constant(kHoleNanLower32));

  // If copying might trigger a GC, we pre-initialize the FixedArray such that
  // it's always in a consistent state.
  if (convert_holes == HoleConversionMode::kConvertToUndefined) {
    DCHECK(IsObjectElementsKind(to_kind));
    // Use undefined for the part that we copy and holes for the rest.
    // Later if we run into a hole in the source we can just skip the writing
    // to the target and are still guaranteed that we get an undefined.
    FillFixedArrayWithValue(to_kind, to_array, IntPtrOrSmiConstant<TIndex>(0),
                            element_count, RootIndex::kUndefinedValue);
    FillFixedArrayWithValue(to_kind, to_array, element_count, capacity,
                            RootIndex::kTheHoleValue);
  } else if (doubles_to_objects_conversion) {
    // Pre-initialized the target with holes so later if we run into a hole in
    // the source we can just skip the writing to the target.
    FillFixedArrayWithValue(to_kind, to_array, IntPtrOrSmiConstant<TIndex>(0),
                            capacity, RootIndex::kTheHoleValue);
  } else if (element_count != capacity) {
    FillFixedArrayWithValue(to_kind, to_array, element_count, capacity,
                            RootIndex::kTheHoleValue);
  }

  TNode<IntPtrT> first_from_element_offset =
      ElementOffsetFromIndex(first_element, from_kind, 0);
  TNode<IntPtrT> limit_offset = Signed(IntPtrAdd(
      first_from_element_offset, IntPtrConstant(first_element_offset)));
  TVARIABLE(IntPtrT, var_from_offset,
            ElementOffsetFromIndex(IntPtrOrSmiAdd(first_element, element_count),
                                   from_kind, first_element_offset));
  // This second variable is used only when the element sizes of source and
  // destination arrays do not match.
  TVARIABLE(IntPtrT, var_to_offset);
  if (element_offset_matches) {
    var_to_offset = var_from_offset.value();
  } else {
    var_to_offset =
        ElementOffsetFromIndex(element_count, to_kind, first_element_offset);
  }

  VariableList vars({&var_from_offset, &var_to_offset}, zone());
  if (var_holes_converted != nullptr) vars.push_back(var_holes_converted);
  Label decrement(this, vars);

  TNode<IntPtrT> to_array_adjusted =
      element_offset_matches
          ? IntPtrSub(BitcastTaggedToWord(to_array), first_from_element_offset)
          : ReinterpretCast<IntPtrT>(to_array);

  Branch(WordEqual(var_from_offset.value(), limit_offset), &done, &decrement);

  BIND(&decrement);
  {
    TNode<IntPtrT> from_offset = Signed(IntPtrSub(
        var_from_offset.value(),
        IntPtrConstant(from_double_elements ? kDoubleSize : kTaggedSize)));
    var_from_offset = from_offset;

    TNode<IntPtrT> to_offset;
    if (element_offset_matches) {
      to_offset = from_offset;
    } else {
      to_offset = IntPtrSub(
          var_to_offset.value(),
          IntPtrConstant(to_double_elements ? kDoubleSize : kTaggedSize));
      var_to_offset = to_offset;
    }

    Label next_iter(this), store_double_hole(this), signal_hole(this);
    Label* if_hole;
    if (convert_holes == HoleConversionMode::kConvertToUndefined) {
      // The target elements array is already preinitialized with undefined
      // so we only need to signal that a hole was found and continue the loop.
      if_hole = &signal_hole;
    } else if (doubles_to_objects_conversion) {
      // The target elements array is already preinitialized with holes, so we
      // can just proceed with the next iteration.
      if_hole = &next_iter;
    } else if (IsDoubleElementsKind(to_kind)) {
      if_hole = &store_double_hole;
    } else {
      // In all the other cases don't check for holes and copy the data as is.
      if_hole = nullptr;
    }

    Node* value = LoadElementAndPrepareForStore(
        from_array, var_from_offset.value(), from_kind, to_kind, if_hole);

    if (needs_write_barrier) {
      CHECK_EQ(to_array, to_array_adjusted);
      Store(to_array_adjusted, to_offset, value);
    } else if (to_double_elements) {
      StoreNoWriteBarrier(MachineRepresentation::kFloat64, to_array_adjusted,
                          to_offset, value);
    } else {
      UnsafeStoreNoWriteBarrier(MachineRepresentation::kTagged,
                                to_array_adjusted, to_offset, value);
    }
    Goto(&next_iter);

    if (if_hole == &store_double_hole) {
      BIND(&store_double_hole);
      // Don't use doubles to store the hole double, since manipulating the
      // signaling NaN used for the hole in C++, e.g. with bit_cast, will
      // change its value on ia32 (the x87 stack is used to return values
      // and stores to the stack silently clear the signalling bit).
      //
      // TODO(danno): When we have a Float32/Float64 wrapper class that
      // preserves double bits during manipulation, remove this code/change
      // this to an indexed Float64 store.
      if (Is64()) {
        StoreNoWriteBarrier(MachineRepresentation::kWord64, to_array_adjusted,
                            to_offset, double_hole);
      } else {
        StoreNoWriteBarrier(MachineRepresentation::kWord32, to_array_adjusted,
                            to_offset, double_hole);
        StoreNoWriteBarrier(MachineRepresentation::kWord32, to_array_adjusted,
                            IntPtrAdd(to_offset, IntPtrConstant(kInt32Size)),
                            double_hole);
      }
      Goto(&next_iter);
    } else if (if_hole == &signal_hole) {
      // This case happens only when IsObjectElementsKind(to_kind).
      BIND(&signal_hole);
      if (var_holes_converted != nullptr) {
        *var_holes_converted = Int32TrueConstant();
      }
      Goto(&next_iter);
    }

    BIND(&next_iter);
    TNode<BoolT> compare = WordNotEqual(from_offset, limit_offset);
    Branch(compare, &decrement, &done);
  }

  BIND(&done);
  Comment("] CopyFixedArrayElements");
}

TNode<FixedArray> CodeStubAssembler::HeapObjectToFixedArray(
    TNode<HeapObject> base, Label* cast_fail) {
  Label fixed_array(this);
  TNode<Map> map = LoadMap(base);
  GotoIf(TaggedEqual(map, FixedArrayMapConstant()), &fixed_array);
  GotoIf(TaggedNotEqual(map, FixedCOWArrayMapConstant()), cast_fail);
  Goto(&fixed_array);
  BIND(&fixed_array);
  return UncheckedCast<FixedArray>(base);
}

void CodeStubAssembler::CopyPropertyArrayValues(TNode<HeapObject> from_array,
                                                TNode<PropertyArray> to_array,
                                                TNode<IntPtrT> property_count,
                                                WriteBarrierMode barrier_mode,
                                                DestroySource destroy_source) {
  CSA_SLOW_ASSERT(this, Word32Or(IsPropertyArray(from_array),
                                 IsEmptyFixedArray(from_array)));
  Comment("[ CopyPropertyArrayValues");

  bool needs_write_barrier = barrier_mode == UPDATE_WRITE_BARRIER;

  if (destroy_source == DestroySource::kNo) {
    // PropertyArray may contain mutable HeapNumbers, which will be cloned on
    // the heap, requiring a write barrier.
    needs_write_barrier = true;
  }

  TNode<IntPtrT> start = IntPtrConstant(0);
  ElementsKind kind = PACKED_ELEMENTS;
  BuildFastArrayForEach(
      from_array, kind, start, property_count,
      [this, to_array, needs_write_barrier, destroy_source](
          TNode<HeapObject> array, TNode<IntPtrT> offset) {
        TNode<AnyTaggedT> value = Load<AnyTaggedT>(array, offset);

        if (destroy_source == DestroySource::kNo) {
          value = CloneIfMutablePrimitive(CAST(value));
        }

        if (needs_write_barrier) {
          Store(to_array, offset, value);
        } else {
          StoreNoWriteBarrier(MachineRepresentation::kTagged, to_array, offset,
                              value);
        }
      });

#ifdef DEBUG
  // Zap {from_array} if the copying above has made it invalid.
  if (destroy_source == DestroySource::kYes) {
    Label did_zap(this);
    GotoIf(IsEmptyFixedArray(from_array), &did_zap);
    FillPropertyArrayWithUndefined(CAST(from_array), start, property_count);

    Goto(&did_zap);
    BIND(&did_zap);
  }
#endif
  Comment("] CopyPropertyArrayValues");
}

TNode<FixedArrayBase> CodeStubAssembler::CloneFixedArray(
    TNode<FixedArrayBase> source, ExtractFixedArrayFlags flags) {
  return ExtractFixedArray(
      source, base::Optional<TNode<BInt>>(IntPtrOrSmiConstant<BInt>(0)),
      base::Optional<TNode<BInt>>(base::nullopt),
      base::Optional<TNode<BInt>>(base::nullopt), flags);
}

Node* CodeStubAssembler::LoadElementAndPrepareForStore(
    TNode<FixedArrayBase> array, TNode<IntPtrT> offset, ElementsKind from_kind,
    ElementsKind to_kind, Label* if_hole) {
  CSA_ASSERT(this, IsFixedArrayWithKind(array, from_kind));
  if (IsDoubleElementsKind(from_kind)) {
    TNode<Float64T> value =
        LoadDoubleWithHoleCheck(array, offset, if_hole, MachineType::Float64());
    if (!IsDoubleElementsKind(to_kind)) {
      return AllocateHeapNumberWithValue(value);
    }
    return value;

  } else {
    TNode<Object> value = Load<Object>(array, offset);
    if (if_hole) {
      GotoIf(TaggedEqual(value, TheHoleConstant()), if_hole);
    }
    if (IsDoubleElementsKind(to_kind)) {
      if (IsSmiElementsKind(from_kind)) {
        return SmiToFloat64(CAST(value));
      }
      return LoadHeapNumberValue(CAST(value));
    }
    return value;
  }
}

template <typename TIndex>
TNode<TIndex> CodeStubAssembler::CalculateNewElementsCapacity(
    TNode<TIndex> old_capacity) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT old_capacity is allowed");
  Comment("TryGrowElementsCapacity");
  TNode<TIndex> half_old_capacity = WordOrSmiShr(old_capacity, 1);
  TNode<TIndex> new_capacity = IntPtrOrSmiAdd(half_old_capacity, old_capacity);
  TNode<TIndex> padding =
      IntPtrOrSmiConstant<TIndex>(JSObject::kMinAddedElementsCapacity);
  return IntPtrOrSmiAdd(new_capacity, padding);
}

template V8_EXPORT_PRIVATE TNode<IntPtrT>
    CodeStubAssembler::CalculateNewElementsCapacity<IntPtrT>(TNode<IntPtrT>);
template V8_EXPORT_PRIVATE TNode<Smi>
    CodeStubAssembler::CalculateNewElementsCapacity<Smi>(TNode<Smi>);

TNode<FixedArrayBase> CodeStubAssembler::TryGrowElementsCapacity(
    TNode<HeapObject> object, TNode<FixedArrayBase> elements, ElementsKind kind,
    TNode<Smi> key, Label* bailout) {
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKindOrEmpty(elements, kind));
  TNode<Smi> capacity = LoadFixedArrayBaseLength(elements);

  return TryGrowElementsCapacity(object, elements, kind,
                                 TaggedToParameter<BInt>(key),
                                 TaggedToParameter<BInt>(capacity), bailout);
}

template <typename TIndex>
TNode<FixedArrayBase> CodeStubAssembler::TryGrowElementsCapacity(
    TNode<HeapObject> object, TNode<FixedArrayBase> elements, ElementsKind kind,
    TNode<TIndex> key, TNode<TIndex> capacity, Label* bailout) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT key and capacity nodes are allowed");
  Comment("TryGrowElementsCapacity");
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKindOrEmpty(elements, kind));

  // If the gap growth is too big, fall back to the runtime.
  TNode<TIndex> max_gap = IntPtrOrSmiConstant<TIndex>(JSObject::kMaxGap);
  TNode<TIndex> max_capacity = IntPtrOrSmiAdd(capacity, max_gap);
  GotoIf(UintPtrOrSmiGreaterThanOrEqual(key, max_capacity), bailout);

  // Calculate the capacity of the new backing store.
  TNode<TIndex> new_capacity = CalculateNewElementsCapacity(
      IntPtrOrSmiAdd(key, IntPtrOrSmiConstant<TIndex>(1)));

  return GrowElementsCapacity(object, elements, kind, kind, capacity,
                              new_capacity, bailout);
}

template <typename TIndex>
TNode<FixedArrayBase> CodeStubAssembler::GrowElementsCapacity(
    TNode<HeapObject> object, TNode<FixedArrayBase> elements,
    ElementsKind from_kind, ElementsKind to_kind, TNode<TIndex> capacity,
    TNode<TIndex> new_capacity, Label* bailout) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT capacities are allowed");
  Comment("[ GrowElementsCapacity");
  CSA_SLOW_ASSERT(this, IsFixedArrayWithKindOrEmpty(elements, from_kind));

  // If size of the allocation for the new capacity doesn't fit in a page
  // that we can bump-pointer allocate from, fall back to the runtime.
  int max_size = FixedArrayBase::GetMaxLengthForNewSpaceAllocation(to_kind);
  GotoIf(UintPtrOrSmiGreaterThanOrEqual(new_capacity,
                                        IntPtrOrSmiConstant<TIndex>(max_size)),
         bailout);

  // Allocate the new backing store.
  TNode<FixedArrayBase> new_elements =
      AllocateFixedArray(to_kind, new_capacity);

  // Copy the elements from the old elements store to the new.
  // The size-check above guarantees that the |new_elements| is allocated
  // in new space so we can skip the write barrier.
  CopyFixedArrayElements(from_kind, elements, to_kind, new_elements, capacity,
                         new_capacity, SKIP_WRITE_BARRIER);

  StoreObjectField(object, JSObject::kElementsOffset, new_elements);
  Comment("] GrowElementsCapacity");
  return new_elements;
}

void CodeStubAssembler::InitializeAllocationMemento(
    TNode<HeapObject> base, TNode<IntPtrT> base_allocation_size,
    TNode<AllocationSite> allocation_site) {
  Comment("[Initialize AllocationMemento");
  TNode<HeapObject> memento = InnerAllocate(base, base_allocation_size);
  StoreMapNoWriteBarrier(memento, RootIndex::kAllocationMementoMap);
  StoreObjectFieldNoWriteBarrier(
      memento, AllocationMemento::kAllocationSiteOffset, allocation_site);
  if (FLAG_allocation_site_pretenuring) {
    TNode<Int32T> count = LoadObjectField<Int32T>(
        allocation_site, AllocationSite::kPretenureCreateCountOffset);

    TNode<Int32T> incremented_count = Int32Add(count, Int32Constant(1));
    StoreObjectFieldNoWriteBarrier(allocation_site,
                                   AllocationSite::kPretenureCreateCountOffset,
                                   incremented_count);
  }
  Comment("]");
}

TNode<Float64T> CodeStubAssembler::TryTaggedToFloat64(
    TNode<Object> value, Label* if_valueisnotnumber) {
  return Select<Float64T>(
      TaggedIsSmi(value), [&]() { return SmiToFloat64(CAST(value)); },
      [&]() {
        GotoIfNot(IsHeapNumber(CAST(value)), if_valueisnotnumber);
        return LoadHeapNumberValue(CAST(value));
      });
}

TNode<Float64T> CodeStubAssembler::TruncateTaggedToFloat64(
    TNode<Context> context, SloppyTNode<Object> value) {
  // We might need to loop once due to ToNumber conversion.
  TVARIABLE(Object, var_value, value);
  TVARIABLE(Float64T, var_result);
  Label loop(this, &var_value), done_loop(this, &var_result);
  Goto(&loop);
  BIND(&loop);
  {
    Label if_valueisnotnumber(this, Label::kDeferred);

    // Load the current {value}.
    value = var_value.value();

    // Convert {value} to Float64 if it is a number and convert it to a number
    // otherwise.
    var_result = TryTaggedToFloat64(value, &if_valueisnotnumber);
    Goto(&done_loop);

    BIND(&if_valueisnotnumber);
    {
      // Convert the {value} to a Number first.
      var_value = CallBuiltin(Builtins::kNonNumberToNumber, context, value);
      Goto(&loop);
    }
  }
  BIND(&done_loop);
  return var_result.value();
}

TNode<Word32T> CodeStubAssembler::TruncateTaggedToWord32(
    TNode<Context> context, SloppyTNode<Object> value) {
  TVARIABLE(Word32T, var_result);
  Label done(this);
  TaggedToWord32OrBigIntImpl<Object::Conversion::kToNumber>(context, value,
                                                            &done, &var_result);
  BIND(&done);
  return var_result.value();
}

// Truncate {value} to word32 and jump to {if_number} if it is a Number,
// or find that it is a BigInt and jump to {if_bigint}.
void CodeStubAssembler::TaggedToWord32OrBigInt(
    TNode<Context> context, TNode<Object> value, Label* if_number,
    TVariable<Word32T>* var_word32, Label* if_bigint,
    TVariable<BigInt>* var_maybe_bigint) {
  TaggedToWord32OrBigIntImpl<Object::Conversion::kToNumeric>(
      context, value, if_number, var_word32, if_bigint, var_maybe_bigint);
}

// Truncate {value} to word32 and jump to {if_number} if it is a Number,
// or find that it is a BigInt and jump to {if_bigint}. In either case,
// store the type feedback in {var_feedback}.
void CodeStubAssembler::TaggedToWord32OrBigIntWithFeedback(
    TNode<Context> context, TNode<Object> value, Label* if_number,
    TVariable<Word32T>* var_word32, Label* if_bigint,
    TVariable<BigInt>* var_maybe_bigint, TVariable<Smi>* var_feedback) {
  TaggedToWord32OrBigIntImpl<Object::Conversion::kToNumeric>(
      context, value, if_number, var_word32, if_bigint, var_maybe_bigint,
      var_feedback);
}

template <Object::Conversion conversion>
void CodeStubAssembler::TaggedToWord32OrBigIntImpl(
    TNode<Context> context, TNode<Object> value, Label* if_number,
    TVariable<Word32T>* var_word32, Label* if_bigint,
    TVariable<BigInt>* var_maybe_bigint, TVariable<Smi>* var_feedback) {
  // We might need to loop after conversion.
  TVARIABLE(Object, var_value, value);
  OverwriteFeedback(var_feedback, BinaryOperationFeedback::kNone);
  VariableList loop_vars({&var_value}, zone());
  if (var_feedback != nullptr) loop_vars.push_back(var_feedback);
  Label loop(this, loop_vars);
  Goto(&loop);
  BIND(&loop);
  {
    value = var_value.value();
    Label not_smi(this), is_heap_number(this), is_oddball(this),
        is_bigint(this);
    GotoIf(TaggedIsNotSmi(value), &not_smi);

    // {value} is a Smi.
    *var_word32 = SmiToInt32(CAST(value));
    CombineFeedback(var_feedback, BinaryOperationFeedback::kSignedSmall);
    Goto(if_number);

    BIND(&not_smi);
    TNode<HeapObject> value_heap_object = CAST(value);
    TNode<Map> map = LoadMap(value_heap_object);
    GotoIf(IsHeapNumberMap(map), &is_heap_number);
    TNode<Uint16T> instance_type = LoadMapInstanceType(map);
    if (conversion == Object::Conversion::kToNumeric) {
      GotoIf(IsBigIntInstanceType(instance_type), &is_bigint);
    }

    // Not HeapNumber (or BigInt if conversion == kToNumeric).
    {
      if (var_feedback != nullptr) {
        // We do not require an Or with earlier feedback here because once we
        // convert the value to a Numeric, we cannot reach this path. We can
        // only reach this path on the first pass when the feedback is kNone.
        CSA_ASSERT(this, SmiEqual(var_feedback->value(),
                                  SmiConstant(BinaryOperationFeedback::kNone)));
      }
      GotoIf(InstanceTypeEqual(instance_type, ODDBALL_TYPE), &is_oddball);
      // Not an oddball either -> convert.
      auto builtin = conversion == Object::Conversion::kToNumeric
                         ? Builtins::kNonNumberToNumeric
                         : Builtins::kNonNumberToNumber;
      var_value = CallBuiltin(builtin, context, value);
      OverwriteFeedback(var_feedback, BinaryOperationFeedback::kAny);
      Goto(&loop);

      BIND(&is_oddball);
      var_value = LoadObjectField(value_heap_object, Oddball::kToNumberOffset);
      OverwriteFeedback(var_feedback,
                        BinaryOperationFeedback::kNumberOrOddball);
      Goto(&loop);
    }

    BIND(&is_heap_number);
    *var_word32 = TruncateHeapNumberValueToWord32(CAST(value));
    CombineFeedback(var_feedback, BinaryOperationFeedback::kNumber);
    Goto(if_number);

    if (conversion == Object::Conversion::kToNumeric) {
      BIND(&is_bigint);
      *var_maybe_bigint = CAST(value);
      CombineFeedback(var_feedback, BinaryOperationFeedback::kBigInt);
      Goto(if_bigint);
    }
  }
}

TNode<Int32T> CodeStubAssembler::TruncateNumberToWord32(TNode<Number> number) {
  TVARIABLE(Int32T, var_result);
  Label done(this), if_heapnumber(this);
  GotoIfNot(TaggedIsSmi(number), &if_heapnumber);
  var_result = SmiToInt32(CAST(number));
  Goto(&done);

  BIND(&if_heapnumber);
  TNode<Float64T> value = LoadHeapNumberValue(CAST(number));
  var_result = Signed(TruncateFloat64ToWord32(value));
  Goto(&done);

  BIND(&done);
  return var_result.value();
}

TNode<Int32T> CodeStubAssembler::TruncateHeapNumberValueToWord32(
    TNode<HeapNumber> object) {
  TNode<Float64T> value = LoadHeapNumberValue(object);
  return Signed(TruncateFloat64ToWord32(value));
}

void CodeStubAssembler::TryHeapNumberToSmi(TNode<HeapNumber> number,
                                           TVariable<Smi>* var_result_smi,
                                           Label* if_smi) {
  TNode<Float64T> value = LoadHeapNumberValue(number);
  TryFloat64ToSmi(value, var_result_smi, if_smi);
}

void CodeStubAssembler::TryFloat32ToSmi(TNode<Float32T> value,
                                        TVariable<Smi>* var_result_smi,
                                        Label* if_smi) {
  TNode<Int32T> ivalue = TruncateFloat32ToInt32(value);
  TNode<Float32T> fvalue = RoundInt32ToFloat32(ivalue);

  Label if_int32(this), if_heap_number(this);

  GotoIfNot(Float32Equal(value, fvalue), &if_heap_number);
  GotoIfNot(Word32Equal(ivalue, Int32Constant(0)), &if_int32);
  Branch(Int32LessThan(UncheckedCast<Int32T>(BitcastFloat32ToInt32(value)),
                       Int32Constant(0)),
         &if_heap_number, &if_int32);

  TVARIABLE(Number, var_result);
  BIND(&if_int32);
  {
    if (SmiValuesAre32Bits()) {
      *var_result_smi = SmiTag(ChangeInt32ToIntPtr(ivalue));
    } else {
      DCHECK(SmiValuesAre31Bits());
      TNode<PairT<Int32T, BoolT>> pair = Int32AddWithOverflow(ivalue, ivalue);
      TNode<BoolT> overflow = Projection<1>(pair);
      GotoIf(overflow, &if_heap_number);
      *var_result_smi =
          BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(Projection<0>(pair)));
    }
    Goto(if_smi);
  }
  BIND(&if_heap_number);
}

void CodeStubAssembler::TryFloat64ToSmi(TNode<Float64T> value,
                                        TVariable<Smi>* var_result_smi,
                                        Label* if_smi) {
  TNode<Int32T> value32 = RoundFloat64ToInt32(value);
  TNode<Float64T> value64 = ChangeInt32ToFloat64(value32);

  Label if_int32(this), if_heap_number(this, Label::kDeferred);

  GotoIfNot(Float64Equal(value, value64), &if_heap_number);
  GotoIfNot(Word32Equal(value32, Int32Constant(0)), &if_int32);
  Branch(Int32LessThan(UncheckedCast<Int32T>(Float64ExtractHighWord32(value)),
                       Int32Constant(0)),
         &if_heap_number, &if_int32);

  TVARIABLE(Number, var_result);
  BIND(&if_int32);
  {
    if (SmiValuesAre32Bits()) {
      *var_result_smi = SmiTag(ChangeInt32ToIntPtr(value32));
    } else {
      DCHECK(SmiValuesAre31Bits());
      TNode<PairT<Int32T, BoolT>> pair = Int32AddWithOverflow(value32, value32);
      TNode<BoolT> overflow = Projection<1>(pair);
      GotoIf(overflow, &if_heap_number);
      *var_result_smi =
          BitcastWordToTaggedSigned(ChangeInt32ToIntPtr(Projection<0>(pair)));
    }
    Goto(if_smi);
  }
  BIND(&if_heap_number);
}

TNode<Number> CodeStubAssembler::ChangeFloat32ToTagged(TNode<Float32T> value) {
  Label if_smi(this), done(this);
  TVARIABLE(Smi, var_smi_result);
  TVARIABLE(Number, var_result);
  TryFloat32ToSmi(value, &var_smi_result, &if_smi);

  var_result = AllocateHeapNumberWithValue(ChangeFloat32ToFloat64(value));
  Goto(&done);

  BIND(&if_smi);
  {
    var_result = var_smi_result.value();
    Goto(&done);
  }
  BIND(&done);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::ChangeFloat64ToTagged(
    SloppyTNode<Float64T> value) {
  Label if_smi(this), done(this);
  TVARIABLE(Smi, var_smi_result);
  TVARIABLE(Number, var_result);
  TryFloat64ToSmi(value, &var_smi_result, &if_smi);

  var_result = AllocateHeapNumberWithValue(value);
  Goto(&done);

  BIND(&if_smi);
  {
    var_result = var_smi_result.value();
    Goto(&done);
  }
  BIND(&done);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::ChangeInt32ToTagged(
    SloppyTNode<Int32T> value) {
  if (SmiValuesAre32Bits()) {
    return SmiTag(ChangeInt32ToIntPtr(value));
  }
  DCHECK(SmiValuesAre31Bits());
  TVARIABLE(Number, var_result);
  TNode<PairT<Int32T, BoolT>> pair = Int32AddWithOverflow(value, value);
  TNode<BoolT> overflow = Projection<1>(pair);
  Label if_overflow(this, Label::kDeferred), if_notoverflow(this),
      if_join(this);
  Branch(overflow, &if_overflow, &if_notoverflow);
  BIND(&if_overflow);
  {
    TNode<Float64T> value64 = ChangeInt32ToFloat64(value);
    TNode<HeapNumber> result = AllocateHeapNumberWithValue(value64);
    var_result = result;
    Goto(&if_join);
  }
  BIND(&if_notoverflow);
  {
    TNode<IntPtrT> almost_tagged_value =
        ChangeInt32ToIntPtr(Projection<0>(pair));
    TNode<Smi> result = BitcastWordToTaggedSigned(almost_tagged_value);
    var_result = result;
    Goto(&if_join);
  }
  BIND(&if_join);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::ChangeUint32ToTagged(
    SloppyTNode<Uint32T> value) {
  Label if_overflow(this, Label::kDeferred), if_not_overflow(this),
      if_join(this);
  TVARIABLE(Number, var_result);
  // If {value} > 2^31 - 1, we need to store it in a HeapNumber.
  Branch(Uint32LessThan(Uint32Constant(Smi::kMaxValue), value), &if_overflow,
         &if_not_overflow);

  BIND(&if_not_overflow);
  {
    // The {value} is definitely in valid Smi range.
    var_result = SmiTag(Signed(ChangeUint32ToWord(value)));
  }
  Goto(&if_join);

  BIND(&if_overflow);
  {
    TNode<Float64T> float64_value = ChangeUint32ToFloat64(value);
    var_result = AllocateHeapNumberWithValue(float64_value);
  }
  Goto(&if_join);

  BIND(&if_join);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::ChangeUintPtrToTagged(TNode<UintPtrT> value) {
  Label if_overflow(this, Label::kDeferred), if_not_overflow(this),
      if_join(this);
  TVARIABLE(Number, var_result);
  // If {value} > 2^31 - 1, we need to store it in a HeapNumber.
  Branch(UintPtrLessThan(UintPtrConstant(Smi::kMaxValue), value), &if_overflow,
         &if_not_overflow);

  BIND(&if_not_overflow);
  {
    // The {value} is definitely in valid Smi range.
    var_result = SmiTag(Signed(value));
  }
  Goto(&if_join);

  BIND(&if_overflow);
  {
    TNode<Float64T> float64_value = ChangeUintPtrToFloat64(value);
    var_result = AllocateHeapNumberWithValue(float64_value);
  }
  Goto(&if_join);

  BIND(&if_join);
  return var_result.value();
}

TNode<String> CodeStubAssembler::ToThisString(TNode<Context> context,
                                              TNode<Object> value,
                                              TNode<String> method_name) {
  TVARIABLE(Object, var_value, value);

  // Check if the {value} is a Smi or a HeapObject.
  Label if_valueissmi(this, Label::kDeferred), if_valueisnotsmi(this),
      if_valueisstring(this);
  Branch(TaggedIsSmi(value), &if_valueissmi, &if_valueisnotsmi);
  BIND(&if_valueisnotsmi);
  {
    // Load the instance type of the {value}.
    TNode<Uint16T> value_instance_type = LoadInstanceType(CAST(value));

    // Check if the {value} is already String.
    Label if_valueisnotstring(this, Label::kDeferred);
    Branch(IsStringInstanceType(value_instance_type), &if_valueisstring,
           &if_valueisnotstring);
    BIND(&if_valueisnotstring);
    {
      // Check if the {value} is null.
      Label if_valueisnullorundefined(this, Label::kDeferred);
      GotoIf(IsNullOrUndefined(value), &if_valueisnullorundefined);
      // Convert the {value} to a String.
      var_value = CallBuiltin(Builtins::kToString, context, value);
      Goto(&if_valueisstring);

      BIND(&if_valueisnullorundefined);
      {
        // The {value} is either null or undefined.
        ThrowTypeError(context, MessageTemplate::kCalledOnNullOrUndefined,
                       method_name);
      }
    }
  }
  BIND(&if_valueissmi);
  {
    // The {value} is a Smi, convert it to a String.
    var_value = CallBuiltin(Builtins::kNumberToString, context, value);
    Goto(&if_valueisstring);
  }
  BIND(&if_valueisstring);
  return CAST(var_value.value());
}

TNode<Uint32T> CodeStubAssembler::ChangeNumberToUint32(TNode<Number> value) {
  TVARIABLE(Uint32T, var_result);
  Label if_smi(this), if_heapnumber(this, Label::kDeferred), done(this);
  Branch(TaggedIsSmi(value), &if_smi, &if_heapnumber);
  BIND(&if_smi);
  {
    var_result = Unsigned(SmiToInt32(CAST(value)));
    Goto(&done);
  }
  BIND(&if_heapnumber);
  {
    var_result = ChangeFloat64ToUint32(LoadHeapNumberValue(CAST(value)));
    Goto(&done);
  }
  BIND(&done);
  return var_result.value();
}

TNode<Float64T> CodeStubAssembler::ChangeNumberToFloat64(TNode<Number> value) {
  TVARIABLE(Float64T, result);
  Label smi(this);
  Label done(this, &result);
  GotoIf(TaggedIsSmi(value), &smi);
  result = LoadHeapNumberValue(CAST(value));
  Goto(&done);

  BIND(&smi);
  {
    result = SmiToFloat64(CAST(value));
    Goto(&done);
  }

  BIND(&done);
  return result.value();
}

TNode<Int32T> CodeStubAssembler::ChangeTaggedNonSmiToInt32(
    TNode<Context> context, TNode<HeapObject> input) {
  return Select<Int32T>(
      IsHeapNumber(input),
      [=] {
        return Signed(TruncateFloat64ToWord32(LoadHeapNumberValue(input)));
      },
      [=] {
        return TruncateNumberToWord32(
            CAST(CallBuiltin(Builtins::kNonNumberToNumber, context, input)));
      });
}

TNode<Float64T> CodeStubAssembler::ChangeTaggedToFloat64(TNode<Context> context,
                                                         TNode<Object> input) {
  TVARIABLE(Float64T, var_result);
  Label end(this), not_smi(this);

  GotoIfNot(TaggedIsSmi(input), &not_smi);
  var_result = SmiToFloat64(CAST(input));
  Goto(&end);

  BIND(&not_smi);
  var_result = Select<Float64T>(
      IsHeapNumber(CAST(input)),
      [=] { return LoadHeapNumberValue(CAST(input)); },
      [=] {
        return ChangeNumberToFloat64(
            CAST(CallBuiltin(Builtins::kNonNumberToNumber, context, input)));
      });
  Goto(&end);

  BIND(&end);
  return var_result.value();
}

TNode<WordT> CodeStubAssembler::TimesSystemPointerSize(
    SloppyTNode<WordT> value) {
  return WordShl(value, kSystemPointerSizeLog2);
}

TNode<WordT> CodeStubAssembler::TimesTaggedSize(SloppyTNode<WordT> value) {
  return WordShl(value, kTaggedSizeLog2);
}

TNode<WordT> CodeStubAssembler::TimesDoubleSize(SloppyTNode<WordT> value) {
  return WordShl(value, kDoubleSizeLog2);
}

TNode<Object> CodeStubAssembler::ToThisValue(TNode<Context> context,
                                             TNode<Object> value,
                                             PrimitiveType primitive_type,
                                             char const* method_name) {
  // We might need to loop once due to JSPrimitiveWrapper unboxing.
  TVARIABLE(Object, var_value, value);
  Label loop(this, &var_value), done_loop(this),
      done_throw(this, Label::kDeferred);
  Goto(&loop);
  BIND(&loop);
  {
    // Check if the {value} is a Smi or a HeapObject.
    GotoIf(
        TaggedIsSmi(var_value.value()),
        (primitive_type == PrimitiveType::kNumber) ? &done_loop : &done_throw);

    TNode<HeapObject> value = CAST(var_value.value());

    // Load the map of the {value}.
    TNode<Map> value_map = LoadMap(value);

    // Load the instance type of the {value}.
    TNode<Uint16T> value_instance_type = LoadMapInstanceType(value_map);

    // Check if {value} is a JSPrimitiveWrapper.
    Label if_valueiswrapper(this, Label::kDeferred), if_valueisnotwrapper(this);
    Branch(InstanceTypeEqual(value_instance_type, JS_PRIMITIVE_WRAPPER_TYPE),
           &if_valueiswrapper, &if_valueisnotwrapper);

    BIND(&if_valueiswrapper);
    {
      // Load the actual value from the {value}.
      var_value = LoadObjectField(value, JSPrimitiveWrapper::kValueOffset);
      Goto(&loop);
    }

    BIND(&if_valueisnotwrapper);
    {
      switch (primitive_type) {
        case PrimitiveType::kBoolean:
          GotoIf(TaggedEqual(value_map, BooleanMapConstant()), &done_loop);
          break;
        case PrimitiveType::kNumber:
          GotoIf(TaggedEqual(value_map, HeapNumberMapConstant()), &done_loop);
          break;
        case PrimitiveType::kString:
          GotoIf(IsStringInstanceType(value_instance_type), &done_loop);
          break;
        case PrimitiveType::kSymbol:
          GotoIf(TaggedEqual(value_map, SymbolMapConstant()), &done_loop);
          break;
      }
      Goto(&done_throw);
    }
  }

  BIND(&done_throw);
  {
    const char* primitive_name = nullptr;
    switch (primitive_type) {
      case PrimitiveType::kBoolean:
        primitive_name = "Boolean";
        break;
      case PrimitiveType::kNumber:
        primitive_name = "Number";
        break;
      case PrimitiveType::kString:
        primitive_name = "String";
        break;
      case PrimitiveType::kSymbol:
        primitive_name = "Symbol";
        break;
    }
    CHECK_NOT_NULL(primitive_name);

    // The {value} is not a compatible receiver for this method.
    ThrowTypeError(context, MessageTemplate::kNotGeneric, method_name,
                   primitive_name);
  }

  BIND(&done_loop);
  return var_value.value();
}

void CodeStubAssembler::ThrowIfNotInstanceType(TNode<Context> context,
                                               TNode<Object> value,
                                               InstanceType instance_type,
                                               char const* method_name) {
  Label out(this), throw_exception(this, Label::kDeferred);

  GotoIf(TaggedIsSmi(value), &throw_exception);

  // Load the instance type of the {value}.
  TNode<Map> map = LoadMap(CAST(value));
  const TNode<Uint16T> value_instance_type = LoadMapInstanceType(map);

  Branch(Word32Equal(value_instance_type, Int32Constant(instance_type)), &out,
         &throw_exception);

  // The {value} is not a compatible receiver for this method.
  BIND(&throw_exception);
  ThrowTypeError(context, MessageTemplate::kIncompatibleMethodReceiver,
                 StringConstant(method_name), value);

  BIND(&out);
}

void CodeStubAssembler::ThrowIfNotJSReceiver(TNode<Context> context,
                                             TNode<Object> value,
                                             MessageTemplate msg_template,
                                             const char* method_name) {
  Label done(this), throw_exception(this, Label::kDeferred);

  GotoIf(TaggedIsSmi(value), &throw_exception);

  // Load the instance type of the {value}.
  TNode<Map> value_map = LoadMap(CAST(value));
  const TNode<Uint16T> value_instance_type = LoadMapInstanceType(value_map);

  Branch(IsJSReceiverInstanceType(value_instance_type), &done,
         &throw_exception);

  // The {value} is not a compatible receiver for this method.
  BIND(&throw_exception);
  ThrowTypeError(context, msg_template, StringConstant(method_name), value);

  BIND(&done);
}

void CodeStubAssembler::ThrowIfNotCallable(TNode<Context> context,
                                           TNode<Object> value,
                                           const char* method_name) {
  Label out(this), throw_exception(this, Label::kDeferred);

  GotoIf(TaggedIsSmi(value), &throw_exception);
  Branch(IsCallable(CAST(value)), &out, &throw_exception);

  // The {value} is not a compatible receiver for this method.
  BIND(&throw_exception);
  ThrowTypeError(context, MessageTemplate::kCalledNonCallable, method_name);

  BIND(&out);
}

void CodeStubAssembler::ThrowRangeError(TNode<Context> context,
                                        MessageTemplate message,
                                        base::Optional<TNode<Object>> arg0,
                                        base::Optional<TNode<Object>> arg1,
                                        base::Optional<TNode<Object>> arg2) {
  TNode<Smi> template_index = SmiConstant(static_cast<int>(message));
  if (!arg0) {
    CallRuntime(Runtime::kThrowRangeError, context, template_index);
  } else if (!arg1) {
    CallRuntime(Runtime::kThrowRangeError, context, template_index, *arg0);
  } else if (!arg2) {
    CallRuntime(Runtime::kThrowRangeError, context, template_index, *arg0,
                *arg1);
  } else {
    CallRuntime(Runtime::kThrowRangeError, context, template_index, *arg0,
                *arg1, *arg2);
  }
  Unreachable();
}

void CodeStubAssembler::ThrowTypeError(TNode<Context> context,
                                       MessageTemplate message,
                                       char const* arg0, char const* arg1) {
  base::Optional<TNode<Object>> arg0_node;
  if (arg0) arg0_node = StringConstant(arg0);
  base::Optional<TNode<Object>> arg1_node;
  if (arg1) arg1_node = StringConstant(arg1);
  ThrowTypeError(context, message, arg0_node, arg1_node);
}

void CodeStubAssembler::ThrowTypeError(TNode<Context> context,
                                       MessageTemplate message,
                                       base::Optional<TNode<Object>> arg0,
                                       base::Optional<TNode<Object>> arg1,
                                       base::Optional<TNode<Object>> arg2) {
  TNode<Smi> template_index = SmiConstant(static_cast<int>(message));
  if (!arg0) {
    CallRuntime(Runtime::kThrowTypeError, context, template_index);
  } else if (!arg1) {
    CallRuntime(Runtime::kThrowTypeError, context, template_index, *arg0);
  } else if (!arg2) {
    CallRuntime(Runtime::kThrowTypeError, context, template_index, *arg0,
                *arg1);
  } else {
    CallRuntime(Runtime::kThrowTypeError, context, template_index, *arg0, *arg1,
                *arg2);
  }
  Unreachable();
}

TNode<BoolT> CodeStubAssembler::InstanceTypeEqual(
    SloppyTNode<Int32T> instance_type, int type) {
  return Word32Equal(instance_type, Int32Constant(type));
}

TNode<BoolT> CodeStubAssembler::IsDictionaryMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits3::IsDictionaryMapBit>(LoadMapBitField3(map));
}

TNode<BoolT> CodeStubAssembler::IsExtensibleMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits3::IsExtensibleBit>(LoadMapBitField3(map));
}

TNode<BoolT> CodeStubAssembler::IsExtensibleNonPrototypeMap(TNode<Map> map) {
  int kMask =
      Map::Bits3::IsExtensibleBit::kMask | Map::Bits3::IsPrototypeMapBit::kMask;
  int kExpected = Map::Bits3::IsExtensibleBit::kMask;
  return Word32Equal(Word32And(LoadMapBitField3(map), Int32Constant(kMask)),
                     Int32Constant(kExpected));
}

TNode<BoolT> CodeStubAssembler::IsCallableMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits1::IsCallableBit>(LoadMapBitField(map));
}

TNode<BoolT> CodeStubAssembler::IsDeprecatedMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits3::IsDeprecatedBit>(LoadMapBitField3(map));
}

TNode<BoolT> CodeStubAssembler::IsUndetectableMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits1::IsUndetectableBit>(LoadMapBitField(map));
}

TNode<BoolT> CodeStubAssembler::IsNoElementsProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = NoElementsProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsArrayIteratorProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = ArrayIteratorProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsPromiseResolveProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = PromiseResolveProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsPromiseThenProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = PromiseThenProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsArraySpeciesProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = ArraySpeciesProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsTypedArraySpeciesProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = TypedArraySpeciesProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsRegExpSpeciesProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = RegExpSpeciesProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsPromiseSpeciesProtectorCellInvalid() {
  TNode<Smi> invalid = SmiConstant(Protectors::kProtectorInvalid);
  TNode<PropertyCell> cell = PromiseSpeciesProtectorConstant();
  TNode<Object> cell_value = LoadObjectField(cell, PropertyCell::kValueOffset);
  return TaggedEqual(cell_value, invalid);
}

TNode<BoolT> CodeStubAssembler::IsPrototypeInitialArrayPrototype(
    TNode<Context> context, TNode<Map> map) {
  const TNode<NativeContext> native_context = LoadNativeContext(context);
  const TNode<Object> initial_array_prototype = LoadContextElement(
      native_context, Context::INITIAL_ARRAY_PROTOTYPE_INDEX);
  TNode<HeapObject> proto = LoadMapPrototype(map);
  return TaggedEqual(proto, initial_array_prototype);
}

TNode<BoolT> CodeStubAssembler::IsPrototypeTypedArrayPrototype(
    TNode<Context> context, TNode<Map> map) {
  const TNode<NativeContext> native_context = LoadNativeContext(context);
  const TNode<Object> typed_array_prototype =
      LoadContextElement(native_context, Context::TYPED_ARRAY_PROTOTYPE_INDEX);
  TNode<HeapObject> proto = LoadMapPrototype(map);
  TNode<HeapObject> proto_of_proto = Select<HeapObject>(
      IsJSObject(proto), [=] { return LoadMapPrototype(LoadMap(proto)); },
      [=] { return NullConstant(); });
  return TaggedEqual(proto_of_proto, typed_array_prototype);
}

TNode<BoolT> CodeStubAssembler::IsFastAliasedArgumentsMap(
    TNode<Context> context, TNode<Map> map) {
  const TNode<NativeContext> native_context = LoadNativeContext(context);
  const TNode<Object> arguments_map = LoadContextElement(
      native_context, Context::FAST_ALIASED_ARGUMENTS_MAP_INDEX);
  return TaggedEqual(arguments_map, map);
}

TNode<BoolT> CodeStubAssembler::IsSlowAliasedArgumentsMap(
    TNode<Context> context, TNode<Map> map) {
  const TNode<NativeContext> native_context = LoadNativeContext(context);
  const TNode<Object> arguments_map = LoadContextElement(
      native_context, Context::SLOW_ALIASED_ARGUMENTS_MAP_INDEX);
  return TaggedEqual(arguments_map, map);
}

TNode<BoolT> CodeStubAssembler::IsSloppyArgumentsMap(TNode<Context> context,
                                                     TNode<Map> map) {
  const TNode<NativeContext> native_context = LoadNativeContext(context);
  const TNode<Object> arguments_map =
      LoadContextElement(native_context, Context::SLOPPY_ARGUMENTS_MAP_INDEX);
  return TaggedEqual(arguments_map, map);
}

TNode<BoolT> CodeStubAssembler::IsStrictArgumentsMap(TNode<Context> context,
                                                     TNode<Map> map) {
  const TNode<NativeContext> native_context = LoadNativeContext(context);
  const TNode<Object> arguments_map =
      LoadContextElement(native_context, Context::STRICT_ARGUMENTS_MAP_INDEX);
  return TaggedEqual(arguments_map, map);
}

TNode<BoolT> CodeStubAssembler::TaggedIsCallable(TNode<Object> object) {
  return Select<BoolT>(
      TaggedIsSmi(object), [=] { return Int32FalseConstant(); },
      [=] {
        return IsCallableMap(LoadMap(UncheckedCast<HeapObject>(object)));
      });
}

TNode<BoolT> CodeStubAssembler::IsCallable(TNode<HeapObject> object) {
  return IsCallableMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsConstructorMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits1::IsConstructorBit>(LoadMapBitField(map));
}

TNode<BoolT> CodeStubAssembler::IsConstructor(TNode<HeapObject> object) {
  return IsConstructorMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsFunctionWithPrototypeSlotMap(TNode<Map> map) {
  return IsSetWord32<Map::Bits1::HasPrototypeSlotBit>(LoadMapBitField(map));
}

TNode<BoolT> CodeStubAssembler::IsSpecialReceiverInstanceType(
    TNode<Int32T> instance_type) {
  STATIC_ASSERT(JS_GLOBAL_OBJECT_TYPE <= LAST_SPECIAL_RECEIVER_TYPE);
  return Int32LessThanOrEqual(instance_type,
                              Int32Constant(LAST_SPECIAL_RECEIVER_TYPE));
}

TNode<BoolT> CodeStubAssembler::IsCustomElementsReceiverInstanceType(
    TNode<Int32T> instance_type) {
  return Int32LessThanOrEqual(instance_type,
                              Int32Constant(LAST_CUSTOM_ELEMENTS_RECEIVER));
}

TNode<BoolT> CodeStubAssembler::IsStringInstanceType(
    SloppyTNode<Int32T> instance_type) {
  STATIC_ASSERT(INTERNALIZED_STRING_TYPE == FIRST_TYPE);
  return Int32LessThan(instance_type, Int32Constant(FIRST_NONSTRING_TYPE));
}

TNode<BoolT> CodeStubAssembler::IsOneByteStringInstanceType(
    TNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  return Word32Equal(
      Word32And(instance_type, Int32Constant(kStringEncodingMask)),
      Int32Constant(kOneByteStringTag));
}

TNode<BoolT> CodeStubAssembler::IsSequentialStringInstanceType(
    SloppyTNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  return Word32Equal(
      Word32And(instance_type, Int32Constant(kStringRepresentationMask)),
      Int32Constant(kSeqStringTag));
}

TNode<BoolT> CodeStubAssembler::IsSeqOneByteStringInstanceType(
    TNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  return Word32Equal(
      Word32And(instance_type,
                Int32Constant(kStringRepresentationMask | kStringEncodingMask)),
      Int32Constant(kSeqStringTag | kOneByteStringTag));
}

TNode<BoolT> CodeStubAssembler::IsConsStringInstanceType(
    SloppyTNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  return Word32Equal(
      Word32And(instance_type, Int32Constant(kStringRepresentationMask)),
      Int32Constant(kConsStringTag));
}

TNode<BoolT> CodeStubAssembler::IsIndirectStringInstanceType(
    SloppyTNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  STATIC_ASSERT(kIsIndirectStringMask == 0x1);
  STATIC_ASSERT(kIsIndirectStringTag == 0x1);
  return UncheckedCast<BoolT>(
      Word32And(instance_type, Int32Constant(kIsIndirectStringMask)));
}

TNode<BoolT> CodeStubAssembler::IsExternalStringInstanceType(
    SloppyTNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  return Word32Equal(
      Word32And(instance_type, Int32Constant(kStringRepresentationMask)),
      Int32Constant(kExternalStringTag));
}

TNode<BoolT> CodeStubAssembler::IsUncachedExternalStringInstanceType(
    SloppyTNode<Int32T> instance_type) {
  CSA_ASSERT(this, IsStringInstanceType(instance_type));
  STATIC_ASSERT(kUncachedExternalStringTag != 0);
  return IsSetWord32(instance_type, kUncachedExternalStringMask);
}

TNode<BoolT> CodeStubAssembler::IsJSReceiverInstanceType(
    SloppyTNode<Int32T> instance_type) {
  STATIC_ASSERT(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
  return Int32GreaterThanOrEqual(instance_type,
                                 Int32Constant(FIRST_JS_RECEIVER_TYPE));
}

TNode<BoolT> CodeStubAssembler::IsJSReceiverMap(TNode<Map> map) {
  return IsJSReceiverInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSReceiver(TNode<HeapObject> object) {
  return IsJSReceiverMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsNullOrJSReceiver(TNode<HeapObject> object) {
  return UncheckedCast<BoolT>(Word32Or(IsJSReceiver(object), IsNull(object)));
}

TNode<BoolT> CodeStubAssembler::IsNullOrUndefined(SloppyTNode<Object> value) {
  return UncheckedCast<BoolT>(Word32Or(IsUndefined(value), IsNull(value)));
}

TNode<BoolT> CodeStubAssembler::IsJSGlobalProxyInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, JS_GLOBAL_PROXY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSGlobalProxyMap(TNode<Map> map) {
  return IsJSGlobalProxyInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSGlobalProxy(TNode<HeapObject> object) {
  return IsJSGlobalProxyMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSGeneratorMap(TNode<Map> map) {
  return InstanceTypeEqual(LoadMapInstanceType(map), JS_GENERATOR_OBJECT_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSObjectInstanceType(
    SloppyTNode<Int32T> instance_type) {
  STATIC_ASSERT(LAST_JS_OBJECT_TYPE == LAST_TYPE);
  return Int32GreaterThanOrEqual(instance_type,
                                 Int32Constant(FIRST_JS_OBJECT_TYPE));
}

TNode<BoolT> CodeStubAssembler::IsJSObjectMap(TNode<Map> map) {
  return IsJSObjectInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSObject(TNode<HeapObject> object) {
  return IsJSObjectMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSFinalizationRegistryMap(TNode<Map> map) {
  return InstanceTypeEqual(LoadMapInstanceType(map),
                           JS_FINALIZATION_REGISTRY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSFinalizationRegistry(
    TNode<HeapObject> object) {
  return IsJSFinalizationRegistryMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSPromiseMap(TNode<Map> map) {
  return InstanceTypeEqual(LoadMapInstanceType(map), JS_PROMISE_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSPromise(TNode<HeapObject> object) {
  return IsJSPromiseMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSProxy(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_PROXY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSStringIterator(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_STRING_ITERATOR_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSRegExpStringIterator(
    TNode<HeapObject> object) {
  return HasInstanceType(object, JS_REG_EXP_STRING_ITERATOR_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsMap(TNode<HeapObject> map) {
  return IsMetaMap(LoadMap(map));
}

TNode<BoolT> CodeStubAssembler::IsJSPrimitiveWrapperInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, JS_PRIMITIVE_WRAPPER_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSPrimitiveWrapper(TNode<HeapObject> object) {
  return IsJSPrimitiveWrapperMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSPrimitiveWrapperMap(TNode<Map> map) {
  return IsJSPrimitiveWrapperInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSArrayInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, JS_ARRAY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSArray(TNode<HeapObject> object) {
  return IsJSArrayMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSArrayMap(TNode<Map> map) {
  return IsJSArrayInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSArrayIterator(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_ARRAY_ITERATOR_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSAsyncGeneratorObject(
    TNode<HeapObject> object) {
  return HasInstanceType(object, JS_ASYNC_GENERATOR_OBJECT_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsFixedArray(TNode<HeapObject> object) {
  return HasInstanceType(object, FIXED_ARRAY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsFixedArraySubclass(TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return UncheckedCast<BoolT>(
      Word32And(Int32GreaterThanOrEqual(instance_type,
                                        Int32Constant(FIRST_FIXED_ARRAY_TYPE)),
                Int32LessThanOrEqual(instance_type,
                                     Int32Constant(LAST_FIXED_ARRAY_TYPE))));
}

TNode<BoolT> CodeStubAssembler::IsNotWeakFixedArraySubclass(
    TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return UncheckedCast<BoolT>(Word32Or(
      Int32LessThan(instance_type, Int32Constant(FIRST_WEAK_FIXED_ARRAY_TYPE)),
      Int32GreaterThan(instance_type,
                       Int32Constant(LAST_WEAK_FIXED_ARRAY_TYPE))));
}

TNode<BoolT> CodeStubAssembler::IsPropertyArray(TNode<HeapObject> object) {
  return HasInstanceType(object, PROPERTY_ARRAY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsPromiseReactionJobTask(
    TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return IsInRange(instance_type, FIRST_PROMISE_REACTION_JOB_TASK_TYPE,
                   LAST_PROMISE_REACTION_JOB_TASK_TYPE);
}

// This complicated check is due to elements oddities. If a smi array is empty
// after Array.p.shift, it is replaced by the empty array constant. If it is
// later filled with a double element, we try to grow it but pass in a double
// elements kind. Usually this would cause a size mismatch (since the source
// fixed array has HOLEY_ELEMENTS and destination has
// HOLEY_DOUBLE_ELEMENTS), but we don't have to worry about it when the
// source array is empty.
// TODO(jgruber): It might we worth creating an empty_double_array constant to
// simplify this case.
TNode<BoolT> CodeStubAssembler::IsFixedArrayWithKindOrEmpty(
    TNode<FixedArrayBase> object, ElementsKind kind) {
  Label out(this);
  TVARIABLE(BoolT, var_result, Int32TrueConstant());

  GotoIf(IsFixedArrayWithKind(object, kind), &out);

  const TNode<Smi> length = LoadFixedArrayBaseLength(object);
  GotoIf(SmiEqual(length, SmiConstant(0)), &out);

  var_result = Int32FalseConstant();
  Goto(&out);

  BIND(&out);
  return var_result.value();
}

TNode<BoolT> CodeStubAssembler::IsFixedArrayWithKind(TNode<HeapObject> object,
                                                     ElementsKind kind) {
  if (IsDoubleElementsKind(kind)) {
    return IsFixedDoubleArray(object);
  } else {
    DCHECK(IsSmiOrObjectElementsKind(kind) || IsSealedElementsKind(kind) ||
           IsNonextensibleElementsKind(kind));
    return IsFixedArraySubclass(object);
  }
}

TNode<BoolT> CodeStubAssembler::IsBoolean(TNode<HeapObject> object) {
  return IsBooleanMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsPropertyCell(TNode<HeapObject> object) {
  return IsPropertyCellMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsHeapNumberInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, HEAP_NUMBER_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsOddball(TNode<HeapObject> object) {
  return IsOddballInstanceType(LoadInstanceType(object));
}

TNode<BoolT> CodeStubAssembler::IsOddballInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, ODDBALL_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsName(TNode<HeapObject> object) {
  return IsNameInstanceType(LoadInstanceType(object));
}

TNode<BoolT> CodeStubAssembler::IsNameInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return Int32LessThanOrEqual(instance_type, Int32Constant(LAST_NAME_TYPE));
}

TNode<BoolT> CodeStubAssembler::IsString(TNode<HeapObject> object) {
  return IsStringInstanceType(LoadInstanceType(object));
}

TNode<BoolT> CodeStubAssembler::IsSeqOneByteString(TNode<HeapObject> object) {
  return IsSeqOneByteStringInstanceType(LoadInstanceType(object));
}

TNode<BoolT> CodeStubAssembler::IsSymbolInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, SYMBOL_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsInternalizedStringInstanceType(
    TNode<Int32T> instance_type) {
  STATIC_ASSERT(kNotInternalizedTag != 0);
  return Word32Equal(
      Word32And(instance_type,
                Int32Constant(kIsNotStringMask | kIsNotInternalizedMask)),
      Int32Constant(kStringTag | kInternalizedTag));
}

TNode<BoolT> CodeStubAssembler::IsUniqueName(TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return Select<BoolT>(
      IsInternalizedStringInstanceType(instance_type),
      [=] { return Int32TrueConstant(); },
      [=] { return IsSymbolInstanceType(instance_type); });
}

// Semantics: guaranteed not to be an integer index (i.e. contains non-digit
// characters, or is outside MAX_SAFE_INTEGER/size_t range). Note that for
// non-TypedArray receivers, there are additional strings that must be treated
// as named property keys, namely the range [0xFFFFFFFF, MAX_SAFE_INTEGER].
TNode<BoolT> CodeStubAssembler::IsUniqueNameNoIndex(TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return Select<BoolT>(
      IsInternalizedStringInstanceType(instance_type),
      [=] {
        return IsSetWord32(LoadNameHashField(CAST(object)),
                           Name::kIsNotIntegerIndexMask);
      },
      [=] { return IsSymbolInstanceType(instance_type); });
}

// Semantics: {object} is a Symbol, or a String that doesn't have a cached
// index. This returns {true} for strings containing representations of
// integers in the range above 9999999 (per kMaxCachedArrayIndexLength)
// and below MAX_SAFE_INTEGER. For CSA_ASSERTs ensuring correct usage, this is
// better than no checking; and we don't have a good/fast way to accurately
// check such strings for being within "array index" (uint32_t) range.
TNode<BoolT> CodeStubAssembler::IsUniqueNameNoCachedIndex(
    TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return Select<BoolT>(
      IsInternalizedStringInstanceType(instance_type),
      [=] {
        return IsSetWord32(LoadNameHashField(CAST(object)),
                           Name::kDoesNotContainCachedArrayIndexMask);
      },
      [=] { return IsSymbolInstanceType(instance_type); });
}

TNode<BoolT> CodeStubAssembler::IsBigIntInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, BIGINT_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsBigInt(TNode<HeapObject> object) {
  return IsBigIntInstanceType(LoadInstanceType(object));
}

TNode<BoolT> CodeStubAssembler::IsPrimitiveInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return Int32LessThanOrEqual(instance_type,
                              Int32Constant(LAST_PRIMITIVE_HEAP_OBJECT_TYPE));
}

TNode<BoolT> CodeStubAssembler::IsPrivateName(SloppyTNode<Symbol> symbol) {
  TNode<Uint32T> flags = LoadObjectField<Uint32T>(symbol, Symbol::kFlagsOffset);
  return IsSetWord32<Symbol::IsPrivateNameBit>(flags);
}

TNode<BoolT> CodeStubAssembler::IsHashTable(TNode<HeapObject> object) {
  TNode<Uint16T> instance_type = LoadInstanceType(object);
  return UncheckedCast<BoolT>(
      Word32And(Int32GreaterThanOrEqual(instance_type,
                                        Int32Constant(FIRST_HASH_TABLE_TYPE)),
                Int32LessThanOrEqual(instance_type,
                                     Int32Constant(LAST_HASH_TABLE_TYPE))));
}

TNode<BoolT> CodeStubAssembler::IsEphemeronHashTable(TNode<HeapObject> object) {
  return HasInstanceType(object, EPHEMERON_HASH_TABLE_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsNameDictionary(TNode<HeapObject> object) {
  return HasInstanceType(object, NAME_DICTIONARY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsGlobalDictionary(TNode<HeapObject> object) {
  return HasInstanceType(object, GLOBAL_DICTIONARY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsNumberDictionary(TNode<HeapObject> object) {
  return HasInstanceType(object, NUMBER_DICTIONARY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSGeneratorObject(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_GENERATOR_OBJECT_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSFunctionInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, JS_FUNCTION_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSFunction(TNode<HeapObject> object) {
  return IsJSFunctionMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSBoundFunction(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_BOUND_FUNCTION_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSFunctionMap(TNode<Map> map) {
  return IsJSFunctionInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSTypedArrayInstanceType(
    SloppyTNode<Int32T> instance_type) {
  return InstanceTypeEqual(instance_type, JS_TYPED_ARRAY_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSTypedArrayMap(TNode<Map> map) {
  return IsJSTypedArrayInstanceType(LoadMapInstanceType(map));
}

TNode<BoolT> CodeStubAssembler::IsJSTypedArray(TNode<HeapObject> object) {
  return IsJSTypedArrayMap(LoadMap(object));
}

TNode<BoolT> CodeStubAssembler::IsJSArrayBuffer(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_ARRAY_BUFFER_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSDataView(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_DATA_VIEW_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsJSRegExp(TNode<HeapObject> object) {
  return HasInstanceType(object, JS_REG_EXP_TYPE);
}

TNode<BoolT> CodeStubAssembler::IsNumeric(SloppyTNode<Object> object) {
  return Select<BoolT>(
      TaggedIsSmi(object), [=] { return Int32TrueConstant(); },
      [=] {
        return UncheckedCast<BoolT>(
            Word32Or(IsHeapNumber(CAST(object)), IsBigInt(CAST(object))));
      });
}

TNode<BoolT> CodeStubAssembler::IsNumberNormalized(TNode<Number> number) {
  TVARIABLE(BoolT, var_result, Int32TrueConstant());
  Label out(this);

  GotoIf(TaggedIsSmi(number), &out);

  TNode<Float64T> value = LoadHeapNumberValue(CAST(number));
  TNode<Float64T> smi_min =
      Float64Constant(static_cast<double>(Smi::kMinValue));
  TNode<Float64T> smi_max =
      Float64Constant(static_cast<double>(Smi::kMaxValue));

  GotoIf(Float64LessThan(value, smi_min), &out);
  GotoIf(Float64GreaterThan(value, smi_max), &out);
  GotoIfNot(Float64Equal(value, value), &out);  // NaN.

  var_result = Int32FalseConstant();
  Goto(&out);

  BIND(&out);
  return var_result.value();
}

TNode<BoolT> CodeStubAssembler::IsNumberPositive(TNode<Number> number) {
  return Select<BoolT>(
      TaggedIsSmi(number), [=] { return TaggedIsPositiveSmi(number); },
      [=] { return IsHeapNumberPositive(CAST(number)); });
}

// TODO(cbruni): Use TNode<HeapNumber> instead of custom name.
TNode<BoolT> CodeStubAssembler::IsHeapNumberPositive(TNode<HeapNumber> number) {
  TNode<Float64T> value = LoadHeapNumberValue(number);
  TNode<Float64T> float_zero = Float64Constant(0.);
  return Float64GreaterThanOrEqual(value, float_zero);
}

TNode<BoolT> CodeStubAssembler::IsNumberNonNegativeSafeInteger(
    TNode<Number> number) {
  return Select<BoolT>(
      // TODO(cbruni): Introduce TaggedIsNonNegateSmi to avoid confusion.
      TaggedIsSmi(number), [=] { return TaggedIsPositiveSmi(number); },
      [=] {
        TNode<HeapNumber> heap_number = CAST(number);
        return Select<BoolT>(
            IsInteger(heap_number),
            [=] { return IsHeapNumberPositive(heap_number); },
            [=] { return Int32FalseConstant(); });
      });
}

TNode<BoolT> CodeStubAssembler::IsSafeInteger(TNode<Object> number) {
  return Select<BoolT>(
      TaggedIsSmi(number), [=] { return Int32TrueConstant(); },
      [=] {
        return Select<BoolT>(
            IsHeapNumber(CAST(number)),
            [=] { return IsSafeInteger(UncheckedCast<HeapNumber>(number)); },
            [=] { return Int32FalseConstant(); });
      });
}

TNode<BoolT> CodeStubAssembler::IsSafeInteger(TNode<HeapNumber> number) {
  // Load the actual value of {number}.
  TNode<Float64T> number_value = LoadHeapNumberValue(number);
  // Truncate the value of {number} to an integer (or an infinity).
  TNode<Float64T> integer = Float64Trunc(number_value);

  return Select<BoolT>(
      // Check if {number}s value matches the integer (ruling out the
      // infinities).
      Float64Equal(Float64Sub(number_value, integer), Float64Constant(0.0)),
      [=] {
        // Check if the {integer} value is in safe integer range.
        return Float64LessThanOrEqual(Float64Abs(integer),
                                      Float64Constant(kMaxSafeInteger));
      },
      [=] { return Int32FalseConstant(); });
}

TNode<BoolT> CodeStubAssembler::IsInteger(TNode<Object> number) {
  return Select<BoolT>(
      TaggedIsSmi(number), [=] { return Int32TrueConstant(); },
      [=] {
        return Select<BoolT>(
            IsHeapNumber(CAST(number)),
            [=] { return IsInteger(UncheckedCast<HeapNumber>(number)); },
            [=] { return Int32FalseConstant(); });
      });
}

TNode<BoolT> CodeStubAssembler::IsInteger(TNode<HeapNumber> number) {
  TNode<Float64T> number_value = LoadHeapNumberValue(number);
  // Truncate the value of {number} to an integer (or an infinity).
  TNode<Float64T> integer = Float64Trunc(number_value);
  // Check if {number}s value matches the integer (ruling out the infinities).
  return Float64Equal(Float64Sub(number_value, integer), Float64Constant(0.0));
}

TNode<BoolT> CodeStubAssembler::IsHeapNumberUint32(TNode<HeapNumber> number) {
  // Check that the HeapNumber is a valid uint32
  return Select<BoolT>(
      IsHeapNumberPositive(number),
      [=] {
        TNode<Float64T> value = LoadHeapNumberValue(number);
        TNode<Uint32T> int_value = TruncateFloat64ToWord32(value);
        return Float64Equal(value, ChangeUint32ToFloat64(int_value));
      },
      [=] { return Int32FalseConstant(); });
}

TNode<BoolT> CodeStubAssembler::IsNumberArrayIndex(TNode<Number> number) {
  return Select<BoolT>(
      TaggedIsSmi(number), [=] { return TaggedIsPositiveSmi(number); },
      [=] { return IsHeapNumberUint32(CAST(number)); });
}

template <typename TIndex>
TNode<BoolT> CodeStubAssembler::FixedArraySizeDoesntFitInNewSpace(
    TNode<TIndex> element_count, int base_size) {
  static_assert(
      std::is_same<TIndex, Smi>::value || std::is_same<TIndex, IntPtrT>::value,
      "Only Smi or IntPtrT element_count is allowed");
  int max_newspace_elements =
      (kMaxRegularHeapObjectSize - base_size) / kTaggedSize;
  return IntPtrOrSmiGreaterThan(
      element_count, IntPtrOrSmiConstant<TIndex>(max_newspace_elements));
}

TNode<Int32T> CodeStubAssembler::StringCharCodeAt(TNode<String> string,
                                                  TNode<UintPtrT> index) {
  CSA_ASSERT(this, UintPtrLessThan(index, LoadStringLengthAsWord(string)));

  TVARIABLE(Int32T, var_result);

  Label return_result(this), if_runtime(this, Label::kDeferred),
      if_stringistwobyte(this), if_stringisonebyte(this);

  ToDirectStringAssembler to_direct(state(), string);
  to_direct.TryToDirect(&if_runtime);
  const TNode<UintPtrT> offset =
      UintPtrAdd(index, Unsigned(to_direct.offset()));
  const TNode<Int32T> instance_type = to_direct.instance_type();
  const TNode<RawPtrT> string_data = to_direct.PointerToData(&if_runtime);

  // Check if the {string} is a TwoByteSeqString or a OneByteSeqString.
  Branch(IsOneByteStringInstanceType(instance_type), &if_stringisonebyte,
         &if_stringistwobyte);

  BIND(&if_stringisonebyte);
  {
    var_result = UncheckedCast<Int32T>(Load<Uint8T>(string_data, offset));
    Goto(&return_result);
  }

  BIND(&if_stringistwobyte);
  {
    var_result = UncheckedCast<Int32T>(
        Load<Uint16T>(string_data, WordShl(offset, IntPtrConstant(1))));
    Goto(&return_result);
  }

  BIND(&if_runtime);
  {
    TNode<Object> result =
        CallRuntime(Runtime::kStringCharCodeAt, NoContextConstant(), string,
                    ChangeUintPtrToTagged(index));
    var_result = SmiToInt32(CAST(result));
    Goto(&return_result);
  }

  BIND(&return_result);
  return var_result.value();
}

TNode<String> CodeStubAssembler::StringFromSingleCharCode(TNode<Int32T> code) {
  TVARIABLE(String, var_result);

  // Check if the {code} is a one-byte char code.
  Label if_codeisonebyte(this), if_codeistwobyte(this, Label::kDeferred),
      if_done(this);
  Branch(Int32LessThanOrEqual(code, Int32Constant(String::kMaxOneByteCharCode)),
         &if_codeisonebyte, &if_codeistwobyte);
  BIND(&if_codeisonebyte);
  {
    // Load the isolate wide single character string cache.
    TNode<FixedArray> cache = SingleCharacterStringCacheConstant();
    TNode<IntPtrT> code_index = Signed(ChangeUint32ToWord(code));

    // Check if we have an entry for the {code} in the single character string
    // cache already.
    Label if_entryisundefined(this, Label::kDeferred),
        if_entryisnotundefined(this);
    TNode<Object> entry = UnsafeLoadFixedArrayElement(cache, code_index);
    Branch(IsUndefined(entry), &if_entryisundefined, &if_entryisnotundefined);

    BIND(&if_entryisundefined);
    {
      // Allocate a new SeqOneByteString for {code} and store it in the {cache}.
      TNode<String> result = AllocateSeqOneByteString(1);
      StoreNoWriteBarrier(
          MachineRepresentation::kWord8, result,
          IntPtrConstant(SeqOneByteString::kHeaderSize - kHeapObjectTag), code);
      StoreFixedArrayElement(cache, code_index, result);
      var_result = result;
      Goto(&if_done);
    }

    BIND(&if_entryisnotundefined);
    {
      // Return the entry from the {cache}.
      var_result = CAST(entry);
      Goto(&if_done);
    }
  }

  BIND(&if_codeistwobyte);
  {
    // Allocate a new SeqTwoByteString for {code}.
    TNode<String> result = AllocateSeqTwoByteString(1);
    StoreNoWriteBarrier(
        MachineRepresentation::kWord16, result,
        IntPtrConstant(SeqTwoByteString::kHeaderSize - kHeapObjectTag), code);
    var_result = result;
    Goto(&if_done);
  }

  BIND(&if_done);
  return var_result.value();
}

ToDirectStringAssembler::ToDirectStringAssembler(
    compiler::CodeAssemblerState* state, TNode<String> string, Flags flags)
    : CodeStubAssembler(state),
      var_string_(string, this),
      var_instance_type_(LoadInstanceType(string), this),
      var_offset_(IntPtrConstant(0), this),
      var_is_external_(Int32Constant(0), this),
      flags_(flags) {}

TNode<String> ToDirectStringAssembler::TryToDirect(Label* if_bailout) {
  Label dispatch(this, {&var_string_, &var_offset_, &var_instance_type_});
  Label if_iscons(this);
  Label if_isexternal(this);
  Label if_issliced(this);
  Label if_isthin(this);
  Label out(this);

  Branch(IsSequentialStringInstanceType(var_instance_type_.value()), &out,
         &dispatch);

  // Dispatch based on string representation.
  BIND(&dispatch);
  {
    int32_t values[] = {
        kSeqStringTag,    kConsStringTag, kExternalStringTag,
        kSlicedStringTag, kThinStringTag,
    };
    Label* labels[] = {
        &out, &if_iscons, &if_isexternal, &if_issliced, &if_isthin,
    };
    STATIC_ASSERT(arraysize(values) == arraysize(labels));

    const TNode<Int32T> representation = Word32And(
        var_instance_type_.value(), Int32Constant(kStringRepresentationMask));
    Switch(representation, if_bailout, values, labels, arraysize(values));
  }

  // Cons string.  Check whether it is flat, then fetch first part.
  // Flat cons strings have an empty second part.
  BIND(&if_iscons);
  {
    const TNode<String> string = var_string_.value();
    GotoIfNot(IsEmptyString(
                  LoadObjectField<String>(string, ConsString::kSecondOffset)),
              if_bailout);

    const TNode<String> lhs =
        LoadObjectField<String>(string, ConsString::kFirstOffset);
    var_string_ = lhs;
    var_instance_type_ = LoadInstanceType(lhs);

    Goto(&dispatch);
  }

  // Sliced string. Fetch parent and correct start index by offset.
  BIND(&if_issliced);
  {
    if (!FLAG_string_slices || (flags_ & kDontUnpackSlicedStrings)) {
      Goto(if_bailout);
    } else {
      const TNode<String> string = var_string_.value();
      const TNode<IntPtrT> sliced_offset =
          LoadAndUntagObjectField(string, SlicedString::kOffsetOffset);
      var_offset_ = IntPtrAdd(var_offset_.value(), sliced_offset);

      const TNode<String> parent =
          LoadObjectField<String>(string, SlicedString::kParentOffset);
      var_string_ = parent;
      var_instance_type_ = LoadInstanceType(parent);

      Goto(&dispatch);
    }
  }

  // Thin string. Fetch the actual string.
  BIND(&if_isthin);
  {
    const TNode<String> string = var_string_.value();
    const TNode<String> actual_string =
        LoadObjectField<String>(string, ThinString::kActualOffset);
    const TNode<Uint16T> actual_instance_type = LoadInstanceType(actual_string);

    var_string_ = actual_string;
    var_instance_type_ = actual_instance_type;

    Goto(&dispatch);
  }

  // External string.
  BIND(&if_isexternal);
  var_is_external_ = Int32Constant(1);
  Goto(&out);

  BIND(&out);
  return var_string_.value();
}

TNode<RawPtrT> ToDirectStringAssembler::TryToSequential(
    StringPointerKind ptr_kind, Label* if_bailout) {
  CHECK(ptr_kind == PTR_TO_DATA || ptr_kind == PTR_TO_STRING);

  TVARIABLE(RawPtrT, var_result);
  Label out(this), if_issequential(this), if_isexternal(this, Label::kDeferred);
  Branch(is_external(), &if_isexternal, &if_issequential);

  BIND(&if_issequential);
  {
    STATIC_ASSERT(SeqOneByteString::kHeaderSize ==
                  SeqTwoByteString::kHeaderSize);
    TNode<RawPtrT> result =
        ReinterpretCast<RawPtrT>(BitcastTaggedToWord(var_string_.value()));
    if (ptr_kind == PTR_TO_DATA) {
      result = RawPtrAdd(result, IntPtrConstant(SeqOneByteString::kHeaderSize -
                                                kHeapObjectTag));
    }
    var_result = result;
    Goto(&out);
  }

  BIND(&if_isexternal);
  {
    GotoIf(IsUncachedExternalStringInstanceType(var_instance_type_.value()),
           if_bailout);

    TNode<String> string = var_string_.value();
    TNode<RawPtrT> result = LoadExternalStringResourceDataPtr(CAST(string));
    if (ptr_kind == PTR_TO_STRING) {
      result = RawPtrSub(result, IntPtrConstant(SeqOneByteString::kHeaderSize -
                                                kHeapObjectTag));
    }
    var_result = result;
    Goto(&out);
  }

  BIND(&out);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::StringToNumber(TNode<String> input) {
  Label runtime(this, Label::kDeferred);
  Label end(this);

  TVARIABLE(Number, var_result);

  // Check if string has a cached array index.
  TNode<Uint32T> hash = LoadNameHashField(input);
  GotoIf(IsSetWord32(hash, Name::kDoesNotContainCachedArrayIndexMask),
         &runtime);

  var_result =
      SmiTag(Signed(DecodeWordFromWord32<String::ArrayIndexValueBits>(hash)));
  Goto(&end);

  BIND(&runtime);
  {
    var_result =
        CAST(CallRuntime(Runtime::kStringToNumber, NoContextConstant(), input));
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

TNode<String> CodeStubAssembler::NumberToString(TNode<Number> input,
                                                Label* bailout) {
  TVARIABLE(String, result);
  TVARIABLE(Smi, smi_input);
  Label if_smi(this), if_heap_number(this), done(this, &result);

  // Load the number string cache.
  TNode<FixedArray> number_string_cache = NumberStringCacheConstant();

  // Make the hash mask from the length of the number string cache. It
  // contains two elements (number and string) for each cache entry.
  TNode<IntPtrT> number_string_cache_length =
      LoadAndUntagFixedArrayBaseLength(number_string_cache);
  TNode<Int32T> one = Int32Constant(1);
  TNode<Word32T> mask = Int32Sub(
      Word32Shr(TruncateWordToInt32(number_string_cache_length), one), one);

  GotoIfNot(TaggedIsSmi(input), &if_heap_number);
  smi_input = CAST(input);
  Goto(&if_smi);

  BIND(&if_heap_number);
  {
    Comment("NumberToString - HeapNumber");
    TNode<HeapNumber> heap_number_input = CAST(input);
    // Try normalizing the HeapNumber.
    TryHeapNumberToSmi(heap_number_input, &smi_input, &if_smi);

    // Make a hash from the two 32-bit values of the double.
    TNode<Int32T> low =
        LoadObjectField<Int32T>(heap_number_input, HeapNumber::kValueOffset);
    TNode<Int32T> high = LoadObjectField<Int32T>(
        heap_number_input, HeapNumber::kValueOffset + kIntSize);
    TNode<Word32T> hash = Word32And(Word32Xor(low, high), mask);
    TNode<IntPtrT> entry_index =
        Signed(ChangeUint32ToWord(Int32Add(hash, hash)));

    // Cache entry's key must be a heap number
    TNode<Object> number_key =
        UnsafeLoadFixedArrayElement(number_string_cache, entry_index);
    GotoIf(TaggedIsSmi(number_key), bailout);
    TNode<HeapObject> number_key_heap_object = CAST(number_key);
    GotoIfNot(IsHeapNumber(number_key_heap_object), bailout);

    // Cache entry's key must match the heap number value we're looking for.
    TNode<Int32T> low_compare = LoadObjectField<Int32T>(
        number_key_heap_object, HeapNumber::kValueOffset);
    TNode<Int32T> high_compare = LoadObjectField<Int32T>(
        number_key_heap_object, HeapNumber::kValueOffset + kIntSize);
    GotoIfNot(Word32Equal(low, low_compare), bailout);
    GotoIfNot(Word32Equal(high, high_compare), bailout);

    // Heap number match, return value from cache entry.
    result = CAST(UnsafeLoadFixedArrayElement(number_string_cache, entry_index,
                                              kTaggedSize));
    Goto(&done);
  }

  BIND(&if_smi);
  {
    Comment("NumberToString - Smi");
    // Load the smi key, make sure it matches the smi we're looking for.
    TNode<Word32T> hash = Word32And(SmiToInt32(smi_input.value()), mask);
    TNode<IntPtrT> entry_index =
        Signed(ChangeUint32ToWord(Int32Add(hash, hash)));
    TNode<Object> smi_key =
        UnsafeLoadFixedArrayElement(number_string_cache, entry_index);
    Label if_smi_cache_missed(this);
    GotoIf(TaggedNotEqual(smi_key, smi_input.value()), &if_smi_cache_missed);

    // Smi match, return value from cache entry.
    result = CAST(UnsafeLoadFixedArrayElement(number_string_cache, entry_index,
                                              kTaggedSize));
    Goto(&done);

    BIND(&if_smi_cache_missed);
    {
      Label store_to_cache(this);

      // Bailout when the cache is not full-size.
      const int kFullCacheSize =
          isolate()->heap()->MaxNumberToStringCacheSize();
      Branch(IntPtrLessThan(number_string_cache_length,
                            IntPtrConstant(kFullCacheSize)),
             bailout, &store_to_cache);

      BIND(&store_to_cache);
      {
        // Generate string and update string hash field.
        result = NumberToStringSmi(SmiToInt32(smi_input.value()),
                                   Int32Constant(10), bailout);

        // Store string into cache.
        StoreFixedArrayElement(number_string_cache, entry_index,
                               smi_input.value());
        StoreFixedArrayElement(number_string_cache,
                               IntPtrAdd(entry_index, IntPtrConstant(1)),
                               result.value());
        Goto(&done);
      }
    }
  }
  BIND(&done);
  return result.value();
}

TNode<String> CodeStubAssembler::NumberToString(TNode<Number> input) {
  TVARIABLE(String, result);
  Label runtime(this, Label::kDeferred), done(this, &result);

  GotoIfForceSlowPath(&runtime);

  result = NumberToString(input, &runtime);
  Goto(&done);

  BIND(&runtime);
  {
    // No cache entry, go to the runtime.
    result = CAST(
        CallRuntime(Runtime::kNumberToStringSlow, NoContextConstant(), input));
    Goto(&done);
  }
  BIND(&done);
  return result.value();
}

TNode<Numeric> CodeStubAssembler::NonNumberToNumberOrNumeric(
    TNode<Context> context, TNode<HeapObject> input, Object::Conversion mode,
    BigIntHandling bigint_handling) {
  CSA_ASSERT(this, Word32BinaryNot(IsHeapNumber(input)));

  TVARIABLE(HeapObject, var_input, input);
  TVARIABLE(Numeric, var_result);
  TVARIABLE(Uint16T, instance_type, LoadInstanceType(var_input.value()));
  Label end(this), if_inputisreceiver(this, Label::kDeferred),
      if_inputisnotreceiver(this);

  // We need to handle JSReceiver first since we might need to do two
  // conversions due to ToPritmive.
  Branch(IsJSReceiverInstanceType(instance_type.value()), &if_inputisreceiver,
         &if_inputisnotreceiver);

  BIND(&if_inputisreceiver);
  {
    // The {var_input.value()} is a JSReceiver, we need to convert it to a
    // Primitive first using the ToPrimitive type conversion, preferably
    // yielding a Number.
    Callable callable = CodeFactory::NonPrimitiveToPrimitive(
        isolate(), ToPrimitiveHint::kNumber);
    TNode<Object> result = CallStub(callable, context, var_input.value());

    // Check if the {result} is already a Number/Numeric.
    Label if_done(this), if_notdone(this);
    Branch(mode == Object::Conversion::kToNumber ? IsNumber(result)
                                                 : IsNumeric(result),
           &if_done, &if_notdone);

    BIND(&if_done);
    {
      // The ToPrimitive conversion already gave us a Number/Numeric, so
      // we're done.
      var_result = CAST(result);
      Goto(&end);
    }

    BIND(&if_notdone);
    {
      // We now have a Primitive {result}, but it's not yet a
      // Number/Numeric.
      var_input = CAST(result);
      // We have a new input. Redo the check and reload instance_type.
      CSA_ASSERT(this, Word32BinaryNot(IsHeapNumber(var_input.value())));
      instance_type = LoadInstanceType(var_input.value());
      Goto(&if_inputisnotreceiver);
    }
  }

  BIND(&if_inputisnotreceiver);
  {
    Label not_plain_primitive(this), if_inputisbigint(this),
        if_inputisother(this, Label::kDeferred);

    // String and Oddball cases.
    TVARIABLE(Number, var_result_number);
    TryPlainPrimitiveNonNumberToNumber(var_input.value(), &var_result_number,
                                       &not_plain_primitive);
    var_result = var_result_number.value();
    Goto(&end);

    BIND(&not_plain_primitive);
    {
      Branch(IsBigIntInstanceType(instance_type.value()), &if_inputisbigint,
             &if_inputisother);

      BIND(&if_inputisbigint);
      {
        if (mode == Object::Conversion::kToNumeric) {
          var_result = CAST(var_input.value());
          Goto(&end);
        } else {
          DCHECK_EQ(mode, Object::Conversion::kToNumber);
          if (bigint_handling == BigIntHandling::kThrow) {
            Goto(&if_inputisother);
          } else {
            DCHECK_EQ(bigint_handling, BigIntHandling::kConvertToNumber);
            var_result = CAST(CallRuntime(Runtime::kBigIntToNumber, context,
                                          var_input.value()));
            Goto(&end);
          }
        }
      }

      BIND(&if_inputisother);
      {
        // The {var_input.value()} is something else (e.g. Symbol), let the
        // runtime figure out the correct exception. Note: We cannot tail call
        // to the runtime here, as js-to-wasm trampolines also use this code
        // currently, and they declare all outgoing parameters as untagged,
        // while we would push a tagged object here.
        auto function_id = mode == Object::Conversion::kToNumber
                               ? Runtime::kToNumber
                               : Runtime::kToNumeric;
        var_result = CAST(CallRuntime(function_id, context, var_input.value()));
        Goto(&end);
      }
    }
  }

  BIND(&end);
  if (mode == Object::Conversion::kToNumber) {
    CSA_ASSERT(this, IsNumber(var_result.value()));
  }
  return var_result.value();
}

TNode<Number> CodeStubAssembler::NonNumberToNumber(
    TNode<Context> context, TNode<HeapObject> input,
    BigIntHandling bigint_handling) {
  return CAST(NonNumberToNumberOrNumeric(
      context, input, Object::Conversion::kToNumber, bigint_handling));
}

void CodeStubAssembler::TryPlainPrimitiveNonNumberToNumber(
    TNode<HeapObject> input, TVariable<Number>* var_result, Label* if_bailout) {
  CSA_ASSERT(this, Word32BinaryNot(IsHeapNumber(input)));
  Label done(this);

  // Dispatch on the {input} instance type.
  TNode<Uint16T> input_instance_type = LoadInstanceType(input);
  Label if_inputisstring(this);
  GotoIf(IsStringInstanceType(input_instance_type), &if_inputisstring);
  GotoIfNot(InstanceTypeEqual(input_instance_type, ODDBALL_TYPE), if_bailout);

  // The {input} is an Oddball, we just need to load the Number value of it.
  *var_result = LoadObjectField<Number>(input, Oddball::kToNumberOffset);
  Goto(&done);

  BIND(&if_inputisstring);
  {
    // The {input} is a String, use the fast stub to convert it to a Number.
    *var_result = StringToNumber(CAST(input));
    Goto(&done);
  }

  BIND(&done);
}

TNode<Numeric> CodeStubAssembler::NonNumberToNumeric(TNode<Context> context,
                                                     TNode<HeapObject> input) {
  return NonNumberToNumberOrNumeric(context, input,
                                    Object::Conversion::kToNumeric);
}

TNode<Number> CodeStubAssembler::ToNumber_Inline(TNode<Context> context,
                                                 SloppyTNode<Object> input) {
  TVARIABLE(Number, var_result);
  Label end(this), not_smi(this, Label::kDeferred);

  GotoIfNot(TaggedIsSmi(input), &not_smi);
  var_result = CAST(input);
  Goto(&end);

  BIND(&not_smi);
  {
    var_result = Select<Number>(
        IsHeapNumber(CAST(input)), [=] { return CAST(input); },
        [=] {
          return CAST(
              CallBuiltin(Builtins::kNonNumberToNumber, context, input));
        });
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::ToNumber(TNode<Context> context,
                                          SloppyTNode<Object> input,
                                          BigIntHandling bigint_handling) {
  TVARIABLE(Number, var_result);
  Label end(this);

  Label not_smi(this, Label::kDeferred);
  GotoIfNot(TaggedIsSmi(input), &not_smi);
  TNode<Smi> input_smi = CAST(input);
  var_result = input_smi;
  Goto(&end);

  BIND(&not_smi);
  {
    Label not_heap_number(this, Label::kDeferred);
    TNode<HeapObject> input_ho = CAST(input);
    GotoIfNot(IsHeapNumber(input_ho), &not_heap_number);

    TNode<HeapNumber> input_hn = CAST(input_ho);
    var_result = input_hn;
    Goto(&end);

    BIND(&not_heap_number);
    {
      var_result = NonNumberToNumber(context, input_ho, bigint_handling);
      Goto(&end);
    }
  }

  BIND(&end);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::PlainPrimitiveToNumber(TNode<Object> input) {
  TVARIABLE(Number, var_result);
  Label end(this), fallback(this);

  Label not_smi(this, Label::kDeferred);
  GotoIfNot(TaggedIsSmi(input), &not_smi);
  TNode<Smi> input_smi = CAST(input);
  var_result = input_smi;
  Goto(&end);

  BIND(&not_smi);
  {
    Label not_heap_number(this, Label::kDeferred);
    TNode<HeapObject> input_ho = CAST(input);
    GotoIfNot(IsHeapNumber(input_ho), &not_heap_number);

    TNode<HeapNumber> input_hn = CAST(input_ho);
    var_result = input_hn;
    Goto(&end);

    BIND(&not_heap_number);
    {
      TryPlainPrimitiveNonNumberToNumber(input_ho, &var_result, &fallback);
      Goto(&end);
      BIND(&fallback);
      Unreachable();
    }
  }

  BIND(&end);
  return var_result.value();
}

TNode<BigInt> CodeStubAssembler::ToBigInt(TNode<Context> context,
                                          TNode<Object> input) {
  TVARIABLE(BigInt, var_result);
  Label if_bigint(this), done(this), if_throw(this);

  GotoIf(TaggedIsSmi(input), &if_throw);
  GotoIf(IsBigInt(CAST(input)), &if_bigint);
  var_result = CAST(CallRuntime(Runtime::kToBigInt, context, input));
  Goto(&done);

  BIND(&if_bigint);
  var_result = CAST(input);
  Goto(&done);

  BIND(&if_throw);
  ThrowTypeError(context, MessageTemplate::kBigIntFromObject, input);

  BIND(&done);
  return var_result.value();
}

void CodeStubAssembler::TaggedToNumeric(TNode<Context> context,
                                        TNode<Object> value,
                                        TVariable<Numeric>* var_numeric) {
  TaggedToNumeric(context, value, var_numeric, nullptr);
}

void CodeStubAssembler::TaggedToNumericWithFeedback(
    TNode<Context> context, TNode<Object> value,
    TVariable<Numeric>* var_numeric, TVariable<Smi>* var_feedback) {
  DCHECK_NOT_NULL(var_feedback);
  TaggedToNumeric(context, value, var_numeric, var_feedback);
}

void CodeStubAssembler::TaggedToNumeric(TNode<Context> context,
                                        TNode<Object> value,
                                        TVariable<Numeric>* var_numeric,
                                        TVariable<Smi>* var_feedback) {
  Label done(this), if_smi(this), if_heapnumber(this), if_bigint(this),
      if_oddball(this);
  GotoIf(TaggedIsSmi(value), &if_smi);
  TNode<HeapObject> heap_object_value = CAST(value);
  TNode<Map> map = LoadMap(heap_object_value);
  GotoIf(IsHeapNumberMap(map), &if_heapnumber);
  TNode<Uint16T> instance_type = LoadMapInstanceType(map);
  GotoIf(IsBigIntInstanceType(instance_type), &if_bigint);

  // {heap_object_value} is not a Numeric yet.
  GotoIf(Word32Equal(instance_type, Int32Constant(ODDBALL_TYPE)), &if_oddball);
  *var_numeric = CAST(
      CallBuiltin(Builtins::kNonNumberToNumeric, context, heap_object_value));
  OverwriteFeedback(var_feedback, BinaryOperationFeedback::kAny);
  Goto(&done);

  BIND(&if_smi);
  *var_numeric = CAST(value);
  OverwriteFeedback(var_feedback, BinaryOperationFeedback::kSignedSmall);
  Goto(&done);

  BIND(&if_heapnumber);
  *var_numeric = CAST(value);
  OverwriteFeedback(var_feedback, BinaryOperationFeedback::kNumber);
  Goto(&done);

  BIND(&if_bigint);
  *var_numeric = CAST(value);
  OverwriteFeedback(var_feedback, BinaryOperationFeedback::kBigInt);
  Goto(&done);

  BIND(&if_oddball);
  OverwriteFeedback(var_feedback, BinaryOperationFeedback::kNumberOrOddball);
  *var_numeric =
      CAST(LoadObjectField(heap_object_value, Oddball::kToNumberOffset));
  Goto(&done);

  Bind(&done);
}

// ES#sec-touint32
TNode<Number> CodeStubAssembler::ToUint32(TNode<Context> context,
                                          SloppyTNode<Object> input) {
  const TNode<Float64T> float_zero = Float64Constant(0.0);
  const TNode<Float64T> float_two_32 =
      Float64Constant(static_cast<double>(1ULL << 32));

  Label out(this);

  TVARIABLE(Object, var_result, input);

  // Early exit for positive smis.
  {
    // TODO(jgruber): This branch and the recheck below can be removed once we
    // have a ToNumber with multiple exits.
    Label next(this, Label::kDeferred);
    Branch(TaggedIsPositiveSmi(input), &out, &next);
    BIND(&next);
  }

  const TNode<Number> number = ToNumber(context, input);
  var_result = number;

  // Perhaps we have a positive smi now.
  {
    Label next(this, Label::kDeferred);
    Branch(TaggedIsPositiveSmi(number), &out, &next);
    BIND(&next);
  }

  Label if_isnegativesmi(this), if_isheapnumber(this);
  Branch(TaggedIsSmi(number), &if_isnegativesmi, &if_isheapnumber);

  BIND(&if_isnegativesmi);
  {
    const TNode<Int32T> uint32_value = SmiToInt32(CAST(number));
    TNode<Float64T> float64_value = ChangeUint32ToFloat64(uint32_value);
    var_result = AllocateHeapNumberWithValue(float64_value);
    Goto(&out);
  }

  BIND(&if_isheapnumber);
  {
    Label return_zero(this);
    const TNode<Float64T> value = LoadHeapNumberValue(CAST(number));

    {
      // +-0.
      Label next(this);
      Branch(Float64Equal(value, float_zero), &return_zero, &next);
      BIND(&next);
    }

    {
      // NaN.
      Label next(this);
      Branch(Float64Equal(value, value), &next, &return_zero);
      BIND(&next);
    }

    {
      // +Infinity.
      Label next(this);
      const TNode<Float64T> positive_infinity =
          Float64Constant(std::numeric_limits<double>::infinity());
      Branch(Float64Equal(value, positive_infinity), &return_zero, &next);
      BIND(&next);
    }

    {
      // -Infinity.
      Label next(this);
      const TNode<Float64T> negative_infinity =
          Float64Constant(-1.0 * std::numeric_limits<double>::infinity());
      Branch(Float64Equal(value, negative_infinity), &return_zero, &next);
      BIND(&next);
    }

    // * Let int be the mathematical value that is the same sign as number and
    //   whose magnitude is floor(abs(number)).
    // * Let int32bit be int modulo 2^32.
    // * Return int32bit.
    {
      TNode<Float64T> x = Float64Trunc(value);
      x = Float64Mod(x, float_two_32);
      x = Float64Add(x, float_two_32);
      x = Float64Mod(x, float_two_32);

      const TNode<Number> result = ChangeFloat64ToTagged(x);
      var_result = result;
      Goto(&out);
    }

    BIND(&return_zero);
    {
      var_result = SmiConstant(0);
      Goto(&out);
    }
  }

  BIND(&out);
  return CAST(var_result.value());
}

TNode<String> CodeStubAssembler::ToString_Inline(TNode<Context> context,
                                                 SloppyTNode<Object> input) {
  TVARIABLE(Object, var_result, input);
  Label stub_call(this, Label::kDeferred), out(this);

  GotoIf(TaggedIsSmi(input), &stub_call);
  Branch(IsString(CAST(input)), &out, &stub_call);

  BIND(&stub_call);
  var_result = CallBuiltin(Builtins::kToString, context, input);
  Goto(&out);

  BIND(&out);
  return CAST(var_result.value());
}

TNode<JSReceiver> CodeStubAssembler::ToObject(TNode<Context> context,
                                              SloppyTNode<Object> input) {
  return CAST(CallBuiltin(Builtins::kToObject, context, input));
}

TNode<JSReceiver> CodeStubAssembler::ToObject_Inline(TNode<Context> context,
                                                     TNode<Object> input) {
  TVARIABLE(JSReceiver, result);
  Label if_isreceiver(this), if_isnotreceiver(this, Label::kDeferred);
  Label done(this);

  BranchIfJSReceiver(input, &if_isreceiver, &if_isnotreceiver);

  BIND(&if_isreceiver);
  {
    result = CAST(input);
    Goto(&done);
  }

  BIND(&if_isnotreceiver);
  {
    result = ToObject(context, input);
    Goto(&done);
  }

  BIND(&done);
  return result.value();
}

TNode<Number> CodeStubAssembler::ToLength_Inline(TNode<Context> context,
                                                 SloppyTNode<Object> input) {
  TNode<Smi> smi_zero = SmiConstant(0);
  return Select<Number>(
      TaggedIsSmi(input), [=] { return SmiMax(CAST(input), smi_zero); },
      [=] { return CAST(CallBuiltin(Builtins::kToLength, context, input)); });
}

TNode<Object> CodeStubAssembler::OrdinaryToPrimitive(
    TNode<Context> context, TNode<Object> input, OrdinaryToPrimitiveHint hint) {
  Callable callable = CodeFactory::OrdinaryToPrimitive(isolate(), hint);
  return CallStub(callable, context, input);
}

TNode<Uint32T> CodeStubAssembler::DecodeWord32(TNode<Word32T> word32,
                                               uint32_t shift, uint32_t mask) {
  DCHECK_EQ((mask >> shift) << shift, mask);
  return Unsigned(Word32And(Word32Shr(word32, static_cast<int>(shift)),
                            Int32Constant(mask >> shift)));
}

TNode<UintPtrT> CodeStubAssembler::DecodeWord(SloppyTNode<WordT> word,
                                              uint32_t shift, uintptr_t mask) {
  DCHECK_EQ((mask >> shift) << shift, mask);
  return Unsigned(WordAnd(WordShr(word, static_cast<int>(shift)),
                          IntPtrConstant(mask >> shift)));
}

TNode<Word32T> CodeStubAssembler::UpdateWord32(TNode<Word32T> word,
                                               TNode<Uint32T> value,
                                               uint32_t shift, uint32_t mask,
                                               bool starts_as_zero) {
  DCHECK_EQ((mask >> shift) << shift, mask);
  // Ensure the {value} fits fully in the mask.
  CSA_ASSERT(this, Uint32LessThanOrEqual(value, Uint32Constant(mask >> shift)));
  TNode<Word32T> encoded_value = Word32Shl(value, Int32Constant(shift));
  TNode<Word32T> masked_word;
  if (starts_as_zero) {
    CSA_ASSERT(this, Word32Equal(Word32And(word, Int32Constant(~mask)), word));
    masked_word = word;
  } else {
    masked_word = Word32And(word, Int32Constant(~mask));
  }
  return Word32Or(masked_word, encoded_value);
}

TNode<WordT> CodeStubAssembler::UpdateWord(TNode<WordT> word,
                                           TNode<UintPtrT> value,
                                           uint32_t shift, uintptr_t mask,
                                           bool starts_as_zero) {
  DCHECK_EQ((mask >> shift) << shift, mask);
  // Ensure the {value} fits fully in the mask.
  CSA_ASSERT(this,
             UintPtrLessThanOrEqual(value, UintPtrConstant(mask >> shift)));
  TNode<WordT> encoded_value = WordShl(value, static_cast<int>(shift));
  TNode<WordT> masked_word;
  if (starts_as_zero) {
    CSA_ASSERT(this, WordEqual(WordAnd(word, UintPtrConstant(~mask)), word));
    masked_word = word;
  } else {
    masked_word = WordAnd(word, UintPtrConstant(~mask));
  }
  return WordOr(masked_word, encoded_value);
}

void CodeStubAssembler::SetCounter(StatsCounter* counter, int value) {
  if (FLAG_native_code_counters && counter->Enabled()) {
    TNode<ExternalReference> counter_address =
        ExternalConstant(ExternalReference::Create(counter));
    StoreNoWriteBarrier(MachineRepresentation::kWord32, counter_address,
                        Int32Constant(value));
  }
}

void CodeStubAssembler::IncrementCounter(StatsCounter* counter, int delta) {
  DCHECK_GT(delta, 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    TNode<ExternalReference> counter_address =
        ExternalConstant(ExternalReference::Create(counter));
    // This operation has to be exactly 32-bit wide in case the external
    // reference table redirects the counter to a uint32_t dummy_stats_counter_
    // field.
    TNode<Int32T> value = Load<Int32T>(counter_address);
    value = Int32Add(value, Int32Constant(delta));
    StoreNoWriteBarrier(MachineRepresentation::kWord32, counter_address, value);
  }
}

void CodeStubAssembler::DecrementCounter(StatsCounter* counter, int delta) {
  DCHECK_GT(delta, 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    TNode<ExternalReference> counter_address =
        ExternalConstant(ExternalReference::Create(counter));
    // This operation has to be exactly 32-bit wide in case the external
    // reference table redirects the counter to a uint32_t dummy_stats_counter_
    // field.
    TNode<Int32T> value = Load<Int32T>(counter_address);
    value = Int32Sub(value, Int32Constant(delta));
    StoreNoWriteBarrier(MachineRepresentation::kWord32, counter_address, value);
  }
}

template <typename TIndex>
void CodeStubAssembler::Increment(TVariable<TIndex>* variable, int value) {
  *variable =
      IntPtrOrSmiAdd(variable->value(), IntPtrOrSmiConstant<TIndex>(value));
}

// Instantiate Increment for Smi and IntPtrT.
// TODO(v8:9708): Consider renaming to [Smi|IntPtrT|RawPtrT]Increment.
template void CodeStubAssembler::Increment<Smi>(TVariable<Smi>* variable,
                                                int value);
template void CodeStubAssembler::Increment<IntPtrT>(
    TVariable<IntPtrT>* variable, int value);
template void CodeStubAssembler::Increment<RawPtrT>(
    TVariable<RawPtrT>* variable, int value);

void CodeStubAssembler::Use(Label* label) {
  GotoIf(Word32Equal(Int32Constant(0), Int32Constant(1)), label);
}

void CodeStubAssembler::TryToName(SloppyTNode<Object> key, Label* if_keyisindex,
                                  TVariable<IntPtrT>* var_index,
                                  Label* if_keyisunique,
                                  TVariable<Name>* var_unique,
                                  Label* if_bailout,
                                  Label* if_notinternalized) {
  Comment("TryToName");

  TVARIABLE(Int32T, var_instance_type);
  Label if_keyisnotindex(this);
  *var_index = TryToIntptr(key, &if_keyisnotindex, &var_instance_type);
  Goto(if_keyisindex);

  BIND(&if_keyisnotindex);
  {
    Label if_symbol(this), if_string(this),
        if_keyisother(this, Label::kDeferred);

    // Symbols are unique.
    GotoIf(IsSymbolInstanceType(var_instance_type.value()), &if_symbol);

    // Miss if |key| is not a String.
    STATIC_ASSERT(FIRST_NAME_TYPE == FIRST_TYPE);
    Branch(IsStringInstanceType(var_instance_type.value()), &if_string,
           &if_keyisother);

    // Symbols are unique.
    BIND(&if_symbol);
    {
      *var_unique = CAST(key);
      Goto(if_keyisunique);
    }

    BIND(&if_string);
    {
      Label if_thinstring(this), if_has_cached_index(this);

      TNode<Uint32T> hash = LoadNameHashField(CAST(key));
      GotoIf(IsClearWord32(hash, Name::kDoesNotContainCachedArrayIndexMask),
             &if_has_cached_index);
      // No cached array index. If the string knows that it contains an index,
      // then it must be an uncacheable index. Handle this case in the runtime.
      GotoIf(IsClearWord32(hash, Name::kIsNotIntegerIndexMask), if_bailout);

      GotoIf(InstanceTypeEqual(var_instance_type.value(), THIN_STRING_TYPE),
             &if_thinstring);
      GotoIf(InstanceTypeEqual(var_instance_type.value(),
                               THIN_ONE_BYTE_STRING_TYPE),
             &if_thinstring);
      // Finally, check if |key| is internalized.
      STATIC_ASSERT(kNotInternalizedTag != 0);
      GotoIf(IsSetWord32(var_instance_type.value(), kIsNotInternalizedMask),
             if_notinternalized != nullptr ? if_notinternalized : if_bailout);

      *var_unique = CAST(key);
      Goto(if_keyisunique);

      BIND(&if_thinstring);
      {
        *var_unique =
            LoadObjectField<String>(CAST(key), ThinString::kActualOffset);
        Goto(if_keyisunique);
      }

      BIND(&if_has_cached_index);
      {
        TNode<IntPtrT> index =
            Signed(DecodeWordFromWord32<String::ArrayIndexValueBits>(hash));
        CSA_ASSERT(this, IntPtrLessThan(index, IntPtrConstant(INT_MAX)));
        *var_index = index;
        Goto(if_keyisindex);
      }
    }

    BIND(&if_keyisother);
    {
      GotoIfNot(InstanceTypeEqual(var_instance_type.value(), ODDBALL_TYPE),
                if_bailout);
      *var_unique =
          LoadObjectField<String>(CAST(key), Oddball::kToStringOffset);
      Goto(if_keyisunique);
    }
  }
}

void CodeStubAssembler::TryInternalizeString(
    TNode<String> string, Label* if_index, TVariable<IntPtrT>* var_index,
    Label* if_internalized, TVariable<Name>* var_internalized,
    Label* if_not_internalized, Label* if_bailout) {
  TNode<ExternalReference> function = ExternalConstant(
      ExternalReference::try_string_to_index_or_lookup_existing());
  const TNode<ExternalReference> isolate_ptr =
      ExternalConstant(ExternalReference::isolate_address(isolate()));
  TNode<Object> result =
      CAST(CallCFunction(function, MachineType::AnyTagged(),
                         std::make_pair(MachineType::Pointer(), isolate_ptr),
                         std::make_pair(MachineType::AnyTagged(), string)));
  Label internalized(this);
  GotoIf(TaggedIsNotSmi(result), &internalized);
  TNode<IntPtrT> word_result = SmiUntag(CAST(result));
  GotoIf(IntPtrEqual(word_result, IntPtrConstant(ResultSentinel::kNotFound)),
         if_not_internalized);
  GotoIf(IntPtrEqual(word_result, IntPtrConstant(ResultSentinel::kUnsupported)),
         if_bailout);
  *var_index = word_result;
  Goto(if_index);

  BIND(&internalized);
  *var_internalized = CAST(result);
  Goto(if_internalized);
}

template <typename Dictionary>
TNode<IntPtrT> CodeStubAssembler::EntryToIndex(TNode<IntPtrT> entry,
                                               int field_index) {
  TNode<IntPtrT> entry_index =
      IntPtrMul(entry, IntPtrConstant(Dictionary::kEntrySize));
  return IntPtrAdd(entry_index, IntPtrConstant(Dictionary::kElementsStartIndex +
                                               field_index));
}

template <typename T>
TNode<T> CodeStubAssembler::LoadDescriptorArrayElement(
    TNode<DescriptorArray> object, TNode<IntPtrT> index,
    int additional_offset) {
  return LoadArrayElement<DescriptorArray, IntPtrT, T>(
      object, DescriptorArray::kHeaderSize, index, additional_offset);
}

TNode<Name> CodeStubAssembler::LoadKeyByKeyIndex(
    TNode<DescriptorArray> container, TNode<IntPtrT> key_index) {
  return CAST(LoadDescriptorArrayElement<HeapObject>(container, key_index, 0));
}

TNode<Uint32T> CodeStubAssembler::LoadDetailsByKeyIndex(
    TNode<DescriptorArray> container, TNode<IntPtrT> key_index) {
  const int kKeyToDetailsOffset =
      DescriptorArray::kEntryDetailsOffset - DescriptorArray::kEntryKeyOffset;
  return Unsigned(LoadAndUntagToWord32ArrayElement(
      container, DescriptorArray::kHeaderSize, key_index, kKeyToDetailsOffset));
}

TNode<Object> CodeStubAssembler::LoadValueByKeyIndex(
    TNode<DescriptorArray> container, TNode<IntPtrT> key_index) {
  const int kKeyToValueOffset =
      DescriptorArray::kEntryValueOffset - DescriptorArray::kEntryKeyOffset;
  return LoadDescriptorArrayElement<Object>(container, key_index,
                                            kKeyToValueOffset);
}

TNode<MaybeObject> CodeStubAssembler::LoadFieldTypeByKeyIndex(
    TNode<DescriptorArray> container, TNode<IntPtrT> key_index) {
  const int kKeyToValueOffset =
      DescriptorArray::kEntryValueOffset - DescriptorArray::kEntryKeyOffset;
  return LoadDescriptorArrayElement<MaybeObject>(container, key_index,
                                                 kKeyToValueOffset);
}

TNode<IntPtrT> CodeStubAssembler::DescriptorEntryToIndex(
    TNode<IntPtrT> descriptor_entry) {
  return IntPtrMul(descriptor_entry,
                   IntPtrConstant(DescriptorArray::kEntrySize));
}

TNode<Name> CodeStubAssembler::LoadKeyByDescriptorEntry(
    TNode<DescriptorArray> container, TNode<IntPtrT> descriptor_entry) {
  return CAST(LoadDescriptorArrayElement<HeapObject>(
      container, DescriptorEntryToIndex(descriptor_entry),
      DescriptorArray::ToKeyIndex(0) * kTaggedSize));
}

TNode<Name> CodeStubAssembler::LoadKeyByDescriptorEntry(
    TNode<DescriptorArray> container, int descriptor_entry) {
  return CAST(LoadDescriptorArrayElement<HeapObject>(
      container, IntPtrConstant(0),
      DescriptorArray::ToKeyIndex(descriptor_entry) * kTaggedSize));
}

TNode<Uint32T> CodeStubAssembler::LoadDetailsByDescriptorEntry(
    TNode<DescriptorArray> container, TNode<IntPtrT> descriptor_entry) {
  return Unsigned(LoadAndUntagToWord32ArrayElement(
      container, DescriptorArray::kHeaderSize,
      DescriptorEntryToIndex(descriptor_entry),
      DescriptorArray::ToDetailsIndex(0) * kTaggedSize));
}

TNode<Uint32T> CodeStubAssembler::LoadDetailsByDescriptorEntry(
    TNode<DescriptorArray> container, int descriptor_entry) {
  return Unsigned(LoadAndUntagToWord32ArrayElement(
      container, DescriptorArray::kHeaderSize, IntPtrConstant(0),
      DescriptorArray::ToDetailsIndex(descriptor_entry) * kTaggedSize));
}

TNode<Object> CodeStubAssembler::LoadValueByDescriptorEntry(
    TNode<DescriptorArray> container, int descriptor_entry) {
  return LoadDescriptorArrayElement<Object>(
      container, IntPtrConstant(0),
      DescriptorArray::ToValueIndex(descriptor_entry) * kTaggedSize);
}

TNode<MaybeObject> CodeStubAssembler::LoadFieldTypeByDescriptorEntry(
    TNode<DescriptorArray> container, TNode<IntPtrT> descriptor_entry) {
  return LoadDescriptorArrayElement<MaybeObject>(
      container, DescriptorEntryToIndex(descriptor_entry),
      DescriptorArray::ToValueIndex(0) * kTaggedSize);
}

template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::EntryToIndex<NameDictionary>(TNode<IntPtrT>, int);
template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::EntryToIndex<GlobalDictionary>(TNode<IntPtrT>, int);
template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::EntryToIndex<NumberDictionary>(TNode<IntPtrT>, int);

// This must be kept in sync with HashTableBase::ComputeCapacity().
TNode<IntPtrT> CodeStubAssembler::HashTableComputeCapacity(
    TNode<IntPtrT> at_least_space_for) {
  TNode<IntPtrT> capacity = IntPtrRoundUpToPowerOfTwo32(
      IntPtrAdd(at_least_space_for, WordShr(at_least_space_for, 1)));
  return IntPtrMax(capacity, IntPtrConstant(HashTableBase::kMinCapacity));
}

TNode<IntPtrT> CodeStubAssembler::IntPtrMax(SloppyTNode<IntPtrT> left,
                                            SloppyTNode<IntPtrT> right) {
  intptr_t left_constant;
  intptr_t right_constant;
  if (ToIntPtrConstant(left, &left_constant) &&
      ToIntPtrConstant(right, &right_constant)) {
    return IntPtrConstant(std::max(left_constant, right_constant));
  }
  return SelectConstant<IntPtrT>(IntPtrGreaterThanOrEqual(left, right), left,
                                 right);
}

TNode<IntPtrT> CodeStubAssembler::IntPtrMin(SloppyTNode<IntPtrT> left,
                                            SloppyTNode<IntPtrT> right) {
  intptr_t left_constant;
  intptr_t right_constant;
  if (ToIntPtrConstant(left, &left_constant) &&
      ToIntPtrConstant(right, &right_constant)) {
    return IntPtrConstant(std::min(left_constant, right_constant));
  }
  return SelectConstant<IntPtrT>(IntPtrLessThanOrEqual(left, right), left,
                                 right);
}

TNode<UintPtrT> CodeStubAssembler::UintPtrMin(TNode<UintPtrT> left,
                                              TNode<UintPtrT> right) {
  intptr_t left_constant;
  intptr_t right_constant;
  if (ToIntPtrConstant(left, &left_constant) &&
      ToIntPtrConstant(right, &right_constant)) {
    return UintPtrConstant(std::min(static_cast<uintptr_t>(left_constant),
                                    static_cast<uintptr_t>(right_constant)));
  }
  return SelectConstant<UintPtrT>(UintPtrLessThanOrEqual(left, right), left,
                                  right);
}

template <>
TNode<HeapObject> CodeStubAssembler::LoadName<NameDictionary>(
    TNode<HeapObject> key) {
  CSA_ASSERT(this, Word32Or(IsTheHole(key), IsName(key)));
  return key;
}

template <>
TNode<HeapObject> CodeStubAssembler::LoadName<GlobalDictionary>(
    TNode<HeapObject> key) {
  TNode<PropertyCell> property_cell = CAST(key);
  return CAST(LoadObjectField(property_cell, PropertyCell::kNameOffset));
}

template <typename Dictionary>
void CodeStubAssembler::NameDictionaryLookup(
    TNode<Dictionary> dictionary, TNode<Name> unique_name, Label* if_found,
    TVariable<IntPtrT>* var_name_index, Label* if_not_found, LookupMode mode) {
  static_assert(std::is_same<Dictionary, NameDictionary>::value ||
                    std::is_same<Dictionary, GlobalDictionary>::value,
                "Unexpected NameDictionary");
  DCHECK_EQ(MachineType::PointerRepresentation(), var_name_index->rep());
  DCHECK_IMPLIES(mode == kFindInsertionIndex, if_found == nullptr);
  Comment("NameDictionaryLookup");
  CSA_ASSERT(this, IsUniqueName(unique_name));

  TNode<IntPtrT> capacity = SmiUntag(GetCapacity<Dictionary>(dictionary));
  TNode<IntPtrT> mask = IntPtrSub(capacity, IntPtrConstant(1));
  TNode<UintPtrT> hash = ChangeUint32ToWord(LoadNameHash(unique_name));

  // See Dictionary::FirstProbe().
  TNode<IntPtrT> count = IntPtrConstant(0);
  TNode<IntPtrT> entry = Signed(WordAnd(hash, mask));
  TNode<Oddball> undefined = UndefinedConstant();

  // Appease the variable merging algorithm for "Goto(&loop)" below.
  *var_name_index = IntPtrConstant(0);

  TVARIABLE(IntPtrT, var_count, count);
  TVARIABLE(IntPtrT, var_entry, entry);
  Label loop(this, {&var_count, &var_entry, var_name_index});
  Goto(&loop);
  BIND(&loop);
  {
    Label next_probe(this);
    TNode<IntPtrT> entry = var_entry.value();

    TNode<IntPtrT> index = EntryToIndex<Dictionary>(entry);
    *var_name_index = index;

    TNode<HeapObject> current =
        CAST(UnsafeLoadFixedArrayElement(dictionary, index));
    GotoIf(TaggedEqual(current, undefined), if_not_found);
    if (mode == kFindExisting) {
      if (Dictionary::ShapeT::kMatchNeedsHoleCheck) {
        GotoIf(TaggedEqual(current, TheHoleConstant()), &next_probe);
      }
      current = LoadName<Dictionary>(current);
      GotoIf(TaggedEqual(current, unique_name), if_found);
    } else {
      DCHECK_EQ(kFindInsertionIndex, mode);
      GotoIf(TaggedEqual(current, TheHoleConstant()), if_not_found);
    }
    Goto(&next_probe);

    BIND(&next_probe);
    // See Dictionary::NextProbe().
    Increment(&var_count);
    entry = Signed(WordAnd(IntPtrAdd(entry, var_count.value()), mask));

    var_entry = entry;
    Goto(&loop);
  }
}

// Instantiate template methods to workaround GCC compilation issue.
template V8_EXPORT_PRIVATE void
CodeStubAssembler::NameDictionaryLookup<NameDictionary>(TNode<NameDictionary>,
                                                        TNode<Name>, Label*,
                                                        TVariable<IntPtrT>*,
                                                        Label*, LookupMode);
template V8_EXPORT_PRIVATE void CodeStubAssembler::NameDictionaryLookup<
    GlobalDictionary>(TNode<GlobalDictionary>, TNode<Name>, Label*,
                      TVariable<IntPtrT>*, Label*, LookupMode);

TNode<Word32T> CodeStubAssembler::ComputeSeededHash(TNode<IntPtrT> key) {
  const TNode<ExternalReference> function_addr =
      ExternalConstant(ExternalReference::compute_integer_hash());
  const TNode<ExternalReference> isolate_ptr =
      ExternalConstant(ExternalReference::isolate_address(isolate()));

  MachineType type_ptr = MachineType::Pointer();
  MachineType type_uint32 = MachineType::Uint32();
  MachineType type_int32 = MachineType::Int32();

  return UncheckedCast<Word32T>(CallCFunction(
      function_addr, type_uint32, std::make_pair(type_ptr, isolate_ptr),
      std::make_pair(type_int32, TruncateIntPtrToInt32(key))));
}

void CodeStubAssembler::NumberDictionaryLookup(
    TNode<NumberDictionary> dictionary, TNode<IntPtrT> intptr_index,
    Label* if_found, TVariable<IntPtrT>* var_entry, Label* if_not_found) {
  CSA_ASSERT(this, IsNumberDictionary(dictionary));
  DCHECK_EQ(MachineType::PointerRepresentation(), var_entry->rep());
  Comment("NumberDictionaryLookup");

  TNode<IntPtrT> capacity = SmiUntag(GetCapacity<NumberDictionary>(dictionary));
  TNode<IntPtrT> mask = IntPtrSub(capacity, IntPtrConstant(1));

  TNode<UintPtrT> hash = ChangeUint32ToWord(ComputeSeededHash(intptr_index));
  TNode<Float64T> key_as_float64 = RoundIntPtrToFloat64(intptr_index);

  // See Dictionary::FirstProbe().
  TNode<IntPtrT> count = IntPtrConstant(0);
  TNode<IntPtrT> entry = Signed(WordAnd(hash, mask));

  TNode<Oddball> undefined = UndefinedConstant();
  TNode<Oddball> the_hole = TheHoleConstant();

  TVARIABLE(IntPtrT, var_count, count);
  Label loop(this, {&var_count, var_entry});
  *var_entry = entry;
  Goto(&loop);
  BIND(&loop);
  {
    TNode<IntPtrT> entry = var_entry->value();

    TNode<IntPtrT> index = EntryToIndex<NumberDictionary>(entry);
    TNode<Object> current = UnsafeLoadFixedArrayElement(dictionary, index);
    GotoIf(TaggedEqual(current, undefined), if_not_found);
    Label next_probe(this);
    {
      Label if_currentissmi(this), if_currentisnotsmi(this);
      Branch(TaggedIsSmi(current), &if_currentissmi, &if_currentisnotsmi);
      BIND(&if_currentissmi);
      {
        TNode<IntPtrT> current_value = SmiUntag(CAST(current));
        Branch(WordEqual(current_value, intptr_index), if_found, &next_probe);
      }
      BIND(&if_currentisnotsmi);
      {
        GotoIf(TaggedEqual(current, the_hole), &next_probe);
        // Current must be the Number.
        TNode<Float64T> current_value = LoadHeapNumberValue(CAST(current));
        Branch(Float64Equal(current_value, key_as_float64), if_found,
               &next_probe);
      }
    }

    BIND(&next_probe);
    // See Dictionary::NextProbe().
    Increment(&var_count);
    entry = Signed(WordAnd(IntPtrAdd(entry, var_count.value()), mask));

    *var_entry = entry;
    Goto(&loop);
  }
}

TNode<Object> CodeStubAssembler::BasicLoadNumberDictionaryElement(
    TNode<NumberDictionary> dictionary, TNode<IntPtrT> intptr_index,
    Label* not_data, Label* if_hole) {
  TVARIABLE(IntPtrT, var_entry);
  Label if_found(this);
  NumberDictionaryLookup(dictionary, intptr_index, &if_found, &var_entry,
                         if_hole);
  BIND(&if_found);

  // Check that the value is a data property.
  TNode<IntPtrT> index = EntryToIndex<NumberDictionary>(var_entry.value());
  TNode<Uint32T> details = LoadDetailsByKeyIndex(dictionary, index);
  TNode<Uint32T> kind = DecodeWord32<PropertyDetails::KindField>(details);
  // TODO(jkummerow): Support accessors without missing?
  GotoIfNot(Word32Equal(kind, Int32Constant(kData)), not_data);
  // Finally, load the value.
  return LoadValueByKeyIndex(dictionary, index);
}

template <class Dictionary>
void CodeStubAssembler::FindInsertionEntry(TNode<Dictionary> dictionary,
                                           TNode<Name> key,
                                           TVariable<IntPtrT>* var_key_index) {
  UNREACHABLE();
}

template <>
void CodeStubAssembler::FindInsertionEntry<NameDictionary>(
    TNode<NameDictionary> dictionary, TNode<Name> key,
    TVariable<IntPtrT>* var_key_index) {
  Label done(this);
  NameDictionaryLookup<NameDictionary>(dictionary, key, nullptr, var_key_index,
                                       &done, kFindInsertionIndex);
  BIND(&done);
}

template <class Dictionary>
void CodeStubAssembler::InsertEntry(TNode<Dictionary> dictionary,
                                    TNode<Name> key, TNode<Object> value,
                                    TNode<IntPtrT> index,
                                    TNode<Smi> enum_index) {
  UNREACHABLE();  // Use specializations instead.
}

template <>
void CodeStubAssembler::InsertEntry<NameDictionary>(
    TNode<NameDictionary> dictionary, TNode<Name> name, TNode<Object> value,
    TNode<IntPtrT> index, TNode<Smi> enum_index) {
  // Store name and value.
  StoreFixedArrayElement(dictionary, index, name);
  StoreValueByKeyIndex<NameDictionary>(dictionary, index, value);

  // Prepare details of the new property.
  PropertyDetails d(kData, NONE, PropertyCellType::kNoCell);
  enum_index =
      SmiShl(enum_index, PropertyDetails::DictionaryStorageField::kShift);
  // We OR over the actual index below, so we expect the initial value to be 0.
  DCHECK_EQ(0, d.dictionary_index());
  TVARIABLE(Smi, var_details, SmiOr(SmiConstant(d.AsSmi()), enum_index));

  // Private names must be marked non-enumerable.
  Label not_private(this, &var_details);
  GotoIfNot(IsPrivateSymbol(name), &not_private);
  TNode<Smi> dont_enum =
      SmiShl(SmiConstant(DONT_ENUM), PropertyDetails::AttributesField::kShift);
  var_details = SmiOr(var_details.value(), dont_enum);
  Goto(&not_private);
  BIND(&not_private);

  // Finally, store the details.
  StoreDetailsByKeyIndex<NameDictionary>(dictionary, index,
                                         var_details.value());
}

template <>
void CodeStubAssembler::InsertEntry<GlobalDictionary>(
    TNode<GlobalDictionary> dictionary, TNode<Name> key, TNode<Object> value,
    TNode<IntPtrT> index, TNode<Smi> enum_index) {
  UNIMPLEMENTED();
}

template <class Dictionary>
void CodeStubAssembler::Add(TNode<Dictionary> dictionary, TNode<Name> key,
                            TNode<Object> value, Label* bailout) {
  CSA_ASSERT(this, Word32BinaryNot(IsEmptyPropertyDictionary(dictionary)));
  TNode<Smi> capacity = GetCapacity<Dictionary>(dictionary);
  TNode<Smi> nof = GetNumberOfElements<Dictionary>(dictionary);
  TNode<Smi> new_nof = SmiAdd(nof, SmiConstant(1));
  // Require 33% to still be free after adding additional_elements.
  // Computing "x + (x >> 1)" on a Smi x does not return a valid Smi!
  // But that's OK here because it's only used for a comparison.
  TNode<Smi> required_capacity_pseudo_smi = SmiAdd(new_nof, SmiShr(new_nof, 1));
  GotoIf(SmiBelow(capacity, required_capacity_pseudo_smi), bailout);
  // Require rehashing if more than 50% of free elements are deleted elements.
  TNode<Smi> deleted = GetNumberOfDeletedElements<Dictionary>(dictionary);
  CSA_ASSERT(this, SmiAbove(capacity, new_nof));
  TNode<Smi> half_of_free_elements = SmiShr(SmiSub(capacity, new_nof), 1);
  GotoIf(SmiAbove(deleted, half_of_free_elements), bailout);

  TNode<Smi> enum_index = GetNextEnumerationIndex<Dictionary>(dictionary);
  TNode<Smi> new_enum_index = SmiAdd(enum_index, SmiConstant(1));
  TNode<Smi> max_enum_index =
      SmiConstant(PropertyDetails::DictionaryStorageField::kMax);
  GotoIf(SmiAbove(new_enum_index, max_enum_index), bailout);

  // No more bailouts after this point.
  // Operations from here on can have side effects.

  SetNextEnumerationIndex<Dictionary>(dictionary, new_enum_index);
  SetNumberOfElements<Dictionary>(dictionary, new_nof);

  TVARIABLE(IntPtrT, var_key_index);
  FindInsertionEntry<Dictionary>(dictionary, key, &var_key_index);
  InsertEntry<Dictionary>(dictionary, key, value, var_key_index.value(),
                          enum_index);
}

template void CodeStubAssembler::Add<NameDictionary>(TNode<NameDictionary>,
                                                     TNode<Name>, TNode<Object>,
                                                     Label*);

template <typename Array>
void CodeStubAssembler::LookupLinear(TNode<Name> unique_name,
                                     TNode<Array> array,
                                     TNode<Uint32T> number_of_valid_entries,
                                     Label* if_found,
                                     TVariable<IntPtrT>* var_name_index,
                                     Label* if_not_found) {
  static_assert(std::is_base_of<FixedArray, Array>::value ||
                    std::is_base_of<WeakFixedArray, Array>::value ||
                    std::is_base_of<DescriptorArray, Array>::value,
                "T must be a descendant of FixedArray or a WeakFixedArray");
  Comment("LookupLinear");
  CSA_ASSERT(this, IsUniqueName(unique_name));
  TNode<IntPtrT> first_inclusive = IntPtrConstant(Array::ToKeyIndex(0));
  TNode<IntPtrT> factor = IntPtrConstant(Array::kEntrySize);
  TNode<IntPtrT> last_exclusive = IntPtrAdd(
      first_inclusive,
      IntPtrMul(ChangeInt32ToIntPtr(number_of_valid_entries), factor));

  BuildFastLoop<IntPtrT>(
      last_exclusive, first_inclusive,
      [=](TNode<IntPtrT> name_index) {
        TNode<MaybeObject> element =
            LoadArrayElement(array, Array::kHeaderSize, name_index);
        TNode<Name> candidate_name = CAST(element);
        *var_name_index = name_index;
        GotoIf(TaggedEqual(candidate_name, unique_name), if_found);
      },
      -Array::kEntrySize, IndexAdvanceMode::kPre);
  Goto(if_not_found);
}

template <>
TNode<Uint32T> CodeStubAssembler::NumberOfEntries<DescriptorArray>(
    TNode<DescriptorArray> descriptors) {
  return Unsigned(LoadNumberOfDescriptors(descriptors));
}

template <>
TNode<Uint32T> CodeStubAssembler::NumberOfEntries<TransitionArray>(
    TNode<TransitionArray> transitions) {
  TNode<IntPtrT> length = LoadAndUntagWeakFixedArrayLength(transitions);
  return Select<Uint32T>(
      UintPtrLessThan(length, IntPtrConstant(TransitionArray::kFirstIndex)),
      [=] { return Unsigned(Int32Constant(0)); },
      [=] {
        return Unsigned(LoadAndUntagToWord32ArrayElement(
            transitions, WeakFixedArray::kHeaderSize,
            IntPtrConstant(TransitionArray::kTransitionLengthIndex)));
      });
}

template <typename Array>
TNode<IntPtrT> CodeStubAssembler::EntryIndexToIndex(
    TNode<Uint32T> entry_index) {
  TNode<Int32T> entry_size = Int32Constant(Array::kEntrySize);
  TNode<Word32T> index = Int32Mul(entry_index, entry_size);
  return ChangeInt32ToIntPtr(index);
}

template <typename Array>
TNode<IntPtrT> CodeStubAssembler::ToKeyIndex(TNode<Uint32T> entry_index) {
  return IntPtrAdd(IntPtrConstant(Array::ToKeyIndex(0)),
                   EntryIndexToIndex<Array>(entry_index));
}

template TNode<IntPtrT> CodeStubAssembler::ToKeyIndex<DescriptorArray>(
    TNode<Uint32T>);
template TNode<IntPtrT> CodeStubAssembler::ToKeyIndex<TransitionArray>(
    TNode<Uint32T>);

template <>
TNode<Uint32T> CodeStubAssembler::GetSortedKeyIndex<DescriptorArray>(
    TNode<DescriptorArray> descriptors, TNode<Uint32T> descriptor_number) {
  TNode<Uint32T> details =
      DescriptorArrayGetDetails(descriptors, descriptor_number);
  return DecodeWord32<PropertyDetails::DescriptorPointer>(details);
}

template <>
TNode<Uint32T> CodeStubAssembler::GetSortedKeyIndex<TransitionArray>(
    TNode<TransitionArray> transitions, TNode<Uint32T> transition_number) {
  return transition_number;
}

template <typename Array>
TNode<Name> CodeStubAssembler::GetKey(TNode<Array> array,
                                      TNode<Uint32T> entry_index) {
  static_assert(std::is_base_of<TransitionArray, Array>::value ||
                    std::is_base_of<DescriptorArray, Array>::value,
                "T must be a descendant of DescriptorArray or TransitionArray");
  const int key_offset = Array::ToKeyIndex(0) * kTaggedSize;
  TNode<MaybeObject> element =
      LoadArrayElement(array, Array::kHeaderSize,
                       EntryIndexToIndex<Array>(entry_index), key_offset);
  return CAST(element);
}

template TNode<Name> CodeStubAssembler::GetKey<DescriptorArray>(
    TNode<DescriptorArray>, TNode<Uint32T>);
template TNode<Name> CodeStubAssembler::GetKey<TransitionArray>(
    TNode<TransitionArray>, TNode<Uint32T>);

TNode<Uint32T> CodeStubAssembler::DescriptorArrayGetDetails(
    TNode<DescriptorArray> descriptors, TNode<Uint32T> descriptor_number) {
  const int details_offset = DescriptorArray::ToDetailsIndex(0) * kTaggedSize;
  return Unsigned(LoadAndUntagToWord32ArrayElement(
      descriptors, DescriptorArray::kHeaderSize,
      EntryIndexToIndex<DescriptorArray>(descriptor_number), details_offset));
}

template <typename Array>
void CodeStubAssembler::LookupBinary(TNode<Name> unique_name,
                                     TNode<Array> array,
                                     TNode<Uint32T> number_of_valid_entries,
                                     Label* if_found,
                                     TVariable<IntPtrT>* var_name_index,
                                     Label* if_not_found) {
  Comment("LookupBinary");
  TVARIABLE(Uint32T, var_low, Unsigned(Int32Constant(0)));
  TNode<Uint32T> limit =
      Unsigned(Int32Sub(NumberOfEntries<Array>(array), Int32Constant(1)));
  TVARIABLE(Uint32T, var_high, limit);
  TNode<Uint32T> hash = LoadNameHashAssumeComputed(unique_name);
  CSA_ASSERT(this, Word32NotEqual(hash, Int32Constant(0)));

  // Assume non-empty array.
  CSA_ASSERT(this, Uint32LessThanOrEqual(var_low.value(), var_high.value()));

  Label binary_loop(this, {&var_high, &var_low});
  Goto(&binary_loop);
  BIND(&binary_loop);
  {
    // mid = low + (high - low) / 2 (to avoid overflow in "(low + high) / 2").
    TNode<Uint32T> mid = Unsigned(
        Int32Add(var_low.value(),
                 Word32Shr(Int32Sub(var_high.value(), var_low.value()), 1)));
    // mid_name = array->GetSortedKey(mid).
    TNode<Uint32T> sorted_key_index = GetSortedKeyIndex<Array>(array, mid);
    TNode<Name> mid_name = GetKey<Array>(array, sorted_key_index);

    TNode<Uint32T> mid_hash = LoadNameHashAssumeComputed(mid_name);

    Label mid_greater(this), mid_less(this), merge(this);
    Branch(Uint32GreaterThanOrEqual(mid_hash, hash), &mid_greater, &mid_less);
    BIND(&mid_greater);
    {
      var_high = mid;
      Goto(&merge);
    }
    BIND(&mid_less);
    {
      var_low = Unsigned(Int32Add(mid, Int32Constant(1)));
      Goto(&merge);
    }
    BIND(&merge);
    GotoIf(Word32NotEqual(var_low.value(), var_high.value()), &binary_loop);
  }

  Label scan_loop(this, &var_low);
  Goto(&scan_loop);
  BIND(&scan_loop);
  {
    GotoIf(Int32GreaterThan(var_low.value(), limit), if_not_found);

    TNode<Uint32T> sort_index =
        GetSortedKeyIndex<Array>(array, var_low.value());
    TNode<Name> current_name = GetKey<Array>(array, sort_index);
    TNode<Uint32T> current_hash = LoadNameHashAssumeComputed(current_name);
    GotoIf(Word32NotEqual(current_hash, hash), if_not_found);
    Label next(this);
    GotoIf(TaggedNotEqual(current_name, unique_name), &next);
    GotoIf(Uint32GreaterThanOrEqual(sort_index, number_of_valid_entries),
           if_not_found);
    *var_name_index = ToKeyIndex<Array>(sort_index);
    Goto(if_found);

    BIND(&next);
    var_low = Unsigned(Int32Add(var_low.value(), Int32Constant(1)));
    Goto(&scan_loop);
  }
}

void CodeStubAssembler::ForEachEnumerableOwnProperty(
    TNode<Context> context, TNode<Map> map, TNode<JSObject> object,
    ForEachEnumerationMode mode, const ForEachKeyValueFunction& body,
    Label* bailout) {
  TNode<Uint16T> type = LoadMapInstanceType(map);
  TNode<Uint32T> bit_field3 = EnsureOnlyHasSimpleProperties(map, type, bailout);

  TVARIABLE(DescriptorArray, var_descriptors, LoadMapDescriptors(map));
  TNode<Uint32T> nof_descriptors =
      DecodeWord32<Map::Bits3::NumberOfOwnDescriptorsBits>(bit_field3);

  TVARIABLE(BoolT, var_stable, Int32TrueConstant());

  TVARIABLE(BoolT, var_has_symbol, Int32FalseConstant());
  // false - iterate only string properties, true - iterate only symbol
  // properties
  TVARIABLE(BoolT, var_is_symbol_processing_loop, Int32FalseConstant());
  TVARIABLE(IntPtrT, var_start_key_index,
            ToKeyIndex<DescriptorArray>(Unsigned(Int32Constant(0))));
  // Note: var_end_key_index is exclusive for the loop
  TVARIABLE(IntPtrT, var_end_key_index,
            ToKeyIndex<DescriptorArray>(nof_descriptors));
  VariableList list({&var_descriptors, &var_stable, &var_has_symbol,
                     &var_is_symbol_processing_loop, &var_start_key_index,
                     &var_end_key_index},
                    zone());
  Label descriptor_array_loop(this, list);

  Goto(&descriptor_array_loop);
  BIND(&descriptor_array_loop);

  BuildFastLoop<IntPtrT>(
      list, var_start_key_index.value(), var_end_key_index.value(),
      [&](TNode<IntPtrT> descriptor_key_index) {
        TNode<Name> next_key =
            LoadKeyByKeyIndex(var_descriptors.value(), descriptor_key_index);

        TVARIABLE(Object, var_value, SmiConstant(0));
        Label callback(this), next_iteration(this);

        if (mode == kEnumerationOrder) {
          // |next_key| is either a string or a symbol
          // Skip strings or symbols depending on
          // |var_is_symbol_processing_loop|.
          Label if_string(this), if_symbol(this), if_name_ok(this);
          Branch(IsSymbol(next_key), &if_symbol, &if_string);
          BIND(&if_symbol);
          {
            // Process symbol property when |var_is_symbol_processing_loop| is
            // true.
            GotoIf(var_is_symbol_processing_loop.value(), &if_name_ok);
            // First iteration need to calculate smaller range for processing
            // symbols
            Label if_first_symbol(this);
            // var_end_key_index is still inclusive at this point.
            var_end_key_index = descriptor_key_index;
            Branch(var_has_symbol.value(), &next_iteration, &if_first_symbol);
            BIND(&if_first_symbol);
            {
              var_start_key_index = descriptor_key_index;
              var_has_symbol = Int32TrueConstant();
              Goto(&next_iteration);
            }
          }
          BIND(&if_string);
          {
            CSA_ASSERT(this, IsString(next_key));
            // Process string property when |var_is_symbol_processing_loop| is
            // false.
            Branch(var_is_symbol_processing_loop.value(), &next_iteration,
                   &if_name_ok);
          }
          BIND(&if_name_ok);
        }
        {
          TVARIABLE(Map, var_map);
          TVARIABLE(HeapObject, var_meta_storage);
          TVARIABLE(IntPtrT, var_entry);
          TVARIABLE(Uint32T, var_details);
          Label if_found(this);

          Label if_found_fast(this), if_found_dict(this);

          Label if_stable(this), if_not_stable(this);
          Branch(var_stable.value(), &if_stable, &if_not_stable);
          BIND(&if_stable);
          {
            // Directly decode from the descriptor array if |object| did not
            // change shape.
            var_map = map;
            var_meta_storage = var_descriptors.value();
            var_entry = Signed(descriptor_key_index);
            Goto(&if_found_fast);
          }
          BIND(&if_not_stable);
          {
            // If the map did change, do a slower lookup. We are still
            // guaranteed that the object has a simple shape, and that the key
            // is a name.
            var_map = LoadMap(object);
            TryLookupPropertyInSimpleObject(
                object, var_map.value(), next_key, &if_found_fast,
                &if_found_dict, &var_meta_storage, &var_entry, &next_iteration);
          }

          BIND(&if_found_fast);
          {
            TNode<DescriptorArray> descriptors = CAST(var_meta_storage.value());
            TNode<IntPtrT> name_index = var_entry.value();

            // Skip non-enumerable properties.
            var_details = LoadDetailsByKeyIndex(descriptors, name_index);
            GotoIf(IsSetWord32(var_details.value(),
                               PropertyDetails::kAttributesDontEnumMask),
                   &next_iteration);

            LoadPropertyFromFastObject(object, var_map.value(), descriptors,
                                       name_index, var_details.value(),
                                       &var_value);
            Goto(&if_found);
          }
          BIND(&if_found_dict);
          {
            TNode<NameDictionary> dictionary = CAST(var_meta_storage.value());
            TNode<IntPtrT> entry = var_entry.value();

            TNode<Uint32T> details = LoadDetailsByKeyIndex(dictionary, entry);
            // Skip non-enumerable properties.
            GotoIf(
                IsSetWord32(details, PropertyDetails::kAttributesDontEnumMask),
                &next_iteration);

            var_details = details;
            var_value = LoadValueByKeyIndex<NameDictionary>(dictionary, entry);
            Goto(&if_found);
          }

          // Here we have details and value which could be an accessor.
          BIND(&if_found);
          {
            Label slow_load(this, Label::kDeferred);

            var_value = CallGetterIfAccessor(var_value.value(), object,
                                             var_details.value(), context,
                                             object, &slow_load, kCallJSGetter);
            Goto(&callback);

            BIND(&slow_load);
            var_value =
                CallRuntime(Runtime::kGetProperty, context, object, next_key);
            Goto(&callback);

            BIND(&callback);
            body(next_key, var_value.value());

            // Check if |object| is still stable, i.e. the descriptors in the
            // preloaded |descriptors| are still the same modulo in-place
            // representation changes.
            GotoIfNot(var_stable.value(), &next_iteration);
            var_stable = TaggedEqual(LoadMap(object), map);
            // Reload the descriptors just in case the actual array changed, and
            // any of the field representations changed in-place.
            var_descriptors = LoadMapDescriptors(map);

            Goto(&next_iteration);
          }
        }
        BIND(&next_iteration);
      },
      DescriptorArray::kEntrySize, IndexAdvanceMode::kPost);

  if (mode == kEnumerationOrder) {
    Label done(this);
    GotoIf(var_is_symbol_processing_loop.value(), &done);
    GotoIfNot(var_has_symbol.value(), &done);
    // All string properties are processed, now process symbol properties.
    var_is_symbol_processing_loop = Int32TrueConstant();
    // Add DescriptorArray::kEntrySize to make the var_end_key_index exclusive
    // as BuildFastLoop() expects.
    Increment(&var_end_key_index, DescriptorArray::kEntrySize);
    Goto(&descriptor_array_loop);

    BIND(&done);
  }
}

TNode<Object> CodeStubAssembler::GetConstructor(TNode<Map> map) {
  TVARIABLE(HeapObject, var_maybe_constructor);
  var_maybe_constructor = map;
  Label loop(this, &var_maybe_constructor), done(this);
  GotoIfNot(IsMap(var_maybe_constructor.value()), &done);
  Goto(&loop);

  BIND(&loop);
  {
    var_maybe_constructor = CAST(
        LoadObjectField(var_maybe_constructor.value(),
                        Map::kConstructorOrBackPointerOrNativeContextOffset));
    GotoIf(IsMap(var_maybe_constructor.value()), &loop);
    Goto(&done);
  }

  BIND(&done);
  return var_maybe_constructor.value();
}

TNode<NativeContext> CodeStubAssembler::GetCreationContext(
    TNode<JSReceiver> receiver, Label* if_bailout) {
  TNode<Map> receiver_map = LoadMap(receiver);
  TNode<Object> constructor = GetConstructor(receiver_map);

  TVARIABLE(JSFunction, var_function);

  Label done(this), if_jsfunction(this), if_jsgenerator(this);
  GotoIf(TaggedIsSmi(constructor), if_bailout);

  TNode<Map> function_map = LoadMap(CAST(constructor));
  GotoIf(IsJSFunctionMap(function_map), &if_jsfunction);
  GotoIf(IsJSGeneratorMap(function_map), &if_jsgenerator);
  // Remote objects don't have a creation context.
  GotoIf(IsFunctionTemplateInfoMap(function_map), if_bailout);

  CSA_ASSERT(this, IsJSFunctionMap(receiver_map));
  var_function = CAST(receiver);
  Goto(&done);

  BIND(&if_jsfunction);
  {
    var_function = CAST(constructor);
    Goto(&done);
  }

  BIND(&if_jsgenerator);
  {
    var_function = LoadJSGeneratorObjectFunction(CAST(receiver));
    Goto(&done);
  }

  BIND(&done);
  TNode<Context> context = LoadJSFunctionContext(var_function.value());

  GotoIfNot(IsContext(context), if_bailout);

  TNode<NativeContext> native_context = LoadNativeContext(context);
  return native_context;
}

void CodeStubAssembler::DescriptorLookup(TNode<Name> unique_name,
                                         TNode<DescriptorArray> descriptors,
                                         TNode<Uint32T> bitfield3,
                                         Label* if_found,
                                         TVariable<IntPtrT>* var_name_index,
                                         Label* if_not_found) {
  Comment("DescriptorArrayLookup");
  TNode<Uint32T> nof =
      DecodeWord32<Map::Bits3::NumberOfOwnDescriptorsBits>(bitfield3);
  Lookup<DescriptorArray>(unique_name, descriptors, nof, if_found,
                          var_name_index, if_not_found);
}

void CodeStubAssembler::TransitionLookup(TNode<Name> unique_name,
                                         TNode<TransitionArray> transitions,
                                         Label* if_found,
                                         TVariable<IntPtrT>* var_name_index,
                                         Label* if_not_found) {
  Comment("TransitionArrayLookup");
  TNode<Uint32T> number_of_valid_transitions =
      NumberOfEntries<TransitionArray>(transitions);
  Lookup<TransitionArray>(unique_name, transitions, number_of_valid_transitions,
                          if_found, var_name_index, if_not_found);
}

template <typename Array>
void CodeStubAssembler::Lookup(TNode<Name> unique_name, TNode<Array> array,
                               TNode<Uint32T> number_of_valid_entries,
                               Label* if_found,
                               TVariable<IntPtrT>* var_name_index,
                               Label* if_not_found) {
  Comment("ArrayLookup");
  if (!number_of_valid_entries) {
    number_of_valid_entries = NumberOfEntries(array);
  }
  GotoIf(Word32Equal(number_of_valid_entries, Int32Constant(0)), if_not_found);
  Label linear_search(this), binary_search(this);
  const int kMaxElementsForLinearSearch = 32;
  Branch(Uint32LessThanOrEqual(number_of_valid_entries,
                               Int32Constant(kMaxElementsForLinearSearch)),
         &linear_search, &binary_search);
  BIND(&linear_search);
  {
    LookupLinear<Array>(unique_name, array, number_of_valid_entries, if_found,
                        var_name_index, if_not_found);
  }
  BIND(&binary_search);
  {
    LookupBinary<Array>(unique_name, array, number_of_valid_entries, if_found,
                        var_name_index, if_not_found);
  }
}

void CodeStubAssembler::TryLookupPropertyInSimpleObject(
    TNode<JSObject> object, TNode<Map> map, TNode<Name> unique_name,
    Label* if_found_fast, Label* if_found_dict,
    TVariable<HeapObject>* var_meta_storage, TVariable<IntPtrT>* var_name_index,
    Label* if_not_found) {
  CSA_ASSERT(this, IsSimpleObjectMap(map));
  CSA_ASSERT(this, IsUniqueNameNoCachedIndex(unique_name));

  TNode<Uint32T> bit_field3 = LoadMapBitField3(map);
  Label if_isfastmap(this), if_isslowmap(this);
  Branch(IsSetWord32<Map::Bits3::IsDictionaryMapBit>(bit_field3), &if_isslowmap,
         &if_isfastmap);
  BIND(&if_isfastmap);
  {
    TNode<DescriptorArray> descriptors = LoadMapDescriptors(map);
    *var_meta_storage = descriptors;

    DescriptorLookup(unique_name, descriptors, bit_field3, if_found_fast,
                     var_name_index, if_not_found);
  }
  BIND(&if_isslowmap);
  {
    TNode<NameDictionary> dictionary = CAST(LoadSlowProperties(object));
    *var_meta_storage = dictionary;

    NameDictionaryLookup<NameDictionary>(dictionary, unique_name, if_found_dict,
                                         var_name_index, if_not_found);
  }
}

void CodeStubAssembler::TryLookupProperty(
    TNode<HeapObject> object, TNode<Map> map, SloppyTNode<Int32T> instance_type,
    TNode<Name> unique_name, Label* if_found_fast, Label* if_found_dict,
    Label* if_found_global, TVariable<HeapObject>* var_meta_storage,
    TVariable<IntPtrT>* var_name_index, Label* if_not_found,
    Label* if_bailout) {
  Label if_objectisspecial(this);
  GotoIf(IsSpecialReceiverInstanceType(instance_type), &if_objectisspecial);

  TryLookupPropertyInSimpleObject(CAST(object), map, unique_name, if_found_fast,
                                  if_found_dict, var_meta_storage,
                                  var_name_index, if_not_found);

  BIND(&if_objectisspecial);
  {
    // Handle global object here and bailout for other special objects.
    GotoIfNot(InstanceTypeEqual(instance_type, JS_GLOBAL_OBJECT_TYPE),
              if_bailout);

    // Handle interceptors and access checks in runtime.
    TNode<Int32T> bit_field = LoadMapBitField(map);
    int mask = Map::Bits1::HasNamedInterceptorBit::kMask |
               Map::Bits1::IsAccessCheckNeededBit::kMask;
    GotoIf(IsSetWord32(bit_field, mask), if_bailout);

    TNode<GlobalDictionary> dictionary = CAST(LoadSlowProperties(CAST(object)));
    *var_meta_storage = dictionary;

    NameDictionaryLookup<GlobalDictionary>(
        dictionary, unique_name, if_found_global, var_name_index, if_not_found);
  }
}

void CodeStubAssembler::TryHasOwnProperty(TNode<HeapObject> object,
                                          TNode<Map> map,
                                          TNode<Int32T> instance_type,
                                          TNode<Name> unique_name,
                                          Label* if_found, Label* if_not_found,
                                          Label* if_bailout) {
  Comment("TryHasOwnProperty");
  CSA_ASSERT(this, IsUniqueNameNoCachedIndex(unique_name));
  TVARIABLE(HeapObject, var_meta_storage);
  TVARIABLE(IntPtrT, var_name_index);

  Label if_found_global(this);
  TryLookupProperty(object, map, instance_type, unique_name, if_found, if_found,
                    &if_found_global, &var_meta_storage, &var_name_index,
                    if_not_found, if_bailout);

  BIND(&if_found_global);
  {
    TVARIABLE(Object, var_value);
    TVARIABLE(Uint32T, var_details);
    // Check if the property cell is not deleted.
    LoadPropertyFromGlobalDictionary(CAST(var_meta_storage.value()),
                                     var_name_index.value(), &var_details,
                                     &var_value, if_not_found);
    Goto(if_found);
  }
}

TNode<Object> CodeStubAssembler::GetMethod(TNode<Context> context,
                                           TNode<Object> object,
                                           Handle<Name> name,
                                           Label* if_null_or_undefined) {
  TNode<Object> method = GetProperty(context, object, name);

  GotoIf(IsUndefined(method), if_null_or_undefined);
  GotoIf(IsNull(method), if_null_or_undefined);

  return method;
}

TNode<Object> CodeStubAssembler::GetIteratorMethod(
    TNode<Context> context, TNode<HeapObject> heap_obj,
    Label* if_iteratorundefined) {
  return GetMethod(context, heap_obj, isolate()->factory()->iterator_symbol(),
                   if_iteratorundefined);
}

void CodeStubAssembler::LoadPropertyFromFastObject(
    TNode<HeapObject> object, TNode<Map> map,
    TNode<DescriptorArray> descriptors, TNode<IntPtrT> name_index,
    TVariable<Uint32T>* var_details, TVariable<Object>* var_value) {
  TNode<Uint32T> details = LoadDetailsByKeyIndex(descriptors, name_index);
  *var_details = details;

  LoadPropertyFromFastObject(object, map, descriptors, name_index, details,
                             var_value);
}

void CodeStubAssembler::LoadPropertyFromFastObject(
    TNode<HeapObject> object, TNode<Map> map,
    TNode<DescriptorArray> descriptors, TNode<IntPtrT> name_index,
    TNode<Uint32T> details, TVariable<Object>* var_value) {
  Comment("[ LoadPropertyFromFastObject");

  TNode<Uint32T> location =
      DecodeWord32<PropertyDetails::LocationField>(details);

  Label if_in_field(this), if_in_descriptor(this), done(this);
  Branch(Word32Equal(location, Int32Constant(kField)), &if_in_field,
         &if_in_descriptor);
  BIND(&if_in_field);
  {
    TNode<IntPtrT> field_index =
        Signed(DecodeWordFromWord32<PropertyDetails::FieldIndexField>(details));
    TNode<Uint32T> representation =
        DecodeWord32<PropertyDetails::RepresentationField>(details);

    field_index =
        IntPtrAdd(field_index, LoadMapInobjectPropertiesStartInWords(map));
    TNode<IntPtrT> instance_size_in_words = LoadMapInstanceSizeInWords(map);

    Label if_inobject(this), if_backing_store(this);
    TVARIABLE(Float64T, var_double_value);
    Label rebox_double(this, &var_double_value);
    Branch(UintPtrLessThan(field_index, instance_size_in_words), &if_inobject,
           &if_backing_store);
    BIND(&if_inobject);
    {
      Comment("if_inobject");
      TNode<IntPtrT> field_offset = TimesTaggedSize(field_index);

      Label if_double(this), if_tagged(this);
      Branch(Word32NotEqual(representation,
                            Int32Constant(Representation::kDouble)),
             &if_tagged, &if_double);
      BIND(&if_tagged);
      {
        *var_value = LoadObjectField(object, field_offset);
        Goto(&done);
      }
      BIND(&if_double);
      {
        if (FLAG_unbox_double_fields) {
          var_double_value = LoadObjectField<Float64T>(object, field_offset);
        } else {
          TNode<HeapNumber> heap_number =
              CAST(LoadObjectField(object, field_offset));
          var_double_value = LoadHeapNumberValue(heap_number);
        }
        Goto(&rebox_double);
      }
    }
    BIND(&if_backing_store);
    {
      Comment("if_backing_store");
      TNode<HeapObject> properties = LoadFastProperties(CAST(object));
      field_index = Signed(IntPtrSub(field_index, instance_size_in_words));
      TNode<Object> value =
          LoadPropertyArrayElement(CAST(properties), field_index);

      Label if_double(this), if_tagged(this);
      Branch(Word32NotEqual(representation,
                            Int32Constant(Representation::kDouble)),
             &if_tagged, &if_double);
      BIND(&if_tagged);
      {
        *var_value = value;
        Goto(&done);
      }
      BIND(&if_double);
      {
        var_double_value = LoadHeapNumberValue(CAST(value));
        Goto(&rebox_double);
      }
    }
    BIND(&rebox_double);
    {
      Comment("rebox_double");
      TNode<HeapNumber> heap_number =
          AllocateHeapNumberWithValue(var_double_value.value());
      *var_value = heap_number;
      Goto(&done);
    }
  }
  BIND(&if_in_descriptor);
  {
    *var_value = LoadValueByKeyIndex(descriptors, name_index);
    Goto(&done);
  }
  BIND(&done);

  Comment("] LoadPropertyFromFastObject");
}

void CodeStubAssembler::LoadPropertyFromNameDictionary(
    TNode<NameDictionary> dictionary, TNode<IntPtrT> name_index,
    TVariable<Uint32T>* var_details, TVariable<Object>* var_value) {
  Comment("LoadPropertyFromNameDictionary");
  *var_details = LoadDetailsByKeyIndex(dictionary, name_index);
  *var_value = LoadValueByKeyIndex(dictionary, name_index);

  Comment("] LoadPropertyFromNameDictionary");
}

void CodeStubAssembler::LoadPropertyFromGlobalDictionary(
    TNode<GlobalDictionary> dictionary, TNode<IntPtrT> name_index,
    TVariable<Uint32T>* var_details, TVariable<Object>* var_value,
    Label* if_deleted) {
  Comment("[ LoadPropertyFromGlobalDictionary");
  TNode<PropertyCell> property_cell =
      CAST(LoadFixedArrayElement(dictionary, name_index));

  TNode<Object> value =
      LoadObjectField(property_cell, PropertyCell::kValueOffset);
  GotoIf(TaggedEqual(value, TheHoleConstant()), if_deleted);

  *var_value = value;

  TNode<Uint32T> details = Unsigned(LoadAndUntagToWord32ObjectField(
      property_cell, PropertyCell::kPropertyDetailsRawOffset));
  *var_details = details;

  Comment("] LoadPropertyFromGlobalDictionary");
}

// |value| is the property backing store's contents, which is either a value or
// an accessor pair, as specified by |details|. |holder| is a JSObject or a
// PropertyCell (TODO: use UnionT). Returns either the original value, or the
// result of the getter call.
TNode<Object> CodeStubAssembler::CallGetterIfAccessor(
    TNode<Object> value, TNode<HeapObject> holder, TNode<Uint32T> details,
    TNode<Context> context, TNode<Object> receiver, Label* if_bailout,
    GetOwnPropertyMode mode) {
  TVARIABLE(Object, var_value, value);
  Label done(this), if_accessor_info(this, Label::kDeferred);

  TNode<Uint32T> kind = DecodeWord32<PropertyDetails::KindField>(details);
  GotoIf(Word32Equal(kind, Int32Constant(kData)), &done);

  // Accessor case.
  GotoIfNot(IsAccessorPair(CAST(value)), &if_accessor_info);

  // AccessorPair case.
  {
    if (mode == kCallJSGetter) {
      Label if_callable(this), if_function_template_info(this);
      TNode<AccessorPair> accessor_pair = CAST(value);
      TNode<HeapObject> getter =
          CAST(LoadObjectField(accessor_pair, AccessorPair::kGetterOffset));
      TNode<Map> getter_map = LoadMap(getter);

      GotoIf(IsCallableMap(getter_map), &if_callable);
      GotoIf(IsFunctionTemplateInfoMap(getter_map), &if_function_template_info);

      // Return undefined if the {getter} is not callable.
      var_value = UndefinedConstant();
      Goto(&done);

      BIND(&if_callable);
      {
        // Call the accessor.
        var_value = Call(context, getter, receiver);
        Goto(&done);
      }

      BIND(&if_function_template_info);
      {
        TNode<HeapObject> cached_property_name = LoadObjectField<HeapObject>(
            getter, FunctionTemplateInfo::kCachedPropertyNameOffset);
        GotoIfNot(IsTheHole(cached_property_name), if_bailout);

        TNode<NativeContext> creation_context =
            GetCreationContext(CAST(holder), if_bailout);
        var_value = CallBuiltin(
            Builtins::kCallFunctionTemplate_CheckAccessAndCompatibleReceiver,
            creation_context, getter, IntPtrConstant(0), receiver);
        Goto(&done);
      }
    } else {
      Goto(&done);
    }
  }

  // AccessorInfo case.
  BIND(&if_accessor_info);
  {
    TNode<AccessorInfo> accessor_info = CAST(value);
    Label if_array(this), if_function(this), if_wrapper(this);

    // Dispatch based on {holder} instance type.
    TNode<Map> holder_map = LoadMap(holder);
    TNode<Uint16T> holder_instance_type = LoadMapInstanceType(holder_map);
    GotoIf(IsJSArrayInstanceType(holder_instance_type), &if_array);
    GotoIf(IsJSFunctionInstanceType(holder_instance_type), &if_function);
    Branch(IsJSPrimitiveWrapperInstanceType(holder_instance_type), &if_wrapper,
           if_bailout);

    // JSArray AccessorInfo case.
    BIND(&if_array);
    {
      // We only deal with the "length" accessor on JSArray.
      GotoIfNot(IsLengthString(
                    LoadObjectField(accessor_info, AccessorInfo::kNameOffset)),
                if_bailout);
      TNode<JSArray> array = CAST(holder);
      var_value = LoadJSArrayLength(array);
      Goto(&done);
    }

    // JSFunction AccessorInfo case.
    BIND(&if_function);
    {
      // We only deal with the "prototype" accessor on JSFunction here.
      GotoIfNot(IsPrototypeString(
                    LoadObjectField(accessor_info, AccessorInfo::kNameOffset)),
                if_bailout);

      TNode<JSFunction> function = CAST(holder);
      GotoIfPrototypeRequiresRuntimeLookup(function, holder_map, if_bailout);
      var_value = LoadJSFunctionPrototype(function, if_bailout);
      Goto(&done);
    }

    // JSPrimitiveWrapper AccessorInfo case.
    BIND(&if_wrapper);
    {
      // We only deal with the "length" accessor on JSPrimitiveWrapper string
      // wrappers.
      GotoIfNot(IsLengthString(
                    LoadObjectField(accessor_info, AccessorInfo::kNameOffset)),
                if_bailout);
      TNode<Object> holder_value = LoadJSPrimitiveWrapperValue(CAST(holder));
      GotoIfNot(TaggedIsNotSmi(holder_value), if_bailout);
      GotoIfNot(IsString(CAST(holder_value)), if_bailout);
      var_value = LoadStringLengthAsSmi(CAST(holder_value));
      Goto(&done);
    }
  }

  BIND(&done);
  return var_value.value();
}

void CodeStubAssembler::TryGetOwnProperty(
    TNode<Context> context, TNode<Object> receiver, TNode<JSReceiver> object,
    TNode<Map> map, TNode<Int32T> instance_type, TNode<Name> unique_name,
    Label* if_found_value, TVariable<Object>* var_value, Label* if_not_found,
    Label* if_bailout) {
  TryGetOwnProperty(context, receiver, object, map, instance_type, unique_name,
                    if_found_value, var_value, nullptr, nullptr, if_not_found,
                    if_bailout, kCallJSGetter);
}

void CodeStubAssembler::TryGetOwnProperty(
    TNode<Context> context, TNode<Object> receiver, TNode<JSReceiver> object,
    TNode<Map> map, TNode<Int32T> instance_type, TNode<Name> unique_name,
    Label* if_found_value, TVariable<Object>* var_value,
    TVariable<Uint32T>* var_details, TVariable<Object>* var_raw_value,
    Label* if_not_found, Label* if_bailout, GetOwnPropertyMode mode) {
  DCHECK_EQ(MachineRepresentation::kTagged, var_value->rep());
  Comment("TryGetOwnProperty");
  CSA_ASSERT(this, IsUniqueNameNoCachedIndex(unique_name));
  TVARIABLE(HeapObject, var_meta_storage);
  TVARIABLE(IntPtrT, var_entry);

  Label if_found_fast(this), if_found_dict(this), if_found_global(this);

  TVARIABLE(Uint32T, local_var_details);
  if (!var_details) {
    var_details = &local_var_details;
  }
  Label if_found(this);

  TryLookupProperty(object, map, instance_type, unique_name, &if_found_fast,
                    &if_found_dict, &if_found_global, &var_meta_storage,
                    &var_entry, if_not_found, if_bailout);
  BIND(&if_found_fast);
  {
    TNode<DescriptorArray> descriptors = CAST(var_meta_storage.value());
    TNode<IntPtrT> name_index = var_entry.value();

    LoadPropertyFromFastObject(object, map, descriptors, name_index,
                               var_details, var_value);
    Goto(&if_found);
  }
  BIND(&if_found_dict);
  {
    TNode<NameDictionary> dictionary = CAST(var_meta_storage.value());
    TNode<IntPtrT> entry = var_entry.value();
    LoadPropertyFromNameDictionary(dictionary, entry, var_details, var_value);
    Goto(&if_found);
  }
  BIND(&if_found_global);
  {
    TNode<GlobalDictionary> dictionary = CAST(var_meta_storage.value());
    TNode<IntPtrT> entry = var_entry.value();

    LoadPropertyFromGlobalDictionary(dictionary, entry, var_details, var_value,
                                     if_not_found);
    Goto(&if_found);
  }
  // Here we have details and value which could be an accessor.
  BIND(&if_found);
  {
    // TODO(ishell): Execute C++ accessor in case of accessor info
    if (var_raw_value) {
      *var_raw_value = *var_value;
    }
    TNode<Object> value =
        CallGetterIfAccessor(var_value->value(), object, var_details->value(),
                             context, receiver, if_bailout, mode);
    *var_value = value;
    Goto(if_found_value);
  }
}

void CodeStubAssembler::TryLookupElement(
    TNode<HeapObject> object, TNode<Map> map, SloppyTNode<Int32T> instance_type,
    SloppyTNode<IntPtrT> intptr_index, Label* if_found, Label* if_absent,
    Label* if_not_found, Label* if_bailout) {
  // Handle special objects in runtime.
  GotoIf(IsSpecialReceiverInstanceType(instance_type), if_bailout);

  TNode<Int32T> elements_kind = LoadMapElementsKind(map);

  // TODO(verwaest): Support other elements kinds as well.
  Label if_isobjectorsmi(this), if_isdouble(this), if_isdictionary(this),
      if_isfaststringwrapper(this), if_isslowstringwrapper(this), if_oob(this),
      if_typedarray(this);
  // clang-format off
  int32_t values[] = {
      // Handled by {if_isobjectorsmi}.
      PACKED_SMI_ELEMENTS, HOLEY_SMI_ELEMENTS, PACKED_ELEMENTS, HOLEY_ELEMENTS,
      PACKED_NONEXTENSIBLE_ELEMENTS, PACKED_SEALED_ELEMENTS,
      HOLEY_NONEXTENSIBLE_ELEMENTS, HOLEY_SEALED_ELEMENTS,
      PACKED_FROZEN_ELEMENTS, HOLEY_FROZEN_ELEMENTS,
      // Handled by {if_isdouble}.
      PACKED_DOUBLE_ELEMENTS, HOLEY_DOUBLE_ELEMENTS,
      // Handled by {if_isdictionary}.
      DICTIONARY_ELEMENTS,
      // Handled by {if_isfaststringwrapper}.
      FAST_STRING_WRAPPER_ELEMENTS,
      // Handled by {if_isslowstringwrapper}.
      SLOW_STRING_WRAPPER_ELEMENTS,
      // Handled by {if_not_found}.
      NO_ELEMENTS,
      // Handled by {if_typed_array}.
      UINT8_ELEMENTS,
      INT8_ELEMENTS,
      UINT16_ELEMENTS,
      INT16_ELEMENTS,
      UINT32_ELEMENTS,
      INT32_ELEMENTS,
      FLOAT32_ELEMENTS,
      FLOAT64_ELEMENTS,
      UINT8_CLAMPED_ELEMENTS,
      BIGUINT64_ELEMENTS,
      BIGINT64_ELEMENTS,
  };
  Label* labels[] = {
      &if_isobjectorsmi, &if_isobjectorsmi, &if_isobjectorsmi,
      &if_isobjectorsmi, &if_isobjectorsmi, &if_isobjectorsmi,
      &if_isobjectorsmi, &if_isobjectorsmi, &if_isobjectorsmi,
      &if_isobjectorsmi,
      &if_isdouble, &if_isdouble,
      &if_isdictionary,
      &if_isfaststringwrapper,
      &if_isslowstringwrapper,
      if_not_found,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
      &if_typedarray,
  };
  // clang-format on
  STATIC_ASSERT(arraysize(values) == arraysize(labels));
  Switch(elements_kind, if_bailout, values, labels, arraysize(values));

  BIND(&if_isobjectorsmi);
  {
    TNode<FixedArray> elements = CAST(LoadElements(CAST(object)));
    TNode<IntPtrT> length = LoadAndUntagFixedArrayBaseLength(elements);

    GotoIfNot(UintPtrLessThan(intptr_index, length), &if_oob);

    TNode<Object> element = UnsafeLoadFixedArrayElement(elements, intptr_index);
    TNode<Oddball> the_hole = TheHoleConstant();
    Branch(TaggedEqual(element, the_hole), if_not_found, if_found);
  }
  BIND(&if_isdouble);
  {
    TNode<FixedArrayBase> elements = LoadElements(CAST(object));
    TNode<IntPtrT> length = LoadAndUntagFixedArrayBaseLength(elements);

    GotoIfNot(UintPtrLessThan(intptr_index, length), &if_oob);

    // Check if the element is a double hole, but don't load it.
    LoadFixedDoubleArrayElement(CAST(elements), intptr_index, if_not_found,
                                MachineType::None());
    Goto(if_found);
  }
  BIND(&if_isdictionary);
  {
    // Negative and too-large keys must be converted to property names.
    if (Is64()) {
      GotoIf(UintPtrLessThan(IntPtrConstant(JSArray::kMaxArrayIndex),
                             intptr_index),
             if_bailout);
    } else {
      GotoIf(IntPtrLessThan(intptr_index, IntPtrConstant(0)), if_bailout);
    }

    TVARIABLE(IntPtrT, var_entry);
    TNode<NumberDictionary> elements = CAST(LoadElements(CAST(object)));
    NumberDictionaryLookup(elements, intptr_index, if_found, &var_entry,
                           if_not_found);
  }
  BIND(&if_isfaststringwrapper);
  {
    TNode<String> string = CAST(LoadJSPrimitiveWrapperValue(CAST(object)));
    TNode<IntPtrT> length = LoadStringLengthAsWord(string);
    GotoIf(UintPtrLessThan(intptr_index, length), if_found);
    Goto(&if_isobjectorsmi);
  }
  BIND(&if_isslowstringwrapper);
  {
    TNode<String> string = CAST(LoadJSPrimitiveWrapperValue(CAST(object)));
    TNode<IntPtrT> length = LoadStringLengthAsWord(string);
    GotoIf(UintPtrLessThan(intptr_index, length), if_found);
    Goto(&if_isdictionary);
  }
  BIND(&if_typedarray);
  {
    TNode<JSArrayBuffer> buffer = LoadJSArrayBufferViewBuffer(CAST(object));
    GotoIf(IsDetachedBuffer(buffer), if_absent);

    TNode<UintPtrT> length = LoadJSTypedArrayLength(CAST(object));
    Branch(UintPtrLessThan(intptr_index, length), if_found, if_absent);
  }
  BIND(&if_oob);
  {
    // Positive OOB indices mean "not found", negative indices and indices
    // out of array index range must be converted to property names.
    if (Is64()) {
      GotoIf(UintPtrLessThan(IntPtrConstant(JSArray::kMaxArrayIndex),
                             intptr_index),
             if_bailout);
    } else {
      GotoIf(IntPtrLessThan(intptr_index, IntPtrConstant(0)), if_bailout);
    }
    Goto(if_not_found);
  }
}

void CodeStubAssembler::BranchIfMaybeSpecialIndex(TNode<String> name_string,
                                                  Label* if_maybe_special_index,
                                                  Label* if_not_special_index) {
  // TODO(cwhan.tunz): Implement fast cases more.

  // If a name is empty or too long, it's not a special index
  // Max length of canonical double: -X.XXXXXXXXXXXXXXXXX-eXXX
  const int kBufferSize = 24;
  TNode<Smi> string_length = LoadStringLengthAsSmi(name_string);
  GotoIf(SmiEqual(string_length, SmiConstant(0)), if_not_special_index);
  GotoIf(SmiGreaterThan(string_length, SmiConstant(kBufferSize)),
         if_not_special_index);

  // If the first character of name is not a digit or '-', or we can't match it
  // to Infinity or NaN, then this is not a special index.
  TNode<Int32T> first_char = StringCharCodeAt(name_string, UintPtrConstant(0));
  // If the name starts with '-', it can be a negative index.
  GotoIf(Word32Equal(first_char, Int32Constant('-')), if_maybe_special_index);
  // If the name starts with 'I', it can be "Infinity".
  GotoIf(Word32Equal(first_char, Int32Constant('I')), if_maybe_special_index);
  // If the name starts with 'N', it can be "NaN".
  GotoIf(Word32Equal(first_char, Int32Constant('N')), if_maybe_special_index);
  // Finally, if the first character is not a digit either, then we are sure
  // that the name is not a special index.
  GotoIf(Uint32LessThan(first_char, Int32Constant('0')), if_not_special_index);
  GotoIf(Uint32LessThan(Int32Constant('9'), first_char), if_not_special_index);
  Goto(if_maybe_special_index);
}

void CodeStubAssembler::TryPrototypeChainLookup(
    TNode<Object> receiver, TNode<Object> object_arg, TNode<Object> key,
    const LookupPropertyInHolder& lookup_property_in_holder,
    const LookupElementInHolder& lookup_element_in_holder, Label* if_end,
    Label* if_bailout, Label* if_proxy) {
  // Ensure receiver is JSReceiver, otherwise bailout.
  GotoIf(TaggedIsSmi(receiver), if_bailout);
  TNode<HeapObject> object = CAST(object_arg);

  TNode<Map> map = LoadMap(object);
  TNode<Uint16T> instance_type = LoadMapInstanceType(map);
  {
    Label if_objectisreceiver(this);
    STATIC_ASSERT(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
    STATIC_ASSERT(FIRST_JS_RECEIVER_TYPE == JS_PROXY_TYPE);
    Branch(IsJSReceiverInstanceType(instance_type), &if_objectisreceiver,
           if_bailout);
    BIND(&if_objectisreceiver);

    GotoIf(InstanceTypeEqual(instance_type, JS_PROXY_TYPE), if_proxy);
  }

  TVARIABLE(IntPtrT, var_index);
  TVARIABLE(Name, var_unique);

  Label if_keyisindex(this), if_iskeyunique(this);
  TryToName(key, &if_keyisindex, &var_index, &if_iskeyunique, &var_unique,
            if_bailout);

  BIND(&if_iskeyunique);
  {
    TVARIABLE(HeapObject, var_holder, object);
    TVARIABLE(Map, var_holder_map, map);
    TVARIABLE(Int32T, var_holder_instance_type, instance_type);

    Label loop(this, {&var_holder, &var_holder_map, &var_holder_instance_type});
    Goto(&loop);
    BIND(&loop);
    {
      TNode<Map> holder_map = var_holder_map.value();
      TNode<Int32T> holder_instance_type = var_holder_instance_type.value();

      Label next_proto(this), check_integer_indexed_exotic(this);
      lookup_property_in_holder(CAST(receiver), var_holder.value(), holder_map,
                                holder_instance_type, var_unique.value(),
                                &check_integer_indexed_exotic, if_bailout);

      BIND(&check_integer_indexed_exotic);
      {
        // Bailout if it can be an integer indexed exotic case.
        GotoIfNot(InstanceTypeEqual(holder_instance_type, JS_TYPED_ARRAY_TYPE),
                  &next_proto);
        GotoIfNot(IsString(var_unique.value()), &next_proto);
        BranchIfMaybeSpecialIndex(CAST(var_unique.value()), if_bailout,
                                  &next_proto);
      }

      BIND(&next_proto);

      TNode<HeapObject> proto = LoadMapPrototype(holder_map);

      GotoIf(IsNull(proto), if_end);

      TNode<Map> map = LoadMap(proto);
      TNode<Uint16T> instance_type = LoadMapInstanceType(map);

      var_holder = proto;
      var_holder_map = map;
      var_holder_instance_type = instance_type;
      Goto(&loop);
    }
  }
  BIND(&if_keyisindex);
  {
    TVARIABLE(HeapObject, var_holder, object);
    TVARIABLE(Map, var_holder_map, map);
    TVARIABLE(Int32T, var_holder_instance_type, instance_type);

    Label loop(this, {&var_holder, &var_holder_map, &var_holder_instance_type});
    Goto(&loop);
    BIND(&loop);
    {
      Label next_proto(this);
      lookup_element_in_holder(CAST(receiver), var_holder.value(),
                               var_holder_map.value(),
                               var_holder_instance_type.value(),
                               var_index.value(), &next_proto, if_bailout);
      BIND(&next_proto);

      TNode<HeapObject> proto = LoadMapPrototype(var_holder_map.value());

      GotoIf(IsNull(proto), if_end);

      TNode<Map> map = LoadMap(proto);
      TNode<Uint16T> instance_type = LoadMapInstanceType(map);

      var_holder = proto;
      var_holder_map = map;
      var_holder_instance_type = instance_type;
      Goto(&loop);
    }
  }
}

TNode<Oddball> CodeStubAssembler::HasInPrototypeChain(TNode<Context> context,
                                                      TNode<HeapObject> object,
                                                      TNode<Object> prototype) {
  TVARIABLE(Oddball, var_result);
  Label return_false(this), return_true(this),
      return_runtime(this, Label::kDeferred), return_result(this);

  // Loop through the prototype chain looking for the {prototype}.
  TVARIABLE(Map, var_object_map, LoadMap(object));
  Label loop(this, &var_object_map);
  Goto(&loop);
  BIND(&loop);
  {
    // Check if we can determine the prototype directly from the {object_map}.
    Label if_objectisdirect(this), if_objectisspecial(this, Label::kDeferred);
    TNode<Map> object_map = var_object_map.value();
    TNode<Uint16T> object_instance_type = LoadMapInstanceType(object_map);
    Branch(IsSpecialReceiverInstanceType(object_instance_type),
           &if_objectisspecial, &if_objectisdirect);
    BIND(&if_objectisspecial);
    {
      // The {object_map} is a special receiver map or a primitive map, check
      // if we need to use the if_objectisspecial path in the runtime.
      GotoIf(InstanceTypeEqual(object_instance_type, JS_PROXY_TYPE),
             &return_runtime);
      TNode<Int32T> object_bitfield = LoadMapBitField(object_map);
      int mask = Map::Bits1::HasNamedInterceptorBit::kMask |
                 Map::Bits1::IsAccessCheckNeededBit::kMask;
      Branch(IsSetWord32(object_bitfield, mask), &return_runtime,
             &if_objectisdirect);
    }
    BIND(&if_objectisdirect);

    // Check the current {object} prototype.
    TNode<HeapObject> object_prototype = LoadMapPrototype(object_map);
    GotoIf(IsNull(object_prototype), &return_false);
    GotoIf(TaggedEqual(object_prototype, prototype), &return_true);

    // Continue with the prototype.
    CSA_ASSERT(this, TaggedIsNotSmi(object_prototype));
    var_object_map = LoadMap(object_prototype);
    Goto(&loop);
  }

  BIND(&return_true);
  var_result = TrueConstant();
  Goto(&return_result);

  BIND(&return_false);
  var_result = FalseConstant();
  Goto(&return_result);

  BIND(&return_runtime);
  {
    // Fallback to the runtime implementation.
    var_result = CAST(
        CallRuntime(Runtime::kHasInPrototypeChain, context, object, prototype));
  }
  Goto(&return_result);

  BIND(&return_result);
  return var_result.value();
}

TNode<Oddball> CodeStubAssembler::OrdinaryHasInstance(
    TNode<Context> context, TNode<Object> callable_maybe_smi,
    TNode<Object> object_maybe_smi) {
  TVARIABLE(Oddball, var_result);
  Label return_runtime(this, Label::kDeferred), return_result(this);

  GotoIfForceSlowPath(&return_runtime);

  // Goto runtime if {object} is a Smi.
  GotoIf(TaggedIsSmi(object_maybe_smi), &return_runtime);

  // Goto runtime if {callable} is a Smi.
  GotoIf(TaggedIsSmi(callable_maybe_smi), &return_runtime);

  {
    // Load map of {callable}.
    TNode<HeapObject> object = CAST(object_maybe_smi);
    TNode<HeapObject> callable = CAST(callable_maybe_smi);
    TNode<Map> callable_map = LoadMap(callable);

    // Goto runtime if {callable} is not a JSFunction.
    TNode<Uint16T> callable_instance_type = LoadMapInstanceType(callable_map);
    GotoIfNot(InstanceTypeEqual(callable_instance_type, JS_FUNCTION_TYPE),
              &return_runtime);

    GotoIfPrototypeRequiresRuntimeLookup(CAST(callable), callable_map,
                                         &return_runtime);

    // Get the "prototype" (or initial map) of the {callable}.
    TNode<HeapObject> callable_prototype = LoadObjectField<HeapObject>(
        callable, JSFunction::kPrototypeOrInitialMapOffset);
    {
      Label no_initial_map(this), walk_prototype_chain(this);
      TVARIABLE(HeapObject, var_callable_prototype, callable_prototype);

      // Resolve the "prototype" if the {callable} has an initial map.
      GotoIfNot(IsMap(callable_prototype), &no_initial_map);
      var_callable_prototype = LoadObjectField<HeapObject>(
          callable_prototype, Map::kPrototypeOffset);
      Goto(&walk_prototype_chain);

      BIND(&no_initial_map);
      // {callable_prototype} is the hole if the "prototype" property hasn't
      // been requested so far.
      Branch(TaggedEqual(callable_prototype, TheHoleConstant()),
             &return_runtime, &walk_prototype_chain);

      BIND(&walk_prototype_chain);
      callable_prototype = var_callable_prototype.value();
    }

    // Loop through the prototype chain looking for the {callable} prototype.
    var_result = HasInPrototypeChain(context, object, callable_prototype);
    Goto(&return_result);
  }

  BIND(&return_runtime);
  {
    // Fallback to the runtime implementation.
    var_result = CAST(CallRuntime(Runtime::kOrdinaryHasInstance, context,
                                  callable_maybe_smi, object_maybe_smi));
  }
  Goto(&return_result);

  BIND(&return_result);
  return var_result.value();
}

template <typename TIndex>
TNode<IntPtrT> CodeStubAssembler::ElementOffsetFromIndex(
    TNode<TIndex> index_node, ElementsKind kind, int base_size) {
  // TODO(v8:9708): Remove IntPtrT variant in favor of UintPtrT.
  static_assert(std::is_same<TIndex, Smi>::value ||
                    std::is_same<TIndex, TaggedIndex>::value ||
                    std::is_same<TIndex, IntPtrT>::value ||
                    std::is_same<TIndex, UintPtrT>::value,
                "Only Smi, UintPtrT or IntPtrT index nodes are allowed");
  int element_size_shift = ElementsKindToShiftSize(kind);
  int element_size = 1 << element_size_shift;
  intptr_t index = 0;
  TNode<IntPtrT> intptr_index_node;
  bool constant_index = false;
  if (std::is_same<TIndex, Smi>::value) {
    TNode<Smi> smi_index_node = ReinterpretCast<Smi>(index_node);
    int const kSmiShiftBits = kSmiShiftSize + kSmiTagSize;
    element_size_shift -= kSmiShiftBits;
    Smi smi_index;
    constant_index = ToSmiConstant(smi_index_node, &smi_index);
    if (constant_index) {
      index = smi_index.value();
    } else {
      if (COMPRESS_POINTERS_BOOL) {
        smi_index_node = NormalizeSmiIndex(smi_index_node);
      }
    }
    intptr_index_node = BitcastTaggedToWordForTagAndSmiBits(smi_index_node);
  } else if (std::is_same<TIndex, TaggedIndex>::value) {
    TNode<TaggedIndex> tagged_index_node =
        ReinterpretCast<TaggedIndex>(index_node);
    element_size_shift -= kSmiTagSize;
    intptr_index_node = BitcastTaggedToWordForTagAndSmiBits(tagged_index_node);
    constant_index = ToIntPtrConstant(intptr_index_node, &index);
  } else {
    intptr_index_node = ReinterpretCast<IntPtrT>(index_node);
    constant_index = ToIntPtrConstant(intptr_index_node, &index);
  }
  if (constant_index) {
    return IntPtrConstant(base_size + element_size * index);
  }

  TNode<IntPtrT> shifted_index =
      (element_size_shift == 0)
          ? intptr_index_node
          : ((element_size_shift > 0)
                 ? WordShl(intptr_index_node,
                           IntPtrConstant(element_size_shift))
                 : WordSar(intptr_index_node,
                           IntPtrConstant(-element_size_shift)));
  return IntPtrAdd(IntPtrConstant(base_size), Signed(shifted_index));
}

// Instantiate ElementOffsetFromIndex for Smi and IntPtrT.
template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::ElementOffsetFromIndex<Smi>(TNode<Smi> index_node,
                                               ElementsKind kind,
                                               int base_size);
template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::ElementOffsetFromIndex<TaggedIndex>(
    TNode<TaggedIndex> index_node, ElementsKind kind, int base_size);
template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::ElementOffsetFromIndex<IntPtrT>(TNode<IntPtrT> index_node,
                                                   ElementsKind kind,
                                                   int base_size);

TNode<BoolT> CodeStubAssembler::IsOffsetInBounds(SloppyTNode<IntPtrT> offset,
                                                 SloppyTNode<IntPtrT> length,
                                                 int header_size,
                                                 ElementsKind kind) {
  // Make sure we point to the last field.
  int element_size = 1 << ElementsKindToShiftSize(kind);
  int correction = header_size - kHeapObjectTag - element_size;
  TNode<IntPtrT> last_offset = ElementOffsetFromIndex(length, kind, correction);
  return IntPtrLessThanOrEqual(offset, last_offset);
}

TNode<HeapObject> CodeStubAssembler::LoadFeedbackCellValue(
    TNode<JSFunction> closure) {
  TNode<FeedbackCell> feedback_cell =
      LoadObjectField<FeedbackCell>(closure, JSFunction::kFeedbackCellOffset);
  return LoadObjectField<HeapObject>(feedback_cell, FeedbackCell::kValueOffset);
}

TNode<HeapObject> CodeStubAssembler::LoadFeedbackVector(
    TNode<JSFunction> closure) {
  TVARIABLE(HeapObject, maybe_vector, LoadFeedbackCellValue(closure));
  Label done(this);

  // If the closure doesn't have a feedback vector allocated yet, return
  // undefined. FeedbackCell can contain Undefined / FixedArray (for lazy
  // allocations) / FeedbackVector.
  // 如果闭包没有分配一个反馈向量, 返回 undefined. FeedbackCell 可以包含 undefined/fixedarray/feedbackvector
  GotoIf(IsFeedbackVector(maybe_vector.value()), &done);

  // In all other cases return Undefined.
  maybe_vector = UndefinedConstant();
  Goto(&done);

  BIND(&done);
  return maybe_vector.value();
}

TNode<ClosureFeedbackCellArray> CodeStubAssembler::LoadClosureFeedbackArray(
    TNode<JSFunction> closure) {
  TVARIABLE(HeapObject, feedback_cell_array, LoadFeedbackCellValue(closure));
  Label end(this);

  // When feedback vectors are not yet allocated feedback cell contains a
  // an array of feedback cells used by create closures.
  GotoIf(HasInstanceType(feedback_cell_array.value(),
                         CLOSURE_FEEDBACK_CELL_ARRAY_TYPE),
         &end);

  // Load FeedbackCellArray from feedback vector.
  TNode<FeedbackVector> vector = CAST(feedback_cell_array.value());
  feedback_cell_array = CAST(
      LoadObjectField(vector, FeedbackVector::kClosureFeedbackCellArrayOffset));
  Goto(&end);

  BIND(&end);
  return CAST(feedback_cell_array.value());
}

TNode<FeedbackVector> CodeStubAssembler::LoadFeedbackVectorForStub() {
  TNode<JSFunction> function =
      CAST(LoadFromParentFrame(StandardFrameConstants::kFunctionOffset));
  return CAST(LoadFeedbackVector(function));
}

void CodeStubAssembler::UpdateFeedback(TNode<Smi> feedback,
                                       TNode<HeapObject> maybe_vector,
                                       TNode<UintPtrT> slot_id) {
  Label end(this);
  // If feedback_vector is not valid, then nothing to do.
  GotoIf(IsUndefined(maybe_vector), &end);

  // This method is used for binary op and compare feedback. These
  // vector nodes are initialized with a smi 0, so we can simply OR
  // our new feedback in place.
  TNode<FeedbackVector> feedback_vector = CAST(maybe_vector);
  TNode<MaybeObject> feedback_element =
      LoadFeedbackVectorSlot(feedback_vector, slot_id);
  TNode<Smi> previous_feedback = CAST(feedback_element);
  TNode<Smi> combined_feedback = SmiOr(previous_feedback, feedback);

  GotoIf(SmiEqual(previous_feedback, combined_feedback), &end);
  {
    StoreFeedbackVectorSlot(feedback_vector, slot_id, combined_feedback,
                            SKIP_WRITE_BARRIER);
    ReportFeedbackUpdate(feedback_vector, slot_id, "UpdateFeedback");
    Goto(&end);
  }

  BIND(&end);
}

void CodeStubAssembler::ReportFeedbackUpdate(
    TNode<FeedbackVector> feedback_vector, SloppyTNode<UintPtrT> slot_id,
    const char* reason) {
  // Reset profiler ticks.
  StoreObjectFieldNoWriteBarrier(
      feedback_vector, FeedbackVector::kProfilerTicksOffset, Int32Constant(0));

#ifdef V8_TRACE_FEEDBACK_UPDATES
  // Trace the update.
  CallRuntime(Runtime::kInterpreterTraceUpdateFeedback, NoContextConstant(),
              LoadFromParentFrame(StandardFrameConstants::kFunctionOffset),
              SmiTag(Signed(slot_id)), StringConstant(reason));
#endif  // V8_TRACE_FEEDBACK_UPDATES
}

void CodeStubAssembler::OverwriteFeedback(TVariable<Smi>* existing_feedback,
                                          int new_feedback) {
  if (existing_feedback == nullptr) return;
  *existing_feedback = SmiConstant(new_feedback);
}

void CodeStubAssembler::CombineFeedback(TVariable<Smi>* existing_feedback,
                                        int feedback) {
  if (existing_feedback == nullptr) return;
  *existing_feedback = SmiOr(existing_feedback->value(), SmiConstant(feedback));
}

void CodeStubAssembler::CombineFeedback(TVariable<Smi>* existing_feedback,
                                        TNode<Smi> feedback) {
  if (existing_feedback == nullptr) return;
  *existing_feedback = SmiOr(existing_feedback->value(), feedback);
}

void CodeStubAssembler::CheckForAssociatedProtector(TNode<Name> name,
                                                    Label* if_protector) {
  // This list must be kept in sync with LookupIterator::UpdateProtector!
  // TODO(jkummerow): Would it be faster to have a bit in Symbol::flags()?
  GotoIf(TaggedEqual(name, ConstructorStringConstant()), if_protector);
  GotoIf(TaggedEqual(name, IteratorSymbolConstant()), if_protector);
  GotoIf(TaggedEqual(name, NextStringConstant()), if_protector);
  GotoIf(TaggedEqual(name, SpeciesSymbolConstant()), if_protector);
  GotoIf(TaggedEqual(name, IsConcatSpreadableSymbolConstant()), if_protector);
  GotoIf(TaggedEqual(name, ResolveStringConstant()), if_protector);
  GotoIf(TaggedEqual(name, ThenStringConstant()), if_protector);
  // Fall through if no case matched.
}

TNode<Map> CodeStubAssembler::LoadReceiverMap(SloppyTNode<Object> receiver) {
  return Select<Map>(
      TaggedIsSmi(receiver), [=] { return HeapNumberMapConstant(); },
      [=] { return LoadMap(UncheckedCast<HeapObject>(receiver)); });
}

TNode<IntPtrT> CodeStubAssembler::TryToIntptr(
    SloppyTNode<Object> key, Label* if_not_intptr,
    TVariable<Int32T>* var_instance_type) {
  TVARIABLE(IntPtrT, var_intptr_key);
  Label done(this, &var_intptr_key), key_is_smi(this), key_is_heapnumber(this);
  GotoIf(TaggedIsSmi(key), &key_is_smi);

  TNode<Int32T> instance_type = LoadInstanceType(CAST(key));
  if (var_instance_type != nullptr) {
    *var_instance_type = instance_type;
  }

  Branch(IsHeapNumberInstanceType(instance_type), &key_is_heapnumber,
         if_not_intptr);

  BIND(&key_is_smi);
  {
    var_intptr_key = SmiUntag(CAST(key));
    Goto(&done);
  }

  BIND(&key_is_heapnumber);
  {
    TNode<Float64T> value = LoadHeapNumberValue(CAST(key));
    TNode<IntPtrT> int_value = ChangeFloat64ToIntPtr(value);
    GotoIfNot(Float64Equal(value, RoundIntPtrToFloat64(int_value)),
              if_not_intptr);
#if V8_TARGET_ARCH_64_BIT
    // We can't rely on Is64() alone because 32-bit compilers rightly complain
    // about kMaxSafeIntegerUint64 not fitting into an intptr_t.
    DCHECK(Is64());
    // TODO(jkummerow): Investigate whether we can drop support for
    // negative indices.
    GotoIfNot(IsInRange(int_value, static_cast<intptr_t>(-kMaxSafeInteger),
                        static_cast<intptr_t>(kMaxSafeIntegerUint64)),
              if_not_intptr);
#else
    DCHECK(!Is64());
#endif
    var_intptr_key = int_value;
    Goto(&done);
  }

  BIND(&done);
  return var_intptr_key.value();
}

TNode<Context> CodeStubAssembler::LoadScriptContext(
    TNode<Context> context, TNode<IntPtrT> context_index) {
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<ScriptContextTable> script_context_table = CAST(
      LoadContextElement(native_context, Context::SCRIPT_CONTEXT_TABLE_INDEX));

  TNode<Context> script_context = CAST(LoadFixedArrayElement(
      script_context_table, context_index,
      ScriptContextTable::kFirstContextSlotIndex * kTaggedSize));
  return script_context;
}

namespace {

// Converts typed array elements kind to a machine representations.
MachineRepresentation ElementsKindToMachineRepresentation(ElementsKind kind) {
  switch (kind) {
    case UINT8_CLAMPED_ELEMENTS:
    case UINT8_ELEMENTS:
    case INT8_ELEMENTS:
      return MachineRepresentation::kWord8;
    case UINT16_ELEMENTS:
    case INT16_ELEMENTS:
      return MachineRepresentation::kWord16;
    case UINT32_ELEMENTS:
    case INT32_ELEMENTS:
      return MachineRepresentation::kWord32;
    case FLOAT32_ELEMENTS:
      return MachineRepresentation::kFloat32;
    case FLOAT64_ELEMENTS:
      return MachineRepresentation::kFloat64;
    default:
      UNREACHABLE();
  }
}

}  // namespace

template <typename TArray, typename TIndex>
void CodeStubAssembler::StoreElementBigIntOrTypedArray(TNode<TArray> elements,
                                                       ElementsKind kind,
                                                       TNode<TIndex> index,
                                                       Node* value) {
  // TODO(v8:9708): Do we want to keep both IntPtrT and UintPtrT variants?
  static_assert(std::is_same<TIndex, Smi>::value ||
                    std::is_same<TIndex, UintPtrT>::value ||
                    std::is_same<TIndex, IntPtrT>::value,
                "Only Smi, UintPtrT or IntPtrT index is allowed");
  static_assert(std::is_same<TArray, RawPtrT>::value ||
                    std::is_same<TArray, FixedArrayBase>::value,
                "Only RawPtrT or FixedArrayBase elements are allowed");
  if (kind == BIGINT64_ELEMENTS || kind == BIGUINT64_ELEMENTS) {
    TNode<IntPtrT> offset = ElementOffsetFromIndex(index, kind, 0);
    TVARIABLE(UintPtrT, var_low);
    // Only used on 32-bit platforms.
    TVARIABLE(UintPtrT, var_high);
    BigIntToRawBytes(CAST(value), &var_low, &var_high);

    MachineRepresentation rep = WordT::kMachineRepresentation;
#if defined(V8_TARGET_BIG_ENDIAN)
    if (!Is64()) {
      StoreNoWriteBarrier(rep, elements, offset, var_high.value());
      StoreNoWriteBarrier(rep, elements,
                          IntPtrAdd(offset, IntPtrConstant(kSystemPointerSize)),
                          var_low.value());
    } else {
      StoreNoWriteBarrier(rep, elements, offset, var_low.value());
    }
#else
    StoreNoWriteBarrier(rep, elements, offset, var_low.value());
    if (!Is64()) {
      StoreNoWriteBarrier(rep, elements,
                          IntPtrAdd(offset, IntPtrConstant(kSystemPointerSize)),
                          var_high.value());
    }
#endif
  } else {
    DCHECK(IsTypedArrayElementsKind(kind));
    if (kind == UINT8_CLAMPED_ELEMENTS) {
      CSA_ASSERT(this, Word32Equal(UncheckedCast<Word32T>(value),
                                   Word32And(Int32Constant(0xFF), value)));
    }
    TNode<IntPtrT> offset = ElementOffsetFromIndex(index, kind, 0);
    // TODO(cbruni): Add OOB check once typed.
    MachineRepresentation rep = ElementsKindToMachineRepresentation(kind);
    StoreNoWriteBarrier(rep, elements, offset, value);
  }
}

template <typename TIndex>
void CodeStubAssembler::StoreElement(TNode<FixedArrayBase> elements,
                                     ElementsKind kind, TNode<TIndex> index,
                                     Node* value) {
  if (kind == BIGINT64_ELEMENTS || kind == BIGUINT64_ELEMENTS ||
      IsTypedArrayElementsKind(kind)) {
    StoreElementBigIntOrTypedArray(elements, kind, index, value);
  } else if (IsDoubleElementsKind(kind)) {
    TNode<Float64T> value_float64 = UncheckedCast<Float64T>(value);
    StoreFixedDoubleArrayElement(CAST(elements), index, value_float64);
  } else {
    WriteBarrierMode barrier_mode = IsSmiElementsKind(kind)
                                        ? UNSAFE_SKIP_WRITE_BARRIER
                                        : UPDATE_WRITE_BARRIER;
    StoreFixedArrayElement(CAST(elements), index, value, barrier_mode, 0);
  }
}

template <typename TIndex>
void CodeStubAssembler::StoreElement(TNode<RawPtrT> elements, ElementsKind kind,
                                     TNode<TIndex> index, Node* value) {
  DCHECK(kind == BIGINT64_ELEMENTS || kind == BIGUINT64_ELEMENTS ||
         IsTypedArrayElementsKind(kind));
  StoreElementBigIntOrTypedArray(elements, kind, index, value);
}
template V8_EXPORT_PRIVATE void CodeStubAssembler::StoreElement<UintPtrT>(
    TNode<RawPtrT>, ElementsKind, TNode<UintPtrT>, Node*);

TNode<Uint8T> CodeStubAssembler::Int32ToUint8Clamped(
    TNode<Int32T> int32_value) {
  Label done(this);
  TNode<Int32T> int32_zero = Int32Constant(0);
  TNode<Int32T> int32_255 = Int32Constant(255);
  TVARIABLE(Word32T, var_value, int32_value);
  GotoIf(Uint32LessThanOrEqual(int32_value, int32_255), &done);
  var_value = int32_zero;
  GotoIf(Int32LessThan(int32_value, int32_zero), &done);
  var_value = int32_255;
  Goto(&done);
  BIND(&done);
  return UncheckedCast<Uint8T>(var_value.value());
}

TNode<Uint8T> CodeStubAssembler::Float64ToUint8Clamped(
    TNode<Float64T> float64_value) {
  Label done(this);
  TVARIABLE(Word32T, var_value, Int32Constant(0));
  GotoIf(Float64LessThanOrEqual(float64_value, Float64Constant(0.0)), &done);
  var_value = Int32Constant(255);
  GotoIf(Float64LessThanOrEqual(Float64Constant(255.0), float64_value), &done);
  {
    TNode<Float64T> rounded_value = Float64RoundToEven(float64_value);
    var_value = TruncateFloat64ToWord32(rounded_value);
    Goto(&done);
  }
  BIND(&done);
  return UncheckedCast<Uint8T>(var_value.value());
}

template <>
TNode<Word32T> CodeStubAssembler::PrepareValueForWriteToTypedArray<Word32T>(
    TNode<Object> input, ElementsKind elements_kind, TNode<Context> context) {
  DCHECK(IsTypedArrayElementsKind(elements_kind));

  switch (elements_kind) {
    case UINT8_ELEMENTS:
    case INT8_ELEMENTS:
    case UINT16_ELEMENTS:
    case INT16_ELEMENTS:
    case UINT32_ELEMENTS:
    case INT32_ELEMENTS:
    case UINT8_CLAMPED_ELEMENTS:
      break;
    default:
      UNREACHABLE();
  }

  TVARIABLE(Word32T, var_result);
  TVARIABLE(Object, var_input, input);
  Label done(this, &var_result), if_smi(this), if_heapnumber_or_oddball(this),
      convert(this), loop(this, &var_input);
  Goto(&loop);
  BIND(&loop);
  GotoIf(TaggedIsSmi(var_input.value()), &if_smi);
  // We can handle both HeapNumber and Oddball here, since Oddball has the
  // same layout as the HeapNumber for the HeapNumber::value field. This
  // way we can also properly optimize stores of oddballs to typed arrays.
  TNode<HeapObject> heap_object = CAST(var_input.value());
  GotoIf(IsHeapNumber(heap_object), &if_heapnumber_or_oddball);
  STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                    Oddball::kToNumberRawOffset);
  Branch(HasInstanceType(heap_object, ODDBALL_TYPE), &if_heapnumber_or_oddball,
         &convert);

  BIND(&if_heapnumber_or_oddball);
  {
    TNode<Float64T> value =
        LoadObjectField<Float64T>(heap_object, HeapNumber::kValueOffset);
    if (elements_kind == UINT8_CLAMPED_ELEMENTS) {
      var_result = Float64ToUint8Clamped(value);
    } else {
      var_result = TruncateFloat64ToWord32(value);
    }
    Goto(&done);
  }

  BIND(&if_smi);
  {
    TNode<Int32T> value = SmiToInt32(CAST(var_input.value()));
    if (elements_kind == UINT8_CLAMPED_ELEMENTS) {
      var_result = Int32ToUint8Clamped(value);
    } else {
      var_result = value;
    }
    Goto(&done);
  }

  BIND(&convert);
  {
    var_input = CallBuiltin(Builtins::kNonNumberToNumber, context, input);
    Goto(&loop);
  }

  BIND(&done);
  return var_result.value();
}

template <>
TNode<Float32T> CodeStubAssembler::PrepareValueForWriteToTypedArray<Float32T>(
    TNode<Object> input, ElementsKind elements_kind, TNode<Context> context) {
  DCHECK(IsTypedArrayElementsKind(elements_kind));
  CHECK_EQ(elements_kind, FLOAT32_ELEMENTS);

  TVARIABLE(Float32T, var_result);
  TVARIABLE(Object, var_input, input);
  Label done(this, &var_result), if_smi(this), if_heapnumber_or_oddball(this),
      convert(this), loop(this, &var_input);
  Goto(&loop);
  BIND(&loop);
  GotoIf(TaggedIsSmi(var_input.value()), &if_smi);
  // We can handle both HeapNumber and Oddball here, since Oddball has the
  // same layout as the HeapNumber for the HeapNumber::value field. This
  // way we can also properly optimize stores of oddballs to typed arrays.
  TNode<HeapObject> heap_object = CAST(var_input.value());
  GotoIf(IsHeapNumber(heap_object), &if_heapnumber_or_oddball);
  STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                    Oddball::kToNumberRawOffset);
  Branch(HasInstanceType(heap_object, ODDBALL_TYPE), &if_heapnumber_or_oddball,
         &convert);

  BIND(&if_heapnumber_or_oddball);
  {
    TNode<Float64T> value =
        LoadObjectField<Float64T>(heap_object, HeapNumber::kValueOffset);
    var_result = TruncateFloat64ToFloat32(value);
    Goto(&done);
  }

  BIND(&if_smi);
  {
    TNode<Int32T> value = SmiToInt32(CAST(var_input.value()));
    var_result = RoundInt32ToFloat32(value);
    Goto(&done);
  }

  BIND(&convert);
  {
    var_input = CallBuiltin(Builtins::kNonNumberToNumber, context, input);
    Goto(&loop);
  }

  BIND(&done);
  return var_result.value();
}

template <>
TNode<Float64T> CodeStubAssembler::PrepareValueForWriteToTypedArray<Float64T>(
    TNode<Object> input, ElementsKind elements_kind, TNode<Context> context) {
  DCHECK(IsTypedArrayElementsKind(elements_kind));
  CHECK_EQ(elements_kind, FLOAT64_ELEMENTS);

  TVARIABLE(Float64T, var_result);
  TVARIABLE(Object, var_input, input);
  Label done(this, &var_result), if_smi(this), if_heapnumber_or_oddball(this),
      convert(this), loop(this, &var_input);
  Goto(&loop);
  BIND(&loop);
  GotoIf(TaggedIsSmi(var_input.value()), &if_smi);
  // We can handle both HeapNumber and Oddball here, since Oddball has the
  // same layout as the HeapNumber for the HeapNumber::value field. This
  // way we can also properly optimize stores of oddballs to typed arrays.
  TNode<HeapObject> heap_object = CAST(var_input.value());
  GotoIf(IsHeapNumber(heap_object), &if_heapnumber_or_oddball);
  STATIC_ASSERT_FIELD_OFFSETS_EQUAL(HeapNumber::kValueOffset,
                                    Oddball::kToNumberRawOffset);
  Branch(HasInstanceType(heap_object, ODDBALL_TYPE), &if_heapnumber_or_oddball,
         &convert);

  BIND(&if_heapnumber_or_oddball);
  {
    var_result =
        LoadObjectField<Float64T>(heap_object, HeapNumber::kValueOffset);
    Goto(&done);
  }

  BIND(&if_smi);
  {
    TNode<Int32T> value = SmiToInt32(CAST(var_input.value()));
    var_result = ChangeInt32ToFloat64(value);
    Goto(&done);
  }

  BIND(&convert);
  {
    var_input = CallBuiltin(Builtins::kNonNumberToNumber, context, input);
    Goto(&loop);
  }

  BIND(&done);
  return var_result.value();
}

Node* CodeStubAssembler::PrepareValueForWriteToTypedArray(
    TNode<Object> input, ElementsKind elements_kind, TNode<Context> context) {
  DCHECK(IsTypedArrayElementsKind(elements_kind));

  switch (elements_kind) {
    case UINT8_ELEMENTS:
    case INT8_ELEMENTS:
    case UINT16_ELEMENTS:
    case INT16_ELEMENTS:
    case UINT32_ELEMENTS:
    case INT32_ELEMENTS:
    case UINT8_CLAMPED_ELEMENTS:
      return PrepareValueForWriteToTypedArray<Word32T>(input, elements_kind,
                                                       context);
    case FLOAT32_ELEMENTS:
      return PrepareValueForWriteToTypedArray<Float32T>(input, elements_kind,
                                                        context);
    case FLOAT64_ELEMENTS:
      return PrepareValueForWriteToTypedArray<Float64T>(input, elements_kind,
                                                        context);
    case BIGINT64_ELEMENTS:
    case BIGUINT64_ELEMENTS:
      return ToBigInt(context, input);
    default:
      UNREACHABLE();
  }
}

void CodeStubAssembler::BigIntToRawBytes(TNode<BigInt> bigint,
                                         TVariable<UintPtrT>* var_low,
                                         TVariable<UintPtrT>* var_high) {
  Label done(this);
  *var_low = Unsigned(IntPtrConstant(0));
  *var_high = Unsigned(IntPtrConstant(0));
  TNode<Word32T> bitfield = LoadBigIntBitfield(bigint);
  TNode<Uint32T> length = DecodeWord32<BigIntBase::LengthBits>(bitfield);
  TNode<Uint32T> sign = DecodeWord32<BigIntBase::SignBits>(bitfield);
  GotoIf(Word32Equal(length, Int32Constant(0)), &done);
  *var_low = LoadBigIntDigit(bigint, 0);
  if (!Is64()) {
    Label load_done(this);
    GotoIf(Word32Equal(length, Int32Constant(1)), &load_done);
    *var_high = LoadBigIntDigit(bigint, 1);
    Goto(&load_done);
    BIND(&load_done);
  }
  GotoIf(Word32Equal(sign, Int32Constant(0)), &done);
  // Negative value. Simulate two's complement.
  if (!Is64()) {
    *var_high = Unsigned(IntPtrSub(IntPtrConstant(0), var_high->value()));
    Label no_carry(this);
    GotoIf(IntPtrEqual(var_low->value(), IntPtrConstant(0)), &no_carry);
    *var_high = Unsigned(IntPtrSub(var_high->value(), IntPtrConstant(1)));
    Goto(&no_carry);
    BIND(&no_carry);
  }
  *var_low = Unsigned(IntPtrSub(IntPtrConstant(0), var_low->value()));
  Goto(&done);
  BIND(&done);
}

void CodeStubAssembler::EmitElementStore(
    TNode<JSObject> object, TNode<Object> key, TNode<Object> value,
    ElementsKind elements_kind, KeyedAccessStoreMode store_mode, Label* bailout,
    TNode<Context> context, TVariable<Object>* maybe_converted_value) {
  CSA_ASSERT(this, Word32BinaryNot(IsJSProxy(object)));

  TNode<FixedArrayBase> elements = LoadElements(object);
  if (!(IsSmiOrObjectElementsKind(elements_kind) ||
        IsSealedElementsKind(elements_kind) ||
        IsNonextensibleElementsKind(elements_kind))) {
    CSA_ASSERT(this, Word32BinaryNot(IsFixedCOWArrayMap(LoadMap(elements))));
  } else if (!IsCOWHandlingStoreMode(store_mode)) {
    GotoIf(IsFixedCOWArrayMap(LoadMap(elements)), bailout);
  }

  // TODO(ishell): introduce TryToIntPtrOrSmi() and use BInt.
  TNode<IntPtrT> intptr_key = TryToIntptr(key, bailout);

  // TODO(rmcilroy): TNodify the converted value once this funciton and
  // StoreElement are templated based on the type elements_kind type.
  Node* converted_value = value;
  if (IsTypedArrayElementsKind(elements_kind)) {
    Label done(this), update_value_and_bailout(this, Label::kDeferred);

    // IntegerIndexedElementSet converts value to a Number/BigInt prior to the
    // bounds check.
    converted_value =
        PrepareValueForWriteToTypedArray(value, elements_kind, context);
    TNode<JSTypedArray> typed_array = CAST(object);

    // There must be no allocations between the buffer load and
    // and the actual store to backing store, because GC may decide that
    // the buffer is not alive or move the elements.
    // TODO(ishell): introduce DisallowHeapAllocationCode scope here.

    // Check if buffer has been detached.
    TNode<JSArrayBuffer> buffer = LoadJSArrayBufferViewBuffer(typed_array);
    if (maybe_converted_value) {
      GotoIf(IsDetachedBuffer(buffer), &update_value_and_bailout);
    } else {
      GotoIf(IsDetachedBuffer(buffer), bailout);
    }

    // Bounds check.
    TNode<UintPtrT> length = LoadJSTypedArrayLength(typed_array);

    if (store_mode == STORE_IGNORE_OUT_OF_BOUNDS) {
      // Skip the store if we write beyond the length or
      // to a property with a negative integer index.
      GotoIfNot(UintPtrLessThan(intptr_key, length), &done);
    } else {
      DCHECK_EQ(store_mode, STANDARD_STORE);
      GotoIfNot(UintPtrLessThan(intptr_key, length), &update_value_and_bailout);
    }

    TNode<RawPtrT> data_ptr = LoadJSTypedArrayDataPtr(typed_array);
    StoreElement(data_ptr, elements_kind, intptr_key, converted_value);
    Goto(&done);

    BIND(&update_value_and_bailout);
    // We already prepared the incoming value for storing into a typed array.
    // This might involve calling ToNumber in some cases. We shouldn't call
    // ToNumber again in the runtime so pass the converted value to the runtime.
    // The prepared value is an untagged value. Convert it to a tagged value
    // to pass it to runtime. It is not possible to do the detached buffer check
    // before we prepare the value, since ToNumber can detach the ArrayBuffer.
    // The spec specifies the order of these operations.
    if (maybe_converted_value != nullptr) {
      switch (elements_kind) {
        case UINT8_ELEMENTS:
        case INT8_ELEMENTS:
        case UINT16_ELEMENTS:
        case INT16_ELEMENTS:
        case UINT8_CLAMPED_ELEMENTS:
          *maybe_converted_value = SmiFromInt32(converted_value);
          break;
        case UINT32_ELEMENTS:
          *maybe_converted_value = ChangeUint32ToTagged(converted_value);
          break;
        case INT32_ELEMENTS:
          *maybe_converted_value = ChangeInt32ToTagged(converted_value);
          break;
        case FLOAT32_ELEMENTS: {
          Label dont_allocate_heap_number(this), end(this);
          GotoIf(TaggedIsSmi(value), &dont_allocate_heap_number);
          GotoIf(IsHeapNumber(CAST(value)), &dont_allocate_heap_number);
          {
            *maybe_converted_value = AllocateHeapNumberWithValue(
                ChangeFloat32ToFloat64(converted_value));
            Goto(&end);
          }
          BIND(&dont_allocate_heap_number);
          {
            *maybe_converted_value = value;
            Goto(&end);
          }
          BIND(&end);
          break;
        }
        case FLOAT64_ELEMENTS: {
          Label dont_allocate_heap_number(this), end(this);
          GotoIf(TaggedIsSmi(value), &dont_allocate_heap_number);
          GotoIf(IsHeapNumber(CAST(value)), &dont_allocate_heap_number);
          {
            *maybe_converted_value =
                AllocateHeapNumberWithValue(converted_value);
            Goto(&end);
          }
          BIND(&dont_allocate_heap_number);
          {
            *maybe_converted_value = value;
            Goto(&end);
          }
          BIND(&end);
          break;
        }
        case BIGINT64_ELEMENTS:
        case BIGUINT64_ELEMENTS:
          *maybe_converted_value = CAST(converted_value);
          break;
        default:
          UNREACHABLE();
      }
    }
    Goto(bailout);

    BIND(&done);
    return;
  }
  DCHECK(IsFastElementsKind(elements_kind) ||
         IsSealedElementsKind(elements_kind) ||
         IsNonextensibleElementsKind(elements_kind));

  // In case value is stored into a fast smi array, assure that the value is
  // a smi before manipulating the backing store. Otherwise the backing store
  // may be left in an invalid state.
  if (IsSmiElementsKind(elements_kind)) {
    GotoIfNot(TaggedIsSmi(value), bailout);
  } else if (IsDoubleElementsKind(elements_kind)) {
    converted_value = TryTaggedToFloat64(value, bailout);
  }

  TNode<Smi> smi_length = Select<Smi>(
      IsJSArray(object),
      [=]() {
        // This is casting Number -> Smi which may not actually be safe.
        return CAST(LoadJSArrayLength(CAST(object)));
      },
      [=]() { return LoadFixedArrayBaseLength(elements); });

  TNode<UintPtrT> length = Unsigned(SmiUntag(smi_length));
  if (IsGrowStoreMode(store_mode) &&
      !(IsSealedElementsKind(elements_kind) ||
        IsNonextensibleElementsKind(elements_kind))) {
    elements = CheckForCapacityGrow(object, elements, elements_kind, length,
                                    intptr_key, bailout);
  } else {
    GotoIfNot(UintPtrLessThan(Unsigned(intptr_key), length), bailout);
  }

  // Cannot store to a hole in holey sealed elements so bailout.
  if (elements_kind == HOLEY_SEALED_ELEMENTS ||
      elements_kind == HOLEY_NONEXTENSIBLE_ELEMENTS) {
    TNode<Object> target_value =
        LoadFixedArrayElement(CAST(elements), intptr_key);
    GotoIf(IsTheHole(target_value), bailout);
  }

  // If we didn't grow {elements}, it might still be COW, in which case we
  // copy it now.
  if (!(IsSmiOrObjectElementsKind(elements_kind) ||
        IsSealedElementsKind(elements_kind) ||
        IsNonextensibleElementsKind(elements_kind))) {
    CSA_ASSERT(this, Word32BinaryNot(IsFixedCOWArrayMap(LoadMap(elements))));
  } else if (IsCOWHandlingStoreMode(store_mode)) {
    elements = CopyElementsOnWrite(object, elements, elements_kind,
                                   Signed(length), bailout);
  }

  CSA_ASSERT(this, Word32BinaryNot(IsFixedCOWArrayMap(LoadMap(elements))));
  StoreElement(elements, elements_kind, intptr_key, converted_value);
}

TNode<FixedArrayBase> CodeStubAssembler::CheckForCapacityGrow(
    TNode<JSObject> object, TNode<FixedArrayBase> elements, ElementsKind kind,
    TNode<UintPtrT> length, TNode<IntPtrT> key, Label* bailout) {
  DCHECK(IsFastElementsKind(kind));
  TVARIABLE(FixedArrayBase, checked_elements);
  Label grow_case(this), no_grow_case(this), done(this),
      grow_bailout(this, Label::kDeferred);

  TNode<BoolT> condition;
  if (IsHoleyElementsKind(kind)) {
    condition = UintPtrGreaterThanOrEqual(key, length);
  } else {
    // We don't support growing here unless the value is being appended.
    condition = WordEqual(key, length);
  }
  Branch(condition, &grow_case, &no_grow_case);

  BIND(&grow_case);
  {
    TNode<IntPtrT> current_capacity =
        SmiUntag(LoadFixedArrayBaseLength(elements));
    checked_elements = elements;
    Label fits_capacity(this);
    // If key is negative, we will notice in Runtime::kGrowArrayElements.
    GotoIf(UintPtrLessThan(key, current_capacity), &fits_capacity);

    {
      TNode<FixedArrayBase> new_elements = TryGrowElementsCapacity(
          object, elements, kind, key, current_capacity, &grow_bailout);
      checked_elements = new_elements;
      Goto(&fits_capacity);
    }

    BIND(&grow_bailout);
    {
      GotoIf(IntPtrLessThan(key, IntPtrConstant(0)), bailout);
      TNode<Number> tagged_key = ChangeUintPtrToTagged(Unsigned(key));
      TNode<Object> maybe_elements = CallRuntime(
          Runtime::kGrowArrayElements, NoContextConstant(), object, tagged_key);
      GotoIf(TaggedIsSmi(maybe_elements), bailout);
      TNode<FixedArrayBase> new_elements = CAST(maybe_elements);
      CSA_ASSERT(this, IsFixedArrayWithKind(new_elements, kind));
      checked_elements = new_elements;
      Goto(&fits_capacity);
    }

    BIND(&fits_capacity);
    GotoIfNot(IsJSArray(object), &done);

    TNode<IntPtrT> new_length = IntPtrAdd(key, IntPtrConstant(1));
    StoreObjectFieldNoWriteBarrier(object, JSArray::kLengthOffset,
                                   SmiTag(new_length));
    Goto(&done);
  }

  BIND(&no_grow_case);
  {
    GotoIfNot(UintPtrLessThan(key, length), bailout);
    checked_elements = elements;
    Goto(&done);
  }

  BIND(&done);
  return checked_elements.value();
}

TNode<FixedArrayBase> CodeStubAssembler::CopyElementsOnWrite(
    TNode<HeapObject> object, TNode<FixedArrayBase> elements, ElementsKind kind,
    TNode<IntPtrT> length, Label* bailout) {
  TVARIABLE(FixedArrayBase, new_elements_var, elements);
  Label done(this);

  GotoIfNot(IsFixedCOWArrayMap(LoadMap(elements)), &done);
  {
    TNode<IntPtrT> capacity = SmiUntag(LoadFixedArrayBaseLength(elements));
    TNode<FixedArrayBase> new_elements = GrowElementsCapacity(
        object, elements, kind, kind, length, capacity, bailout);
    new_elements_var = new_elements;
    Goto(&done);
  }

  BIND(&done);
  return new_elements_var.value();
}

void CodeStubAssembler::TransitionElementsKind(TNode<JSObject> object,
                                               TNode<Map> map,
                                               ElementsKind from_kind,
                                               ElementsKind to_kind,
                                               Label* bailout) {
  DCHECK(!IsHoleyElementsKind(from_kind) || IsHoleyElementsKind(to_kind));
  if (AllocationSite::ShouldTrack(from_kind, to_kind)) {
    TrapAllocationMemento(object, bailout);
  }

  if (!IsSimpleMapChangeTransition(from_kind, to_kind)) {
    Comment("Non-simple map transition");
    TNode<FixedArrayBase> elements = LoadElements(object);

    Label done(this);
    GotoIf(TaggedEqual(elements, EmptyFixedArrayConstant()), &done);

    // TODO(ishell): Use BInt for elements_length and array_length.
    TNode<IntPtrT> elements_length =
        SmiUntag(LoadFixedArrayBaseLength(elements));
    TNode<IntPtrT> array_length = Select<IntPtrT>(
        IsJSArray(object),
        [=]() {
          CSA_ASSERT(this, IsFastElementsKind(LoadElementsKind(object)));
          return SmiUntag(LoadFastJSArrayLength(CAST(object)));
        },
        [=]() { return elements_length; });

    CSA_ASSERT(this, WordNotEqual(elements_length, IntPtrConstant(0)));

    GrowElementsCapacity(object, elements, from_kind, to_kind, array_length,
                         elements_length, bailout);
    Goto(&done);
    BIND(&done);
  }

  StoreMap(object, map);
}

void CodeStubAssembler::TrapAllocationMemento(TNode<JSObject> object,
                                              Label* memento_found) {
  Comment("[ TrapAllocationMemento");
  Label no_memento_found(this);
  Label top_check(this), map_check(this);

  TNode<ExternalReference> new_space_top_address = ExternalConstant(
      ExternalReference::new_space_allocation_top_address(isolate()));
  const int kMementoMapOffset = JSArray::kHeaderSize;
  const int kMementoLastWordOffset =
      kMementoMapOffset + AllocationMemento::kSize - kTaggedSize;

  // Bail out if the object is not in new space.
  TNode<IntPtrT> object_word = BitcastTaggedToWord(object);
  TNode<IntPtrT> object_page = PageFromAddress(object_word);
  {
    TNode<IntPtrT> page_flags =
        Load<IntPtrT>(object_page, IntPtrConstant(Page::kFlagsOffset));
    GotoIf(WordEqual(
               WordAnd(page_flags,
                       IntPtrConstant(MemoryChunk::kIsInYoungGenerationMask)),
               IntPtrConstant(0)),
           &no_memento_found);
    // TODO(ulan): Support allocation memento for a large object by allocating
    // additional word for the memento after the large object.
    GotoIf(WordNotEqual(WordAnd(page_flags,
                                IntPtrConstant(MemoryChunk::kIsLargePageMask)),
                        IntPtrConstant(0)),
           &no_memento_found);
  }

  TNode<IntPtrT> memento_last_word = IntPtrAdd(
      object_word, IntPtrConstant(kMementoLastWordOffset - kHeapObjectTag));
  TNode<IntPtrT> memento_last_word_page = PageFromAddress(memento_last_word);

  TNode<IntPtrT> new_space_top = Load<IntPtrT>(new_space_top_address);
  TNode<IntPtrT> new_space_top_page = PageFromAddress(new_space_top);

  // If the object is in new space, we need to check whether respective
  // potential memento object is on the same page as the current top.
  GotoIf(WordEqual(memento_last_word_page, new_space_top_page), &top_check);

  // The object is on a different page than allocation top. Bail out if the
  // object sits on the page boundary as no memento can follow and we cannot
  // touch the memory following it.
  Branch(WordEqual(object_page, memento_last_word_page), &map_check,
         &no_memento_found);

  // If top is on the same page as the current object, we need to check whether
  // we are below top.
  BIND(&top_check);
  {
    Branch(UintPtrGreaterThanOrEqual(memento_last_word, new_space_top),
           &no_memento_found, &map_check);
  }

  // Memento map check.
  BIND(&map_check);
  {
    TNode<Object> memento_map = LoadObjectField(object, kMementoMapOffset);
    Branch(TaggedEqual(memento_map, AllocationMementoMapConstant()),
           memento_found, &no_memento_found);
  }
  BIND(&no_memento_found);
  Comment("] TrapAllocationMemento");
}

TNode<IntPtrT> CodeStubAssembler::PageFromAddress(TNode<IntPtrT> address) {
  return WordAnd(address, IntPtrConstant(~kPageAlignmentMask));
}

TNode<AllocationSite> CodeStubAssembler::CreateAllocationSiteInFeedbackVector(
    TNode<FeedbackVector> feedback_vector, TNode<UintPtrT> slot) {
  TNode<IntPtrT> size = IntPtrConstant(AllocationSite::kSizeWithWeakNext);
  TNode<HeapObject> site = Allocate(size, CodeStubAssembler::kPretenured);
  StoreMapNoWriteBarrier(site, RootIndex::kAllocationSiteWithWeakNextMap);
  // Should match AllocationSite::Initialize.
  TNode<WordT> field = UpdateWord<AllocationSite::ElementsKindBits>(
      IntPtrConstant(0), UintPtrConstant(GetInitialFastElementsKind()));
  StoreObjectFieldNoWriteBarrier(
      site, AllocationSite::kTransitionInfoOrBoilerplateOffset,
      SmiTag(Signed(field)));

  // Unlike literals, constructed arrays don't have nested sites
  TNode<Smi> zero = SmiConstant(0);
  StoreObjectFieldNoWriteBarrier(site, AllocationSite::kNestedSiteOffset, zero);

  // Pretenuring calculation field.
  StoreObjectFieldNoWriteBarrier(site, AllocationSite::kPretenureDataOffset,
                                 Int32Constant(0));

  // Pretenuring memento creation count field.
  StoreObjectFieldNoWriteBarrier(
      site, AllocationSite::kPretenureCreateCountOffset, Int32Constant(0));

  // Store an empty fixed array for the code dependency.
  StoreObjectFieldRoot(site, AllocationSite::kDependentCodeOffset,
                       RootIndex::kEmptyWeakFixedArray);

  // Link the object to the allocation site list
  TNode<ExternalReference> site_list = ExternalConstant(
      ExternalReference::allocation_sites_list_address(isolate()));
  TNode<Object> next_site =
      LoadBufferObject(ReinterpretCast<RawPtrT>(site_list), 0);

  // TODO(mvstanton): This is a store to a weak pointer, which we may want to
  // mark as such in order to skip the write barrier, once we have a unified
  // system for weakness. For now we decided to keep it like this because having
  // an initial write barrier backed store makes this pointer strong until the
  // next GC, and allocation sites are designed to survive several GCs anyway.
  StoreObjectField(site, AllocationSite::kWeakNextOffset, next_site);
  StoreFullTaggedNoWriteBarrier(site_list, site);

  StoreFeedbackVectorSlot(feedback_vector, slot, site);
  return CAST(site);
}

TNode<MaybeObject> CodeStubAssembler::StoreWeakReferenceInFeedbackVector(
    TNode<FeedbackVector> feedback_vector, TNode<UintPtrT> slot,
    TNode<HeapObject> value, int additional_offset) {
  TNode<MaybeObject> weak_value = MakeWeak(value);
  StoreFeedbackVectorSlot(feedback_vector, slot, weak_value,
                          UPDATE_WRITE_BARRIER, additional_offset);
  return weak_value;
}

TNode<BoolT> CodeStubAssembler::HasBoilerplate(
    TNode<Object> maybe_literal_site) {
  return TaggedIsNotSmi(maybe_literal_site);
}

TNode<Smi> CodeStubAssembler::LoadTransitionInfo(
    TNode<AllocationSite> allocation_site) {
  TNode<Smi> transition_info = CAST(LoadObjectField(
      allocation_site, AllocationSite::kTransitionInfoOrBoilerplateOffset));
  return transition_info;
}

TNode<JSObject> CodeStubAssembler::LoadBoilerplate(
    TNode<AllocationSite> allocation_site) {
  TNode<JSObject> boilerplate = CAST(LoadObjectField(
      allocation_site, AllocationSite::kTransitionInfoOrBoilerplateOffset));
  return boilerplate;
}

TNode<Int32T> CodeStubAssembler::LoadElementsKind(
    TNode<AllocationSite> allocation_site) {
  TNode<Smi> transition_info = LoadTransitionInfo(allocation_site);
  TNode<Int32T> elements_kind =
      Signed(DecodeWord32<AllocationSite::ElementsKindBits>(
          SmiToInt32(transition_info)));
  CSA_ASSERT(this, IsFastElementsKind(elements_kind));
  return elements_kind;
}

template <typename TIndex>
TNode<TIndex> CodeStubAssembler::BuildFastLoop(const VariableList& vars,
                                               TNode<TIndex> start_index,
                                               TNode<TIndex> end_index,
                                               const FastLoopBody<TIndex>& body,
                                               int increment,
                                               IndexAdvanceMode advance_mode) {
  TVARIABLE(TIndex, var, start_index);
  VariableList vars_copy(vars.begin(), vars.end(), zone());
  vars_copy.push_back(&var);
  Label loop(this, vars_copy);
  Label after_loop(this);
  // Introduce an explicit second check of the termination condition before the
  // loop that helps turbofan generate better code. If there's only a single
  // check, then the CodeStubAssembler forces it to be at the beginning of the
  // loop requiring a backwards branch at the end of the loop (it's not possible
  // to force the loop header check at the end of the loop and branch forward to
  // it from the pre-header). The extra branch is slower in the case that the
  // loop actually iterates.
  TNode<BoolT> first_check = IntPtrOrSmiEqual(var.value(), end_index);
  int32_t first_check_val;
  if (ToInt32Constant(first_check, &first_check_val)) {
    if (first_check_val) return var.value();
    Goto(&loop);
  } else {
    Branch(first_check, &after_loop, &loop);
  }

  BIND(&loop);
  {
    if (advance_mode == IndexAdvanceMode::kPre) {
      Increment(&var, increment);
    }
    body(var.value());
    if (advance_mode == IndexAdvanceMode::kPost) {
      Increment(&var, increment);
    }
    Branch(IntPtrOrSmiNotEqual(var.value(), end_index), &loop, &after_loop);
  }
  BIND(&after_loop);
  return var.value();
}

// Instantiate BuildFastLoop for IntPtrT and UintPtrT.
template V8_EXPORT_PRIVATE TNode<IntPtrT>
CodeStubAssembler::BuildFastLoop<IntPtrT>(const VariableList& vars,
                                          TNode<IntPtrT> start_index,
                                          TNode<IntPtrT> end_index,
                                          const FastLoopBody<IntPtrT>& body,
                                          int increment,
                                          IndexAdvanceMode advance_mode);
template V8_EXPORT_PRIVATE TNode<UintPtrT>
CodeStubAssembler::BuildFastLoop<UintPtrT>(const VariableList& vars,
                                           TNode<UintPtrT> start_index,
                                           TNode<UintPtrT> end_index,
                                           const FastLoopBody<UintPtrT>& body,
                                           int increment,
                                           IndexAdvanceMode advance_mode);

template <typename TIndex>
void CodeStubAssembler::BuildFastArrayForEach(
    TNode<UnionT<UnionT<FixedArray, PropertyArray>, HeapObject>> array,
    ElementsKind kind, TNode<TIndex> first_element_inclusive,
    TNode<TIndex> last_element_exclusive, const FastArrayForEachBody& body,
    ForEachDirection direction) {
  STATIC_ASSERT(FixedArray::kHeaderSize == FixedDoubleArray::kHeaderSize);
  CSA_SLOW_ASSERT(this, Word32Or(IsFixedArrayWithKind(array, kind),
                                 IsPropertyArray(array)));

  int32_t first_val;
  bool constant_first = ToInt32Constant(first_element_inclusive, &first_val);
  int32_t last_val;
  bool constent_last = ToInt32Constant(last_element_exclusive, &last_val);
  if (constant_first && constent_last) {
    int delta = last_val - first_val;
    DCHECK_GE(delta, 0);
    if (delta <= kElementLoopUnrollThreshold) {
      if (direction == ForEachDirection::kForward) {
        for (int i = first_val; i < last_val; ++i) {
          TNode<IntPtrT> index = IntPtrConstant(i);
          TNode<IntPtrT> offset = ElementOffsetFromIndex(
              index, kind, FixedArray::kHeaderSize - kHeapObjectTag);
          body(array, offset);
        }
      } else {
        for (int i = last_val - 1; i >= first_val; --i) {
          TNode<IntPtrT> index = IntPtrConstant(i);
          TNode<IntPtrT> offset = ElementOffsetFromIndex(
              index, kind, FixedArray::kHeaderSize - kHeapObjectTag);
          body(array, offset);
        }
      }
      return;
    }
  }

  TNode<IntPtrT> start = ElementOffsetFromIndex(
      first_element_inclusive, kind, FixedArray::kHeaderSize - kHeapObjectTag);
  TNode<IntPtrT> limit = ElementOffsetFromIndex(
      last_element_exclusive, kind, FixedArray::kHeaderSize - kHeapObjectTag);
  if (direction == ForEachDirection::kReverse) std::swap(start, limit);

  int increment = IsDoubleElementsKind(kind) ? kDoubleSize : kTaggedSize;
  BuildFastLoop<IntPtrT>(
      start, limit, [&](TNode<IntPtrT> offset) { body(array, offset); },
      direction == ForEachDirection::kReverse ? -increment : increment,
      direction == ForEachDirection::kReverse ? IndexAdvanceMode::kPre
                                              : IndexAdvanceMode::kPost);
}

template <typename TIndex>
void CodeStubAssembler::GotoIfFixedArraySizeDoesntFitInNewSpace(
    TNode<TIndex> element_count, Label* doesnt_fit, int base_size) {
  GotoIf(FixedArraySizeDoesntFitInNewSpace(element_count, base_size),
         doesnt_fit);
}

void CodeStubAssembler::InitializeFieldsWithRoot(TNode<HeapObject> object,
                                                 TNode<IntPtrT> start_offset,
                                                 TNode<IntPtrT> end_offset,
                                                 RootIndex root_index) {
  CSA_SLOW_ASSERT(this, TaggedIsNotSmi(object));
  start_offset = IntPtrAdd(start_offset, IntPtrConstant(-kHeapObjectTag));
  end_offset = IntPtrAdd(end_offset, IntPtrConstant(-kHeapObjectTag));
  TNode<Object> root_value = LoadRoot(root_index);
  BuildFastLoop<IntPtrT>(
      end_offset, start_offset,
      [=](TNode<IntPtrT> current) {
        StoreNoWriteBarrier(MachineRepresentation::kTagged, object, current,
                            root_value);
      },
      -kTaggedSize, CodeStubAssembler::IndexAdvanceMode::kPre);
}

void CodeStubAssembler::BranchIfNumberRelationalComparison(Operation op,
                                                           TNode<Number> left,
                                                           TNode<Number> right,
                                                           Label* if_true,
                                                           Label* if_false) {
  Label do_float_comparison(this);
  TVARIABLE(Float64T, var_left_float);
  TVARIABLE(Float64T, var_right_float);

  Branch(
      TaggedIsSmi(left),
      [&] {
        TNode<Smi> smi_left = CAST(left);

        Branch(
            TaggedIsSmi(right),
            [&] {
              TNode<Smi> smi_right = CAST(right);

              // Both {left} and {right} are Smi, so just perform a fast
              // Smi comparison.
              switch (op) {
                case Operation::kEqual:
                  BranchIfSmiEqual(smi_left, smi_right, if_true, if_false);
                  break;
                case Operation::kLessThan:
                  BranchIfSmiLessThan(smi_left, smi_right, if_true, if_false);
                  break;
                case Operation::kLessThanOrEqual:
                  BranchIfSmiLessThanOrEqual(smi_left, smi_right, if_true,
                                             if_false);
                  break;
                case Operation::kGreaterThan:
                  BranchIfSmiLessThan(smi_right, smi_left, if_true, if_false);
                  break;
                case Operation::kGreaterThanOrEqual:
                  BranchIfSmiLessThanOrEqual(smi_right, smi_left, if_true,
                                             if_false);
                  break;
                default:
                  UNREACHABLE();
              }
            },
            [&] {
              var_left_float = SmiToFloat64(smi_left);
              var_right_float = LoadHeapNumberValue(CAST(right));
              Goto(&do_float_comparison);
            });
      },
      [&] {
        var_left_float = LoadHeapNumberValue(CAST(left));

        Branch(
            TaggedIsSmi(right),
            [&] {
              var_right_float = SmiToFloat64(CAST(right));
              Goto(&do_float_comparison);
            },
            [&] {
              var_right_float = LoadHeapNumberValue(CAST(right));
              Goto(&do_float_comparison);
            });
      });

  BIND(&do_float_comparison);
  {
    switch (op) {
      case Operation::kEqual:
        Branch(Float64Equal(var_left_float.value(), var_right_float.value()),
               if_true, if_false);
        break;
      case Operation::kLessThan:
        Branch(Float64LessThan(var_left_float.value(), var_right_float.value()),
               if_true, if_false);
        break;
      case Operation::kLessThanOrEqual:
        Branch(Float64LessThanOrEqual(var_left_float.value(),
                                      var_right_float.value()),
               if_true, if_false);
        break;
      case Operation::kGreaterThan:
        Branch(
            Float64GreaterThan(var_left_float.value(), var_right_float.value()),
            if_true, if_false);
        break;
      case Operation::kGreaterThanOrEqual:
        Branch(Float64GreaterThanOrEqual(var_left_float.value(),
                                         var_right_float.value()),
               if_true, if_false);
        break;
      default:
        UNREACHABLE();
    }
  }
}

void CodeStubAssembler::GotoIfNumberGreaterThanOrEqual(TNode<Number> left,
                                                       TNode<Number> right,
                                                       Label* if_true) {
  Label if_false(this);
  BranchIfNumberRelationalComparison(Operation::kGreaterThanOrEqual, left,
                                     right, if_true, &if_false);
  BIND(&if_false);
}

namespace {
Operation Reverse(Operation op) {
  switch (op) {
    case Operation::kLessThan:
      return Operation::kGreaterThan;
    case Operation::kLessThanOrEqual:
      return Operation::kGreaterThanOrEqual;
    case Operation::kGreaterThan:
      return Operation::kLessThan;
    case Operation::kGreaterThanOrEqual:
      return Operation::kLessThanOrEqual;
    default:
      break;
  }
  UNREACHABLE();
}
}  // anonymous namespace

TNode<Oddball> CodeStubAssembler::RelationalComparison(
    Operation op, TNode<Object> left, TNode<Object> right,
    TNode<Context> context, TVariable<Smi>* var_type_feedback) {
  Label return_true(this), return_false(this), do_float_comparison(this),
      end(this);
  TVARIABLE(Oddball, var_result);  // Actually only "true" or "false".
  TVARIABLE(Float64T, var_left_float);
  TVARIABLE(Float64T, var_right_float);

  // We might need to loop several times due to ToPrimitive and/or ToNumeric
  // conversions.
  TVARIABLE(Object, var_left, left);
  TVARIABLE(Object, var_right, right);
  VariableList loop_variable_list({&var_left, &var_right}, zone());
  if (var_type_feedback != nullptr) {
    // Initialize the type feedback to None. The current feedback is combined
    // with the previous feedback.
    *var_type_feedback = SmiConstant(CompareOperationFeedback::kNone);
    loop_variable_list.push_back(var_type_feedback);
  }
  Label loop(this, loop_variable_list);
  Goto(&loop);
  BIND(&loop);
  {
    left = var_left.value();
    right = var_right.value();

    Label if_left_smi(this), if_left_not_smi(this);
    Branch(TaggedIsSmi(left), &if_left_smi, &if_left_not_smi);

    BIND(&if_left_smi);
    {
      TNode<Smi> smi_left = CAST(left);
      Label if_right_smi(this), if_right_heapnumber(this),
          if_right_bigint(this, Label::kDeferred),
          if_right_not_numeric(this, Label::kDeferred);
      GotoIf(TaggedIsSmi(right), &if_right_smi);
      TNode<Map> right_map = LoadMap(CAST(right));
      GotoIf(IsHeapNumberMap(right_map), &if_right_heapnumber);
      TNode<Uint16T> right_instance_type = LoadMapInstanceType(right_map);
      Branch(IsBigIntInstanceType(right_instance_type), &if_right_bigint,
             &if_right_not_numeric);

      BIND(&if_right_smi);
      {
        TNode<Smi> smi_right = CAST(right);
        CombineFeedback(var_type_feedback,
                        CompareOperationFeedback::kSignedSmall);
        switch (op) {
          case Operation::kLessThan:
            BranchIfSmiLessThan(smi_left, smi_right, &return_true,
                                &return_false);
            break;
          case Operation::kLessThanOrEqual:
            BranchIfSmiLessThanOrEqual(smi_left, smi_right, &return_true,
                                       &return_false);
            break;
          case Operation::kGreaterThan:
            BranchIfSmiLessThan(smi_right, smi_left, &return_true,
                                &return_false);
            break;
          case Operation::kGreaterThanOrEqual:
            BranchIfSmiLessThanOrEqual(smi_right, smi_left, &return_true,
                                       &return_false);
            break;
          default:
            UNREACHABLE();
        }
      }

      BIND(&if_right_heapnumber);
      {
        CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);
        var_left_float = SmiToFloat64(smi_left);
        var_right_float = LoadHeapNumberValue(CAST(right));
        Goto(&do_float_comparison);
      }

      BIND(&if_right_bigint);
      {
        OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kAny);
        var_result = CAST(CallRuntime(Runtime::kBigIntCompareToNumber,
                                      NoContextConstant(),
                                      SmiConstant(Reverse(op)), right, left));
        Goto(&end);
      }

      BIND(&if_right_not_numeric);
      {
        OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kAny);
        // Convert {right} to a Numeric; we don't need to perform the
        // dedicated ToPrimitive(right, hint Number) operation, as the
        // ToNumeric(right) will by itself already invoke ToPrimitive with
        // a Number hint.
        var_right = CallBuiltin(Builtins::kNonNumberToNumeric, context, right);
        Goto(&loop);
      }
    }

    BIND(&if_left_not_smi);
    {
      TNode<Map> left_map = LoadMap(CAST(left));

      Label if_right_smi(this), if_right_not_smi(this);
      Branch(TaggedIsSmi(right), &if_right_smi, &if_right_not_smi);

      BIND(&if_right_smi);
      {
        Label if_left_heapnumber(this), if_left_bigint(this, Label::kDeferred),
            if_left_not_numeric(this, Label::kDeferred);
        GotoIf(IsHeapNumberMap(left_map), &if_left_heapnumber);
        TNode<Uint16T> left_instance_type = LoadMapInstanceType(left_map);
        Branch(IsBigIntInstanceType(left_instance_type), &if_left_bigint,
               &if_left_not_numeric);

        BIND(&if_left_heapnumber);
        {
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);
          var_left_float = LoadHeapNumberValue(CAST(left));
          var_right_float = SmiToFloat64(CAST(right));
          Goto(&do_float_comparison);
        }

        BIND(&if_left_bigint);
        {
          OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kAny);
          var_result = CAST(CallRuntime(Runtime::kBigIntCompareToNumber,
                                        NoContextConstant(), SmiConstant(op),
                                        left, right));
          Goto(&end);
        }

        BIND(&if_left_not_numeric);
        {
          OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kAny);
          // Convert {left} to a Numeric; we don't need to perform the
          // dedicated ToPrimitive(left, hint Number) operation, as the
          // ToNumeric(left) will by itself already invoke ToPrimitive with
          // a Number hint.
          var_left = CallBuiltin(Builtins::kNonNumberToNumeric, context, left);
          Goto(&loop);
        }
      }

      BIND(&if_right_not_smi);
      {
        TNode<Map> right_map = LoadMap(CAST(right));

        Label if_left_heapnumber(this), if_left_bigint(this, Label::kDeferred),
            if_left_string(this, Label::kDeferred),
            if_left_other(this, Label::kDeferred);
        GotoIf(IsHeapNumberMap(left_map), &if_left_heapnumber);
        TNode<Uint16T> left_instance_type = LoadMapInstanceType(left_map);
        GotoIf(IsBigIntInstanceType(left_instance_type), &if_left_bigint);
        Branch(IsStringInstanceType(left_instance_type), &if_left_string,
               &if_left_other);

        BIND(&if_left_heapnumber);
        {
          Label if_right_heapnumber(this),
              if_right_bigint(this, Label::kDeferred),
              if_right_not_numeric(this, Label::kDeferred);
          GotoIf(TaggedEqual(right_map, left_map), &if_right_heapnumber);
          TNode<Uint16T> right_instance_type = LoadMapInstanceType(right_map);
          Branch(IsBigIntInstanceType(right_instance_type), &if_right_bigint,
                 &if_right_not_numeric);

          BIND(&if_right_heapnumber);
          {
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kNumber);
            var_left_float = LoadHeapNumberValue(CAST(left));
            var_right_float = LoadHeapNumberValue(CAST(right));
            Goto(&do_float_comparison);
          }

          BIND(&if_right_bigint);
          {
            OverwriteFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            var_result = CAST(CallRuntime(
                Runtime::kBigIntCompareToNumber, NoContextConstant(),
                SmiConstant(Reverse(op)), right, left));
            Goto(&end);
          }

          BIND(&if_right_not_numeric);
          {
            OverwriteFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            // Convert {right} to a Numeric; we don't need to perform
            // dedicated ToPrimitive(right, hint Number) operation, as the
            // ToNumeric(right) will by itself already invoke ToPrimitive with
            // a Number hint.
            var_right =
                CallBuiltin(Builtins::kNonNumberToNumeric, context, right);
            Goto(&loop);
          }
        }

        BIND(&if_left_bigint);
        {
          Label if_right_heapnumber(this), if_right_bigint(this),
              if_right_string(this), if_right_other(this);
          GotoIf(IsHeapNumberMap(right_map), &if_right_heapnumber);
          TNode<Uint16T> right_instance_type = LoadMapInstanceType(right_map);
          GotoIf(IsBigIntInstanceType(right_instance_type), &if_right_bigint);
          Branch(IsStringInstanceType(right_instance_type), &if_right_string,
                 &if_right_other);

          BIND(&if_right_heapnumber);
          {
            OverwriteFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            var_result = CAST(CallRuntime(Runtime::kBigIntCompareToNumber,
                                          NoContextConstant(), SmiConstant(op),
                                          left, right));
            Goto(&end);
          }

          BIND(&if_right_bigint);
          {
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kBigInt);
            var_result = CAST(CallRuntime(Runtime::kBigIntCompareToBigInt,
                                          NoContextConstant(), SmiConstant(op),
                                          left, right));
            Goto(&end);
          }

          BIND(&if_right_string);
          {
            OverwriteFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            var_result = CAST(CallRuntime(Runtime::kBigIntCompareToString,
                                          NoContextConstant(), SmiConstant(op),
                                          left, right));
            Goto(&end);
          }

          // {right} is not a Number, BigInt, or String.
          BIND(&if_right_other);
          {
            OverwriteFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            // Convert {right} to a Numeric; we don't need to perform
            // dedicated ToPrimitive(right, hint Number) operation, as the
            // ToNumeric(right) will by itself already invoke ToPrimitive with
            // a Number hint.
            var_right =
                CallBuiltin(Builtins::kNonNumberToNumeric, context, right);
            Goto(&loop);
          }
        }

        BIND(&if_left_string);
        {
          TNode<Uint16T> right_instance_type = LoadMapInstanceType(right_map);

          Label if_right_not_string(this, Label::kDeferred);
          GotoIfNot(IsStringInstanceType(right_instance_type),
                    &if_right_not_string);

          // Both {left} and {right} are strings.
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kString);
          Builtins::Name builtin;
          switch (op) {
            case Operation::kLessThan:
              builtin = Builtins::kStringLessThan;
              break;
            case Operation::kLessThanOrEqual:
              builtin = Builtins::kStringLessThanOrEqual;
              break;
            case Operation::kGreaterThan:
              builtin = Builtins::kStringGreaterThan;
              break;
            case Operation::kGreaterThanOrEqual:
              builtin = Builtins::kStringGreaterThanOrEqual;
              break;
            default:
              UNREACHABLE();
          }
          var_result = CAST(CallBuiltin(builtin, context, left, right));
          Goto(&end);

          BIND(&if_right_not_string);
          {
            OverwriteFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            // {left} is a String, while {right} isn't. Check if {right} is
            // a BigInt, otherwise call ToPrimitive(right, hint Number) if
            // {right} is a receiver, or ToNumeric(left) and then
            // ToNumeric(right) in the other cases.
            STATIC_ASSERT(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
            Label if_right_bigint(this),
                if_right_receiver(this, Label::kDeferred);
            GotoIf(IsBigIntInstanceType(right_instance_type), &if_right_bigint);
            GotoIf(IsJSReceiverInstanceType(right_instance_type),
                   &if_right_receiver);

            var_left =
                CallBuiltin(Builtins::kNonNumberToNumeric, context, left);
            var_right = CallBuiltin(Builtins::kToNumeric, context, right);
            Goto(&loop);

            BIND(&if_right_bigint);
            {
              var_result = CAST(CallRuntime(
                  Runtime::kBigIntCompareToString, NoContextConstant(),
                  SmiConstant(Reverse(op)), right, left));
              Goto(&end);
            }

            BIND(&if_right_receiver);
            {
              Callable callable = CodeFactory::NonPrimitiveToPrimitive(
                  isolate(), ToPrimitiveHint::kNumber);
              var_right = CallStub(callable, context, right);
              Goto(&loop);
            }
          }
        }

        BIND(&if_left_other);
        {
          // {left} is neither a Numeric nor a String, and {right} is not a Smi.
          if (var_type_feedback != nullptr) {
            // Collect NumberOrOddball feedback if {left} is an Oddball
            // and {right} is either a HeapNumber or Oddball. Otherwise collect
            // Any feedback.
            Label collect_any_feedback(this), collect_oddball_feedback(this),
                collect_feedback_done(this);
            GotoIfNot(InstanceTypeEqual(left_instance_type, ODDBALL_TYPE),
                      &collect_any_feedback);

            GotoIf(IsHeapNumberMap(right_map), &collect_oddball_feedback);
            TNode<Uint16T> right_instance_type = LoadMapInstanceType(right_map);
            Branch(InstanceTypeEqual(right_instance_type, ODDBALL_TYPE),
                   &collect_oddball_feedback, &collect_any_feedback);

            BIND(&collect_oddball_feedback);
            {
              CombineFeedback(var_type_feedback,
                              CompareOperationFeedback::kNumberOrOddball);
              Goto(&collect_feedback_done);
            }

            BIND(&collect_any_feedback);
            {
              OverwriteFeedback(var_type_feedback,
                                CompareOperationFeedback::kAny);
              Goto(&collect_feedback_done);
            }

            BIND(&collect_feedback_done);
          }

          // If {left} is a receiver, call ToPrimitive(left, hint Number).
          // Otherwise call ToNumeric(right) and then ToNumeric(left), the
          // order here is important as it's observable by user code.
          STATIC_ASSERT(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
          Label if_left_receiver(this, Label::kDeferred);
          GotoIf(IsJSReceiverInstanceType(left_instance_type),
                 &if_left_receiver);

          var_right = CallBuiltin(Builtins::kToNumeric, context, right);
          var_left = CallBuiltin(Builtins::kNonNumberToNumeric, context, left);
          Goto(&loop);

          BIND(&if_left_receiver);
          {
            Callable callable = CodeFactory::NonPrimitiveToPrimitive(
                isolate(), ToPrimitiveHint::kNumber);
            var_left = CallStub(callable, context, left);
            Goto(&loop);
          }
        }
      }
    }
  }

  BIND(&do_float_comparison);
  {
    switch (op) {
      case Operation::kLessThan:
        Branch(Float64LessThan(var_left_float.value(), var_right_float.value()),
               &return_true, &return_false);
        break;
      case Operation::kLessThanOrEqual:
        Branch(Float64LessThanOrEqual(var_left_float.value(),
                                      var_right_float.value()),
               &return_true, &return_false);
        break;
      case Operation::kGreaterThan:
        Branch(
            Float64GreaterThan(var_left_float.value(), var_right_float.value()),
            &return_true, &return_false);
        break;
      case Operation::kGreaterThanOrEqual:
        Branch(Float64GreaterThanOrEqual(var_left_float.value(),
                                         var_right_float.value()),
               &return_true, &return_false);
        break;
      default:
        UNREACHABLE();
    }
  }

  BIND(&return_true);
  {
    var_result = TrueConstant();
    Goto(&end);
  }

  BIND(&return_false);
  {
    var_result = FalseConstant();
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

TNode<Smi> CodeStubAssembler::CollectFeedbackForString(
    SloppyTNode<Int32T> instance_type) {
  TNode<Smi> feedback = SelectSmiConstant(
      Word32Equal(
          Word32And(instance_type, Int32Constant(kIsNotInternalizedMask)),
          Int32Constant(kInternalizedTag)),
      CompareOperationFeedback::kInternalizedString,
      CompareOperationFeedback::kString);
  return feedback;
}

void CodeStubAssembler::GenerateEqual_Same(SloppyTNode<Object> value,
                                           Label* if_equal, Label* if_notequal,
                                           TVariable<Smi>* var_type_feedback) {
  // In case of abstract or strict equality checks, we need additional checks
  // for NaN values because they are not considered equal, even if both the
  // left and the right hand side reference exactly the same value.

  Label if_smi(this), if_heapnumber(this);
  GotoIf(TaggedIsSmi(value), &if_smi);

  TNode<HeapObject> value_heapobject = CAST(value);
  TNode<Map> value_map = LoadMap(value_heapobject);
  GotoIf(IsHeapNumberMap(value_map), &if_heapnumber);

  // For non-HeapNumbers, all we do is collect type feedback.
  if (var_type_feedback != nullptr) {
    TNode<Uint16T> instance_type = LoadMapInstanceType(value_map);

    Label if_string(this), if_receiver(this), if_oddball(this), if_symbol(this),
        if_bigint(this);
    GotoIf(IsStringInstanceType(instance_type), &if_string);
    GotoIf(IsJSReceiverInstanceType(instance_type), &if_receiver);
    GotoIf(IsOddballInstanceType(instance_type), &if_oddball);
    Branch(IsBigIntInstanceType(instance_type), &if_bigint, &if_symbol);

    BIND(&if_string);
    {
      CSA_ASSERT(this, IsString(value_heapobject));
      CombineFeedback(var_type_feedback,
                      CollectFeedbackForString(instance_type));
      Goto(if_equal);
    }

    BIND(&if_symbol);
    {
      CSA_ASSERT(this, IsSymbol(value_heapobject));
      CombineFeedback(var_type_feedback, CompareOperationFeedback::kSymbol);
      Goto(if_equal);
    }

    BIND(&if_receiver);
    {
      CSA_ASSERT(this, IsJSReceiver(value_heapobject));
      CombineFeedback(var_type_feedback, CompareOperationFeedback::kReceiver);
      Goto(if_equal);
    }

    BIND(&if_bigint);
    {
      CSA_ASSERT(this, IsBigInt(value_heapobject));
      CombineFeedback(var_type_feedback, CompareOperationFeedback::kBigInt);
      Goto(if_equal);
    }

    BIND(&if_oddball);
    {
      CSA_ASSERT(this, IsOddball(value_heapobject));
      Label if_boolean(this), if_not_boolean(this);
      Branch(IsBooleanMap(value_map), &if_boolean, &if_not_boolean);

      BIND(&if_boolean);
      {
        CombineFeedback(var_type_feedback, CompareOperationFeedback::kBoolean);
        Goto(if_equal);
      }

      BIND(&if_not_boolean);
      {
        CSA_ASSERT(this, IsNullOrUndefined(value_heapobject));
        CombineFeedback(var_type_feedback,
                        CompareOperationFeedback::kReceiverOrNullOrUndefined);
        Goto(if_equal);
      }
    }
  } else {
    Goto(if_equal);
  }

  BIND(&if_heapnumber);
  {
    CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);
    TNode<Float64T> number_value = LoadHeapNumberValue(value_heapobject);
    BranchIfFloat64IsNaN(number_value, if_notequal, if_equal);
  }

  BIND(&if_smi);
  {
    CombineFeedback(var_type_feedback, CompareOperationFeedback::kSignedSmall);
    Goto(if_equal);
  }
}

// ES6 section 7.2.12 Abstract Equality Comparison
TNode<Oddball> CodeStubAssembler::Equal(SloppyTNode<Object> left,
                                        SloppyTNode<Object> right,
                                        TNode<Context> context,
                                        TVariable<Smi>* var_type_feedback) {
  // This is a slightly optimized version of Object::Equals. Whenever you
  // change something functionality wise in here, remember to update the
  // Object::Equals method as well.

  Label if_equal(this), if_notequal(this), do_float_comparison(this),
      do_right_stringtonumber(this, Label::kDeferred), end(this);
  TVARIABLE(Oddball, result);
  TVARIABLE(Float64T, var_left_float);
  TVARIABLE(Float64T, var_right_float);

  // We can avoid code duplication by exploiting the fact that abstract equality
  // is symmetric.
  Label use_symmetry(this);

  // We might need to loop several times due to ToPrimitive and/or ToNumber
  // conversions.
  TVARIABLE(Object, var_left, left);
  TVARIABLE(Object, var_right, right);
  VariableList loop_variable_list({&var_left, &var_right}, zone());
  if (var_type_feedback != nullptr) {
    // Initialize the type feedback to None. The current feedback will be
    // combined with the previous feedback.
    OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kNone);
    loop_variable_list.push_back(var_type_feedback);
  }
  Label loop(this, loop_variable_list);
  Goto(&loop);
  BIND(&loop);
  {
    left = var_left.value();
    right = var_right.value();

    Label if_notsame(this);
    GotoIf(TaggedNotEqual(left, right), &if_notsame);
    {
      // {left} and {right} reference the exact same value, yet we need special
      // treatment for HeapNumber, as NaN is not equal to NaN.
      GenerateEqual_Same(left, &if_equal, &if_notequal, var_type_feedback);
    }

    BIND(&if_notsame);
    Label if_left_smi(this), if_left_not_smi(this);
    Branch(TaggedIsSmi(left), &if_left_smi, &if_left_not_smi);

    BIND(&if_left_smi);
    {
      Label if_right_smi(this), if_right_not_smi(this);
      CombineFeedback(var_type_feedback,
                      CompareOperationFeedback::kSignedSmall);
      Branch(TaggedIsSmi(right), &if_right_smi, &if_right_not_smi);

      BIND(&if_right_smi);
      {
        // We have already checked for {left} and {right} being the same value,
        // so when we get here they must be different Smis.
        Goto(&if_notequal);
      }

      BIND(&if_right_not_smi);
      {
        TNode<Map> right_map = LoadMap(CAST(right));
        Label if_right_heapnumber(this), if_right_boolean(this),
            if_right_oddball(this), if_right_bigint(this, Label::kDeferred),
            if_right_receiver(this, Label::kDeferred);
        GotoIf(IsHeapNumberMap(right_map), &if_right_heapnumber);

        // {left} is Smi and {right} is not HeapNumber or Smi.
        TNode<Uint16T> right_type = LoadMapInstanceType(right_map);
        GotoIf(IsStringInstanceType(right_type), &do_right_stringtonumber);
        GotoIf(IsOddballInstanceType(right_type), &if_right_oddball);
        GotoIf(IsBigIntInstanceType(right_type), &if_right_bigint);
        GotoIf(IsJSReceiverInstanceType(right_type), &if_right_receiver);
        CombineFeedback(var_type_feedback, CompareOperationFeedback::kAny);
        Goto(&if_notequal);

        BIND(&if_right_heapnumber);
        {
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);
          var_left_float = SmiToFloat64(CAST(left));
          var_right_float = LoadHeapNumberValue(CAST(right));
          Goto(&do_float_comparison);
        }

        BIND(&if_right_oddball);
        {
          Label if_right_boolean(this);
          GotoIf(IsBooleanMap(right_map), &if_right_boolean);
          CombineFeedback(var_type_feedback,
                          CompareOperationFeedback::kOddball);
          Goto(&if_notequal);

          BIND(&if_right_boolean);
          {
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kBoolean);
            var_right = LoadObjectField(CAST(right), Oddball::kToNumberOffset);
            Goto(&loop);
          }
        }

        BIND(&if_right_bigint);
        {
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kBigInt);
          result = CAST(CallRuntime(Runtime::kBigIntEqualToNumber,
                                    NoContextConstant(), right, left));
          Goto(&end);
        }

        BIND(&if_right_receiver);
        {
          CombineFeedback(var_type_feedback,
                          CompareOperationFeedback::kReceiver);
          Callable callable = CodeFactory::NonPrimitiveToPrimitive(isolate());
          var_right = CallStub(callable, context, right);
          Goto(&loop);
        }
      }
    }

    BIND(&if_left_not_smi);
    {
      GotoIf(TaggedIsSmi(right), &use_symmetry);

      Label if_left_symbol(this), if_left_number(this),
          if_left_string(this, Label::kDeferred),
          if_left_bigint(this, Label::kDeferred), if_left_oddball(this),
          if_left_receiver(this);

      TNode<Map> left_map = LoadMap(CAST(left));
      TNode<Map> right_map = LoadMap(CAST(right));
      TNode<Uint16T> left_type = LoadMapInstanceType(left_map);
      TNode<Uint16T> right_type = LoadMapInstanceType(right_map);

      GotoIf(IsStringInstanceType(left_type), &if_left_string);
      GotoIf(IsSymbolInstanceType(left_type), &if_left_symbol);
      GotoIf(IsHeapNumberInstanceType(left_type), &if_left_number);
      GotoIf(IsOddballInstanceType(left_type), &if_left_oddball);
      Branch(IsBigIntInstanceType(left_type), &if_left_bigint,
             &if_left_receiver);

      BIND(&if_left_string);
      {
        GotoIfNot(IsStringInstanceType(right_type), &use_symmetry);
        result =
            CAST(CallBuiltin(Builtins::kStringEqual, context, left, right));
        CombineFeedback(var_type_feedback,
                        SmiOr(CollectFeedbackForString(left_type),
                              CollectFeedbackForString(right_type)));
        Goto(&end);
      }

      BIND(&if_left_number);
      {
        Label if_right_not_number(this);

        CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);
        GotoIf(Word32NotEqual(left_type, right_type), &if_right_not_number);

        var_left_float = LoadHeapNumberValue(CAST(left));
        var_right_float = LoadHeapNumberValue(CAST(right));
        Goto(&do_float_comparison);

        BIND(&if_right_not_number);
        {
          Label if_right_oddball(this);

          GotoIf(IsStringInstanceType(right_type), &do_right_stringtonumber);
          GotoIf(IsOddballInstanceType(right_type), &if_right_oddball);
          GotoIf(IsBigIntInstanceType(right_type), &use_symmetry);
          GotoIf(IsJSReceiverInstanceType(right_type), &use_symmetry);
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kAny);
          Goto(&if_notequal);

          BIND(&if_right_oddball);
          {
            Label if_right_boolean(this);
            GotoIf(IsBooleanMap(right_map), &if_right_boolean);
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kOddball);
            Goto(&if_notequal);

            BIND(&if_right_boolean);
            {
              CombineFeedback(var_type_feedback,
                              CompareOperationFeedback::kBoolean);
              var_right =
                  LoadObjectField(CAST(right), Oddball::kToNumberOffset);
              Goto(&loop);
            }
          }
        }
      }

      BIND(&if_left_bigint);
      {
        Label if_right_heapnumber(this), if_right_bigint(this),
            if_right_string(this), if_right_boolean(this);
        CombineFeedback(var_type_feedback, CompareOperationFeedback::kBigInt);

        GotoIf(IsHeapNumberMap(right_map), &if_right_heapnumber);
        GotoIf(IsBigIntInstanceType(right_type), &if_right_bigint);
        GotoIf(IsStringInstanceType(right_type), &if_right_string);
        GotoIf(IsBooleanMap(right_map), &if_right_boolean);
        Branch(IsJSReceiverInstanceType(right_type), &use_symmetry,
               &if_notequal);

        BIND(&if_right_heapnumber);
        {
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);
          result = CAST(CallRuntime(Runtime::kBigIntEqualToNumber,
                                    NoContextConstant(), left, right));
          Goto(&end);
        }

        BIND(&if_right_bigint);
        {
          // We already have BigInt feedback.
          result = CAST(CallRuntime(Runtime::kBigIntEqualToBigInt,
                                    NoContextConstant(), left, right));
          Goto(&end);
        }

        BIND(&if_right_string);
        {
          CombineFeedback(var_type_feedback, CompareOperationFeedback::kString);
          result = CAST(CallRuntime(Runtime::kBigIntEqualToString,
                                    NoContextConstant(), left, right));
          Goto(&end);
        }

        BIND(&if_right_boolean);
        {
          CombineFeedback(var_type_feedback,
                          CompareOperationFeedback::kBoolean);
          var_right = LoadObjectField(CAST(right), Oddball::kToNumberOffset);
          Goto(&loop);
        }
      }

      BIND(&if_left_oddball);
      {
        Label if_left_boolean(this), if_left_not_boolean(this);
        GotoIf(IsBooleanMap(left_map), &if_left_boolean);
        if (var_type_feedback != nullptr) {
          CombineFeedback(var_type_feedback,
                          CompareOperationFeedback::kNullOrUndefined);
          GotoIf(IsUndetectableMap(left_map), &if_left_not_boolean);
        }
        Goto(&if_left_not_boolean);

        BIND(&if_left_not_boolean);
        {
          // {left} is either Null or Undefined. Check if {right} is
          // undetectable (which includes Null and Undefined).
          Label if_right_undetectable(this), if_right_number(this),
              if_right_oddball(this),
              if_right_not_number_or_oddball_or_undetectable(this);
          GotoIf(IsUndetectableMap(right_map), &if_right_undetectable);
          GotoIf(IsHeapNumberInstanceType(right_type), &if_right_number);
          GotoIf(IsOddballInstanceType(right_type), &if_right_oddball);
          Goto(&if_right_not_number_or_oddball_or_undetectable);

          BIND(&if_right_undetectable);
          {
            // If {right} is undetectable, it must be either also
            // Null or Undefined, or a Receiver (aka document.all).
            CombineFeedback(
                var_type_feedback,
                CompareOperationFeedback::kReceiverOrNullOrUndefined);
            Goto(&if_equal);
          }

          BIND(&if_right_number);
          {
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kNumber);
            Goto(&if_notequal);
          }

          BIND(&if_right_oddball);
          {
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kOddball);
            Goto(&if_notequal);
          }

          BIND(&if_right_not_number_or_oddball_or_undetectable);
          {
            if (var_type_feedback != nullptr) {
              // Track whether {right} is Null, Undefined or Receiver.
              CombineFeedback(
                  var_type_feedback,
                  CompareOperationFeedback::kReceiverOrNullOrUndefined);
              GotoIf(IsJSReceiverInstanceType(right_type), &if_notequal);
              CombineFeedback(var_type_feedback,
                              CompareOperationFeedback::kAny);
            }
            Goto(&if_notequal);
          }
        }

        BIND(&if_left_boolean);
        {
          CombineFeedback(var_type_feedback,
                          CompareOperationFeedback::kBoolean);

          // If {right} is a Boolean too, it must be a different Boolean.
          GotoIf(TaggedEqual(right_map, left_map), &if_notequal);

          // Otherwise, convert {left} to number and try again.
          var_left = LoadObjectField(CAST(left), Oddball::kToNumberOffset);
          Goto(&loop);
        }
      }

      BIND(&if_left_symbol);
      {
        Label if_right_receiver(this);
        GotoIf(IsJSReceiverInstanceType(right_type), &if_right_receiver);
        // {right} is not a JSReceiver and also not the same Symbol as {left},
        // so the result is "not equal".
        if (var_type_feedback != nullptr) {
          Label if_right_symbol(this);
          GotoIf(IsSymbolInstanceType(right_type), &if_right_symbol);
          *var_type_feedback = SmiConstant(CompareOperationFeedback::kAny);
          Goto(&if_notequal);

          BIND(&if_right_symbol);
          {
            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kSymbol);
            Goto(&if_notequal);
          }
        } else {
          Goto(&if_notequal);
        }

        BIND(&if_right_receiver);
        {
          // {left} is a Primitive and {right} is a JSReceiver, so swapping
          // the order is not observable.
          if (var_type_feedback != nullptr) {
            *var_type_feedback = SmiConstant(CompareOperationFeedback::kAny);
          }
          Goto(&use_symmetry);
        }
      }

      BIND(&if_left_receiver);
      {
        CSA_ASSERT(this, IsJSReceiverInstanceType(left_type));
        Label if_right_receiver(this), if_right_not_receiver(this);
        Branch(IsJSReceiverInstanceType(right_type), &if_right_receiver,
               &if_right_not_receiver);

        BIND(&if_right_receiver);
        {
          // {left} and {right} are different JSReceiver references.
          CombineFeedback(var_type_feedback,
                          CompareOperationFeedback::kReceiver);
          Goto(&if_notequal);
        }

        BIND(&if_right_not_receiver);
        {
          // Check if {right} is undetectable, which means it must be Null
          // or Undefined, since we already ruled out Receiver for {right}.
          Label if_right_undetectable(this),
              if_right_not_undetectable(this, Label::kDeferred);
          Branch(IsUndetectableMap(right_map), &if_right_undetectable,
                 &if_right_not_undetectable);

          BIND(&if_right_undetectable);
          {
            // When we get here, {right} must be either Null or Undefined.
            CSA_ASSERT(this, IsNullOrUndefined(right));
            if (var_type_feedback != nullptr) {
              *var_type_feedback = SmiConstant(
                  CompareOperationFeedback::kReceiverOrNullOrUndefined);
            }
            Branch(IsUndetectableMap(left_map), &if_equal, &if_notequal);
          }

          BIND(&if_right_not_undetectable);
          {
            // {right} is a Primitive, and neither Null or Undefined;
            // convert {left} to Primitive too.
            CombineFeedback(var_type_feedback, CompareOperationFeedback::kAny);
            Callable callable = CodeFactory::NonPrimitiveToPrimitive(isolate());
            var_left = CallStub(callable, context, left);
            Goto(&loop);
          }
        }
      }
    }

    BIND(&do_right_stringtonumber);
    {
      if (var_type_feedback != nullptr) {
        TNode<Map> right_map = LoadMap(CAST(right));
        TNode<Uint16T> right_type = LoadMapInstanceType(right_map);
        CombineFeedback(var_type_feedback,
                        CollectFeedbackForString(right_type));
      }
      var_right = CallBuiltin(Builtins::kStringToNumber, context, right);
      Goto(&loop);
    }

    BIND(&use_symmetry);
    {
      var_left = right;
      var_right = left;
      Goto(&loop);
    }
  }

  BIND(&do_float_comparison);
  {
    Branch(Float64Equal(var_left_float.value(), var_right_float.value()),
           &if_equal, &if_notequal);
  }

  BIND(&if_equal);
  {
    result = TrueConstant();
    Goto(&end);
  }

  BIND(&if_notequal);
  {
    result = FalseConstant();
    Goto(&end);
  }

  BIND(&end);
  return result.value();
}

TNode<Oddball> CodeStubAssembler::StrictEqual(
    SloppyTNode<Object> lhs, SloppyTNode<Object> rhs,
    TVariable<Smi>* var_type_feedback) {
  // Pseudo-code for the algorithm below:
  //
  // if (lhs == rhs) {
  //   if (lhs->IsHeapNumber()) return HeapNumber::cast(lhs)->value() != NaN;
  //   return true;
  // }
  // if (!lhs->IsSmi()) {
  //   if (lhs->IsHeapNumber()) {
  //     if (rhs->IsSmi()) {
  //       return Smi::ToInt(rhs) == HeapNumber::cast(lhs)->value();
  //     } else if (rhs->IsHeapNumber()) {
  //       return HeapNumber::cast(rhs)->value() ==
  //       HeapNumber::cast(lhs)->value();
  //     } else {
  //       return false;
  //     }
  //   } else {
  //     if (rhs->IsSmi()) {
  //       return false;
  //     } else {
  //       if (lhs->IsString()) {
  //         if (rhs->IsString()) {
  //           return %StringEqual(lhs, rhs);
  //         } else {
  //           return false;
  //         }
  //       } else if (lhs->IsBigInt()) {
  //         if (rhs->IsBigInt()) {
  //           return %BigIntEqualToBigInt(lhs, rhs);
  //         } else {
  //           return false;
  //         }
  //       } else {
  //         return false;
  //       }
  //     }
  //   }
  // } else {
  //   if (rhs->IsSmi()) {
  //     return false;
  //   } else {
  //     if (rhs->IsHeapNumber()) {
  //       return Smi::ToInt(lhs) == HeapNumber::cast(rhs)->value();
  //     } else {
  //       return false;
  //     }
  //   }
  // }

  Label if_equal(this), if_notequal(this), if_not_equivalent_types(this),
      end(this);
  TVARIABLE(Oddball, result);

  OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kNone);

  // Check if {lhs} and {rhs} refer to the same object.
  Label if_same(this), if_notsame(this);
  Branch(TaggedEqual(lhs, rhs), &if_same, &if_notsame);

  BIND(&if_same);
  {
    // The {lhs} and {rhs} reference the exact same value, yet we need special
    // treatment for HeapNumber, as NaN is not equal to NaN.
    GenerateEqual_Same(lhs, &if_equal, &if_notequal, var_type_feedback);
  }

  BIND(&if_notsame);
  {
    // The {lhs} and {rhs} reference different objects, yet for Smi, HeapNumber,
    // BigInt and String they can still be considered equal.

    // Check if {lhs} is a Smi or a HeapObject.
    Label if_lhsissmi(this), if_lhsisnotsmi(this);
    Branch(TaggedIsSmi(lhs), &if_lhsissmi, &if_lhsisnotsmi);

    BIND(&if_lhsisnotsmi);
    {
      // Load the map of {lhs}.
      TNode<Map> lhs_map = LoadMap(CAST(lhs));

      // Check if {lhs} is a HeapNumber.
      Label if_lhsisnumber(this), if_lhsisnotnumber(this);
      Branch(IsHeapNumberMap(lhs_map), &if_lhsisnumber, &if_lhsisnotnumber);

      BIND(&if_lhsisnumber);
      {
        // Check if {rhs} is a Smi or a HeapObject.
        Label if_rhsissmi(this), if_rhsisnotsmi(this);
        Branch(TaggedIsSmi(rhs), &if_rhsissmi, &if_rhsisnotsmi);

        BIND(&if_rhsissmi);
        {
          // Convert {lhs} and {rhs} to floating point values.
          TNode<Float64T> lhs_value = LoadHeapNumberValue(CAST(lhs));
          TNode<Float64T> rhs_value = SmiToFloat64(CAST(rhs));

          CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);

          // Perform a floating point comparison of {lhs} and {rhs}.
          Branch(Float64Equal(lhs_value, rhs_value), &if_equal, &if_notequal);
        }

        BIND(&if_rhsisnotsmi);
        {
          TNode<HeapObject> rhs_ho = CAST(rhs);
          // Load the map of {rhs}.
          TNode<Map> rhs_map = LoadMap(rhs_ho);

          // Check if {rhs} is also a HeapNumber.
          Label if_rhsisnumber(this), if_rhsisnotnumber(this);
          Branch(IsHeapNumberMap(rhs_map), &if_rhsisnumber, &if_rhsisnotnumber);

          BIND(&if_rhsisnumber);
          {
            // Convert {lhs} and {rhs} to floating point values.
            TNode<Float64T> lhs_value = LoadHeapNumberValue(CAST(lhs));
            TNode<Float64T> rhs_value = LoadHeapNumberValue(CAST(rhs));

            CombineFeedback(var_type_feedback,
                            CompareOperationFeedback::kNumber);

            // Perform a floating point comparison of {lhs} and {rhs}.
            Branch(Float64Equal(lhs_value, rhs_value), &if_equal, &if_notequal);
          }

          BIND(&if_rhsisnotnumber);
          Goto(&if_not_equivalent_types);
        }
      }

      BIND(&if_lhsisnotnumber);
      {
        // Check if {rhs} is a Smi or a HeapObject.
        Label if_rhsissmi(this), if_rhsisnotsmi(this);
        Branch(TaggedIsSmi(rhs), &if_rhsissmi, &if_rhsisnotsmi);

        BIND(&if_rhsissmi);
        Goto(&if_not_equivalent_types);

        BIND(&if_rhsisnotsmi);
        {
          // Load the instance type of {lhs}.
          TNode<Uint16T> lhs_instance_type = LoadMapInstanceType(lhs_map);

          // Check if {lhs} is a String.
          Label if_lhsisstring(this, Label::kDeferred), if_lhsisnotstring(this);
          Branch(IsStringInstanceType(lhs_instance_type), &if_lhsisstring,
                 &if_lhsisnotstring);

          BIND(&if_lhsisstring);
          {
            // Load the instance type of {rhs}.
            TNode<Uint16T> rhs_instance_type = LoadInstanceType(CAST(rhs));

            // Check if {rhs} is also a String.
            Label if_rhsisstring(this, Label::kDeferred),
                if_rhsisnotstring(this);
            Branch(IsStringInstanceType(rhs_instance_type), &if_rhsisstring,
                   &if_rhsisnotstring);

            BIND(&if_rhsisstring);
            {
              if (var_type_feedback != nullptr) {
                TNode<Smi> lhs_feedback =
                    CollectFeedbackForString(lhs_instance_type);
                TNode<Smi> rhs_feedback =
                    CollectFeedbackForString(rhs_instance_type);
                *var_type_feedback = SmiOr(lhs_feedback, rhs_feedback);
              }
              result = CAST(CallBuiltin(Builtins::kStringEqual,
                                        NoContextConstant(), lhs, rhs));
              Goto(&end);
            }

            BIND(&if_rhsisnotstring);
            Goto(&if_not_equivalent_types);
          }

          BIND(&if_lhsisnotstring);
          {
            // Check if {lhs} is a BigInt.
            Label if_lhsisbigint(this), if_lhsisnotbigint(this);
            Branch(IsBigIntInstanceType(lhs_instance_type), &if_lhsisbigint,
                   &if_lhsisnotbigint);

            BIND(&if_lhsisbigint);
            {
              // Load the instance type of {rhs}.
              TNode<Uint16T> rhs_instance_type = LoadInstanceType(CAST(rhs));

              // Check if {rhs} is also a BigInt.
              Label if_rhsisbigint(this, Label::kDeferred),
                  if_rhsisnotbigint(this);
              Branch(IsBigIntInstanceType(rhs_instance_type), &if_rhsisbigint,
                     &if_rhsisnotbigint);

              BIND(&if_rhsisbigint);
              {
                CombineFeedback(var_type_feedback,
                                CompareOperationFeedback::kBigInt);
                result = CAST(CallRuntime(Runtime::kBigIntEqualToBigInt,
                                          NoContextConstant(), lhs, rhs));
                Goto(&end);
              }

              BIND(&if_rhsisnotbigint);
              Goto(&if_not_equivalent_types);
            }

            BIND(&if_lhsisnotbigint);
            if (var_type_feedback != nullptr) {
              // Load the instance type of {rhs}.
              TNode<Map> rhs_map = LoadMap(CAST(rhs));
              TNode<Uint16T> rhs_instance_type = LoadMapInstanceType(rhs_map);

              Label if_lhsissymbol(this), if_lhsisreceiver(this),
                  if_lhsisoddball(this);
              GotoIf(IsJSReceiverInstanceType(lhs_instance_type),
                     &if_lhsisreceiver);
              GotoIf(IsBooleanMap(lhs_map), &if_not_equivalent_types);
              GotoIf(IsOddballInstanceType(lhs_instance_type),
                     &if_lhsisoddball);
              Branch(IsSymbolInstanceType(lhs_instance_type), &if_lhsissymbol,
                     &if_not_equivalent_types);

              BIND(&if_lhsisreceiver);
              {
                GotoIf(IsBooleanMap(rhs_map), &if_not_equivalent_types);
                OverwriteFeedback(var_type_feedback,
                                  CompareOperationFeedback::kReceiver);
                GotoIf(IsJSReceiverInstanceType(rhs_instance_type),
                       &if_notequal);
                OverwriteFeedback(
                    var_type_feedback,
                    CompareOperationFeedback::kReceiverOrNullOrUndefined);
                GotoIf(IsOddballInstanceType(rhs_instance_type), &if_notequal);
                Goto(&if_not_equivalent_types);
              }

              BIND(&if_lhsisoddball);
              {
                Label if_lhsisboolean(this), if_lhsisnotboolean(this);
                Branch(IsBooleanMap(lhs_map), &if_lhsisboolean,
                       &if_lhsisnotboolean);

                BIND(&if_lhsisboolean);
                {
                  OverwriteFeedback(var_type_feedback,
                                    CompareOperationFeedback::kNumberOrOddball);
                  GotoIf(IsBooleanMap(rhs_map), &if_notequal);
                  Goto(&if_not_equivalent_types);
                }

                BIND(&if_lhsisnotboolean);
                {
                  Label if_rhsisheapnumber(this), if_rhsisnotheapnumber(this);

                  STATIC_ASSERT(LAST_PRIMITIVE_HEAP_OBJECT_TYPE ==
                                ODDBALL_TYPE);
                  GotoIf(Int32LessThan(rhs_instance_type,
                                       Int32Constant(ODDBALL_TYPE)),
                         &if_not_equivalent_types);

                  Branch(IsHeapNumberMap(rhs_map), &if_rhsisheapnumber,
                         &if_rhsisnotheapnumber);

                  BIND(&if_rhsisheapnumber);
                  {
                    OverwriteFeedback(
                        var_type_feedback,
                        CompareOperationFeedback::kNumberOrOddball);
                    Goto(&if_not_equivalent_types);
                  }

                  BIND(&if_rhsisnotheapnumber);
                  {
                    OverwriteFeedback(
                        var_type_feedback,
                        CompareOperationFeedback::kReceiverOrNullOrUndefined);
                    Goto(&if_notequal);
                  }
                }
              }

              BIND(&if_lhsissymbol);
              {
                GotoIfNot(IsSymbolInstanceType(rhs_instance_type),
                          &if_not_equivalent_types);
                OverwriteFeedback(var_type_feedback,
                                  CompareOperationFeedback::kSymbol);
                Goto(&if_notequal);
              }
            } else {
              Goto(&if_notequal);
            }
          }
        }
      }
    }

    BIND(&if_lhsissmi);
    {
      // We already know that {lhs} and {rhs} are not reference equal, and {lhs}
      // is a Smi; so {lhs} and {rhs} can only be strictly equal if {rhs} is a
      // HeapNumber with an equal floating point value.

      // Check if {rhs} is a Smi or a HeapObject.
      Label if_rhsissmi(this), if_rhsisnotsmi(this);
      Branch(TaggedIsSmi(rhs), &if_rhsissmi, &if_rhsisnotsmi);

      BIND(&if_rhsissmi);
      CombineFeedback(var_type_feedback,
                      CompareOperationFeedback::kSignedSmall);
      Goto(&if_notequal);

      BIND(&if_rhsisnotsmi);
      {
        // Load the map of the {rhs}.
        TNode<Map> rhs_map = LoadMap(CAST(rhs));

        // The {rhs} could be a HeapNumber with the same value as {lhs}.
        Label if_rhsisnumber(this), if_rhsisnotnumber(this);
        Branch(IsHeapNumberMap(rhs_map), &if_rhsisnumber, &if_rhsisnotnumber);

        BIND(&if_rhsisnumber);
        {
          // Convert {lhs} and {rhs} to floating point values.
          TNode<Float64T> lhs_value = SmiToFloat64(CAST(lhs));
          TNode<Float64T> rhs_value = LoadHeapNumberValue(CAST(rhs));

          CombineFeedback(var_type_feedback, CompareOperationFeedback::kNumber);

          // Perform a floating point comparison of {lhs} and {rhs}.
          Branch(Float64Equal(lhs_value, rhs_value), &if_equal, &if_notequal);
        }

        BIND(&if_rhsisnotnumber);
        {
          TNode<Uint16T> rhs_instance_type = LoadMapInstanceType(rhs_map);
          GotoIfNot(IsOddballInstanceType(rhs_instance_type),
                    &if_not_equivalent_types);
          OverwriteFeedback(var_type_feedback,
                            CompareOperationFeedback::kNumberOrOddball);
          Goto(&if_notequal);
        }
      }
    }
  }

  BIND(&if_equal);
  {
    result = TrueConstant();
    Goto(&end);
  }

  BIND(&if_not_equivalent_types);
  {
    OverwriteFeedback(var_type_feedback, CompareOperationFeedback::kAny);
    Goto(&if_notequal);
  }

  BIND(&if_notequal);
  {
    result = FalseConstant();
    Goto(&end);
  }

  BIND(&end);
  return result.value();
}

// ECMA#sec-samevalue
// This algorithm differs from the Strict Equality Comparison Algorithm in its
// treatment of signed zeroes and NaNs.
void CodeStubAssembler::BranchIfSameValue(SloppyTNode<Object> lhs,
                                          SloppyTNode<Object> rhs,
                                          Label* if_true, Label* if_false,
                                          SameValueMode mode) {
  TVARIABLE(Float64T, var_lhs_value);
  TVARIABLE(Float64T, var_rhs_value);
  Label do_fcmp(this);

  // Immediately jump to {if_true} if {lhs} == {rhs}, because - unlike
  // StrictEqual - SameValue considers two NaNs to be equal.
  GotoIf(TaggedEqual(lhs, rhs), if_true);

  // Check if the {lhs} is a Smi.
  Label if_lhsissmi(this), if_lhsisheapobject(this);
  Branch(TaggedIsSmi(lhs), &if_lhsissmi, &if_lhsisheapobject);

  BIND(&if_lhsissmi);
  {
    // Since {lhs} is a Smi, the comparison can only yield true
    // iff the {rhs} is a HeapNumber with the same float64 value.
    Branch(TaggedIsSmi(rhs), if_false, [&] {
      GotoIfNot(IsHeapNumber(CAST(rhs)), if_false);
      var_lhs_value = SmiToFloat64(CAST(lhs));
      var_rhs_value = LoadHeapNumberValue(CAST(rhs));
      Goto(&do_fcmp);
    });
  }

  BIND(&if_lhsisheapobject);
  {
    // Check if the {rhs} is a Smi.
    Branch(
        TaggedIsSmi(rhs),
        [&] {
          // Since {rhs} is a Smi, the comparison can only yield true
          // iff the {lhs} is a HeapNumber with the same float64 value.
          GotoIfNot(IsHeapNumber(CAST(lhs)), if_false);
          var_lhs_value = LoadHeapNumberValue(CAST(lhs));
          var_rhs_value = SmiToFloat64(CAST(rhs));
          Goto(&do_fcmp);
        },
        [&] {
          // Now this can only yield true if either both {lhs} and {rhs} are
          // HeapNumbers with the same value, or both are Strings with the
          // same character sequence, or both are BigInts with the same
          // value.
          Label if_lhsisheapnumber(this), if_lhsisstring(this),
              if_lhsisbigint(this);
          const TNode<Map> lhs_map = LoadMap(CAST(lhs));
          GotoIf(IsHeapNumberMap(lhs_map), &if_lhsisheapnumber);
          if (mode != SameValueMode::kNumbersOnly) {
            const TNode<Uint16T> lhs_instance_type =
                LoadMapInstanceType(lhs_map);
            GotoIf(IsStringInstanceType(lhs_instance_type), &if_lhsisstring);
            GotoIf(IsBigIntInstanceType(lhs_instance_type), &if_lhsisbigint);
          }
          Goto(if_false);

          BIND(&if_lhsisheapnumber);
          {
            GotoIfNot(IsHeapNumber(CAST(rhs)), if_false);
            var_lhs_value = LoadHeapNumberValue(CAST(lhs));
            var_rhs_value = LoadHeapNumberValue(CAST(rhs));
            Goto(&do_fcmp);
          }

          if (mode != SameValueMode::kNumbersOnly) {
            BIND(&if_lhsisstring);
            {
              // Now we can only yield true if {rhs} is also a String
              // with the same sequence of characters.
              GotoIfNot(IsString(CAST(rhs)), if_false);
              const TNode<Object> result = CallBuiltin(
                  Builtins::kStringEqual, NoContextConstant(), lhs, rhs);
              Branch(IsTrue(result), if_true, if_false);
            }

            BIND(&if_lhsisbigint);
            {
              GotoIfNot(IsBigInt(CAST(rhs)), if_false);
              const TNode<Object> result = CallRuntime(
                  Runtime::kBigIntEqualToBigInt, NoContextConstant(), lhs, rhs);
              Branch(IsTrue(result), if_true, if_false);
            }
          }
        });
  }

  BIND(&do_fcmp);
  {
    TNode<Float64T> lhs_value = UncheckedCast<Float64T>(var_lhs_value.value());
    TNode<Float64T> rhs_value = UncheckedCast<Float64T>(var_rhs_value.value());
    BranchIfSameNumberValue(lhs_value, rhs_value, if_true, if_false);
  }
}

void CodeStubAssembler::BranchIfSameNumberValue(TNode<Float64T> lhs_value,
                                                TNode<Float64T> rhs_value,
                                                Label* if_true,
                                                Label* if_false) {
  Label if_equal(this), if_notequal(this);
  Branch(Float64Equal(lhs_value, rhs_value), &if_equal, &if_notequal);

  BIND(&if_equal);
  {
    // We still need to handle the case when {lhs} and {rhs} are -0.0 and
    // 0.0 (or vice versa). Compare the high word to
    // distinguish between the two.
    const TNode<Uint32T> lhs_hi_word = Float64ExtractHighWord32(lhs_value);
    const TNode<Uint32T> rhs_hi_word = Float64ExtractHighWord32(rhs_value);

    // If x is +0 and y is -0, return false.
    // If x is -0 and y is +0, return false.
    Branch(Word32Equal(lhs_hi_word, rhs_hi_word), if_true, if_false);
  }

  BIND(&if_notequal);
  {
    // Return true iff both {rhs} and {lhs} are NaN.
    GotoIf(Float64Equal(lhs_value, lhs_value), if_false);
    Branch(Float64Equal(rhs_value, rhs_value), if_false, if_true);
  }
}

TNode<Oddball> CodeStubAssembler::HasProperty(TNode<Context> context,
                                              SloppyTNode<Object> object,
                                              SloppyTNode<Object> key,
                                              HasPropertyLookupMode mode) {
  Label call_runtime(this, Label::kDeferred), return_true(this),
      return_false(this), end(this), if_proxy(this, Label::kDeferred);

  CodeStubAssembler::LookupPropertyInHolder lookup_property_in_holder =
      [this, &return_true](
          TNode<HeapObject> receiver, TNode<HeapObject> holder,
          TNode<Map> holder_map, TNode<Int32T> holder_instance_type,
          TNode<Name> unique_name, Label* next_holder, Label* if_bailout) {
        TryHasOwnProperty(holder, holder_map, holder_instance_type, unique_name,
                          &return_true, next_holder, if_bailout);
      };

  CodeStubAssembler::LookupElementInHolder lookup_element_in_holder =
      [this, &return_true, &return_false](
          TNode<HeapObject> receiver, TNode<HeapObject> holder,
          TNode<Map> holder_map, TNode<Int32T> holder_instance_type,
          TNode<IntPtrT> index, Label* next_holder, Label* if_bailout) {
        TryLookupElement(holder, holder_map, holder_instance_type, index,
                         &return_true, &return_false, next_holder, if_bailout);
      };

  TryPrototypeChainLookup(object, object, key, lookup_property_in_holder,
                          lookup_element_in_holder, &return_false,
                          &call_runtime, &if_proxy);

  TVARIABLE(Oddball, result);

  BIND(&if_proxy);
  {
    TNode<Name> name = CAST(CallBuiltin(Builtins::kToName, context, key));
    switch (mode) {
      case kHasProperty:
        GotoIf(IsPrivateSymbol(name), &return_false);

        result = CAST(
            CallBuiltin(Builtins::kProxyHasProperty, context, object, name));
        Goto(&end);
        break;
      case kForInHasProperty:
        Goto(&call_runtime);
        break;
    }
  }

  BIND(&return_true);
  {
    result = TrueConstant();
    Goto(&end);
  }

  BIND(&return_false);
  {
    result = FalseConstant();
    Goto(&end);
  }

  BIND(&call_runtime);
  {
    Runtime::FunctionId fallback_runtime_function_id;
    switch (mode) {
      case kHasProperty:
        fallback_runtime_function_id = Runtime::kHasProperty;
        break;
      case kForInHasProperty:
        fallback_runtime_function_id = Runtime::kForInHasProperty;
        break;
    }

    result =
        CAST(CallRuntime(fallback_runtime_function_id, context, object, key));
    Goto(&end);
  }

  BIND(&end);
  CSA_ASSERT(this, IsBoolean(result.value()));
  return result.value();
}

void CodeStubAssembler::ForInPrepare(TNode<HeapObject> enumerator,
                                     TNode<UintPtrT> slot,
                                     TNode<HeapObject> maybe_feedback_vector,
                                     TNode<FixedArray>* cache_array_out,
                                     TNode<Smi>* cache_length_out) {
  // Check if we're using an enum cache.
  TVARIABLE(FixedArray, cache_array);
  TVARIABLE(Smi, cache_length);
  Label if_fast(this), if_slow(this, Label::kDeferred), out(this);
  Branch(IsMap(enumerator), &if_fast, &if_slow);

  BIND(&if_fast);
  {
    // Load the enumeration length and cache from the {enumerator}.
    TNode<Map> map_enumerator = CAST(enumerator);
    TNode<WordT> enum_length = LoadMapEnumLength(map_enumerator);
    CSA_ASSERT(this, WordNotEqual(enum_length,
                                  IntPtrConstant(kInvalidEnumCacheSentinel)));
    TNode<DescriptorArray> descriptors = LoadMapDescriptors(map_enumerator);
    TNode<EnumCache> enum_cache = LoadObjectField<EnumCache>(
        descriptors, DescriptorArray::kEnumCacheOffset);
    TNode<FixedArray> enum_keys =
        LoadObjectField<FixedArray>(enum_cache, EnumCache::kKeysOffset);

    // Check if we have enum indices available.
    TNode<FixedArray> enum_indices =
        LoadObjectField<FixedArray>(enum_cache, EnumCache::kIndicesOffset);
    TNode<IntPtrT> enum_indices_length =
        LoadAndUntagFixedArrayBaseLength(enum_indices);
    TNode<Smi> feedback = SelectSmiConstant(
        IntPtrLessThanOrEqual(enum_length, enum_indices_length),
        static_cast<int>(ForInFeedback::kEnumCacheKeysAndIndices),
        static_cast<int>(ForInFeedback::kEnumCacheKeys));
    UpdateFeedback(feedback, maybe_feedback_vector, slot);

    cache_array = enum_keys;
    cache_length = SmiTag(Signed(enum_length));
    Goto(&out);
  }

  BIND(&if_slow);
  {
    // The {enumerator} is a FixedArray with all the keys to iterate.
    TNode<FixedArray> array_enumerator = CAST(enumerator);

    // Record the fact that we hit the for-in slow-path.
    UpdateFeedback(SmiConstant(ForInFeedback::kAny), maybe_feedback_vector,
                   slot);

    cache_array = array_enumerator;
    cache_length = LoadFixedArrayBaseLength(array_enumerator);
    Goto(&out);
  }

  BIND(&out);
  *cache_array_out = cache_array.value();
  *cache_length_out = cache_length.value();
}

TNode<FixedArray> CodeStubAssembler::ForInPrepareForTorque(
    TNode<HeapObject> enumerator, TNode<UintPtrT> slot,
    TNode<HeapObject> maybe_feedback_vector) {
  TNode<FixedArray> cache_array;
  TNode<Smi> cache_length;
  ForInPrepare(enumerator, slot, maybe_feedback_vector, &cache_array,
               &cache_length);

  TNode<FixedArray> result = AllocateUninitializedFixedArray(2);
  StoreFixedArrayElement(result, 0, cache_array);
  StoreFixedArrayElement(result, 1, cache_length);

  return result;
}

TNode<String> CodeStubAssembler::Typeof(SloppyTNode<Object> value) {
  TVARIABLE(String, result_var);

  Label return_number(this, Label::kDeferred), if_oddball(this),
      return_function(this), return_undefined(this), return_object(this),
      return_string(this), return_bigint(this), return_result(this);

  GotoIf(TaggedIsSmi(value), &return_number);

  TNode<HeapObject> value_heap_object = CAST(value);
  TNode<Map> map = LoadMap(value_heap_object);

  GotoIf(IsHeapNumberMap(map), &return_number);

  TNode<Uint16T> instance_type = LoadMapInstanceType(map);

  GotoIf(InstanceTypeEqual(instance_type, ODDBALL_TYPE), &if_oddball);

  TNode<Int32T> callable_or_undetectable_mask =
      Word32And(LoadMapBitField(map),
                Int32Constant(Map::Bits1::IsCallableBit::kMask |
                              Map::Bits1::IsUndetectableBit::kMask));

  GotoIf(Word32Equal(callable_or_undetectable_mask,
                     Int32Constant(Map::Bits1::IsCallableBit::kMask)),
         &return_function);

  GotoIfNot(Word32Equal(callable_or_undetectable_mask, Int32Constant(0)),
            &return_undefined);

  GotoIf(IsJSReceiverInstanceType(instance_type), &return_object);

  GotoIf(IsStringInstanceType(instance_type), &return_string);

  GotoIf(IsBigIntInstanceType(instance_type), &return_bigint);

  CSA_ASSERT(this, InstanceTypeEqual(instance_type, SYMBOL_TYPE));
  result_var = HeapConstant(isolate()->factory()->symbol_string());
  Goto(&return_result);

  BIND(&return_number);
  {
    result_var = HeapConstant(isolate()->factory()->number_string());
    Goto(&return_result);
  }

  BIND(&if_oddball);
  {
    TNode<String> type =
        CAST(LoadObjectField(value_heap_object, Oddball::kTypeOfOffset));
    result_var = type;
    Goto(&return_result);
  }

  BIND(&return_function);
  {
    result_var = HeapConstant(isolate()->factory()->function_string());
    Goto(&return_result);
  }

  BIND(&return_undefined);
  {
    result_var = HeapConstant(isolate()->factory()->undefined_string());
    Goto(&return_result);
  }

  BIND(&return_object);
  {
    result_var = HeapConstant(isolate()->factory()->object_string());
    Goto(&return_result);
  }

  BIND(&return_string);
  {
    result_var = HeapConstant(isolate()->factory()->string_string());
    Goto(&return_result);
  }

  BIND(&return_bigint);
  {
    result_var = HeapConstant(isolate()->factory()->bigint_string());
    Goto(&return_result);
  }

  BIND(&return_result);
  return result_var.value();
}

TNode<HeapObject> CodeStubAssembler::GetSuperConstructor(
    TNode<JSFunction> active_function) {
  TNode<Map> map = LoadMap(active_function);
  return LoadMapPrototype(map);
}

TNode<JSReceiver> CodeStubAssembler::SpeciesConstructor(
    TNode<Context> context, SloppyTNode<Object> object,
    TNode<JSReceiver> default_constructor) {
  Isolate* isolate = this->isolate();
  TVARIABLE(JSReceiver, var_result, default_constructor);

  // 2. Let C be ? Get(O, "constructor").
  TNode<Object> constructor =
      GetProperty(context, object, isolate->factory()->constructor_string());

  // 3. If C is undefined, return defaultConstructor.
  Label out(this);
  GotoIf(IsUndefined(constructor), &out);

  // 4. If Type(C) is not Object, throw a TypeError exception.
  ThrowIfNotJSReceiver(context, constructor,
                       MessageTemplate::kConstructorNotReceiver, "");

  // 5. Let S be ? Get(C, @@species).
  TNode<Object> species =
      GetProperty(context, constructor, isolate->factory()->species_symbol());

  // 6. If S is either undefined or null, return defaultConstructor.
  GotoIf(IsNullOrUndefined(species), &out);

  // 7. If IsConstructor(S) is true, return S.
  Label throw_error(this);
  GotoIf(TaggedIsSmi(species), &throw_error);
  GotoIfNot(IsConstructorMap(LoadMap(CAST(species))), &throw_error);
  var_result = CAST(species);
  Goto(&out);

  // 8. Throw a TypeError exception.
  BIND(&throw_error);
  ThrowTypeError(context, MessageTemplate::kSpeciesNotConstructor);

  BIND(&out);
  return var_result.value();
}

TNode<Oddball> CodeStubAssembler::InstanceOf(TNode<Object> object,
                                             TNode<Object> callable,
                                             TNode<Context> context) {
  TVARIABLE(Oddball, var_result);
  Label if_notcallable(this, Label::kDeferred),
      if_notreceiver(this, Label::kDeferred), if_otherhandler(this),
      if_nohandler(this, Label::kDeferred), return_true(this),
      return_false(this), return_result(this, &var_result);

  // Ensure that the {callable} is actually a JSReceiver.
  GotoIf(TaggedIsSmi(callable), &if_notreceiver);
  GotoIfNot(IsJSReceiver(CAST(callable)), &if_notreceiver);

  // Load the @@hasInstance property from {callable}.
  TNode<Object> inst_of_handler =
      GetProperty(context, callable, HasInstanceSymbolConstant());

  // Optimize for the likely case where {inst_of_handler} is the builtin
  // Function.prototype[@@hasInstance] method, and emit a direct call in
  // that case without any additional checking.
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Object> function_has_instance =
      LoadContextElement(native_context, Context::FUNCTION_HAS_INSTANCE_INDEX);
  GotoIfNot(TaggedEqual(inst_of_handler, function_has_instance),
            &if_otherhandler);
  {
    // Call to Function.prototype[@@hasInstance] directly.
    Callable builtin(BUILTIN_CODE(isolate(), FunctionPrototypeHasInstance),
                     CallTrampolineDescriptor{});
    var_result =
        CAST(CallJS(builtin, context, inst_of_handler, callable, object));
    Goto(&return_result);
  }

  BIND(&if_otherhandler);
  {
    // Check if there's actually an {inst_of_handler}.
    GotoIf(IsNull(inst_of_handler), &if_nohandler);
    GotoIf(IsUndefined(inst_of_handler), &if_nohandler);

    // Call the {inst_of_handler} for {callable} and {object}.
    TNode<Object> result = Call(context, inst_of_handler, callable, object);

    // Convert the {result} to a Boolean.
    BranchIfToBooleanIsTrue(result, &return_true, &return_false);
  }

  BIND(&if_nohandler);
  {
    // Ensure that the {callable} is actually Callable.
    GotoIfNot(IsCallable(CAST(callable)), &if_notcallable);

    // Use the OrdinaryHasInstance algorithm.
    var_result = CAST(
        CallBuiltin(Builtins::kOrdinaryHasInstance, context, callable, object));
    Goto(&return_result);
  }

  BIND(&if_notcallable);
  { ThrowTypeError(context, MessageTemplate::kNonCallableInInstanceOfCheck); }

  BIND(&if_notreceiver);
  { ThrowTypeError(context, MessageTemplate::kNonObjectInInstanceOfCheck); }

  BIND(&return_true);
  var_result = TrueConstant();
  Goto(&return_result);

  BIND(&return_false);
  var_result = FalseConstant();
  Goto(&return_result);

  BIND(&return_result);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::NumberInc(TNode<Number> value) {
  TVARIABLE(Number, var_result);
  TVARIABLE(Float64T, var_finc_value);
  Label if_issmi(this), if_isnotsmi(this), do_finc(this), end(this);
  Branch(TaggedIsSmi(value), &if_issmi, &if_isnotsmi);

  BIND(&if_issmi);
  {
    Label if_overflow(this);
    TNode<Smi> smi_value = CAST(value);
    TNode<Smi> one = SmiConstant(1);
    var_result = TrySmiAdd(smi_value, one, &if_overflow);
    Goto(&end);

    BIND(&if_overflow);
    {
      var_finc_value = SmiToFloat64(smi_value);
      Goto(&do_finc);
    }
  }

  BIND(&if_isnotsmi);
  {
    TNode<HeapNumber> heap_number_value = CAST(value);

    // Load the HeapNumber value.
    var_finc_value = LoadHeapNumberValue(heap_number_value);
    Goto(&do_finc);
  }

  BIND(&do_finc);
  {
    TNode<Float64T> finc_value = var_finc_value.value();
    TNode<Float64T> one = Float64Constant(1.0);
    TNode<Float64T> finc_result = Float64Add(finc_value, one);
    var_result = AllocateHeapNumberWithValue(finc_result);
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::NumberDec(TNode<Number> value) {
  TVARIABLE(Number, var_result);
  TVARIABLE(Float64T, var_fdec_value);
  Label if_issmi(this), if_isnotsmi(this), do_fdec(this), end(this);
  Branch(TaggedIsSmi(value), &if_issmi, &if_isnotsmi);

  BIND(&if_issmi);
  {
    TNode<Smi> smi_value = CAST(value);
    TNode<Smi> one = SmiConstant(1);
    Label if_overflow(this);
    var_result = TrySmiSub(smi_value, one, &if_overflow);
    Goto(&end);

    BIND(&if_overflow);
    {
      var_fdec_value = SmiToFloat64(smi_value);
      Goto(&do_fdec);
    }
  }

  BIND(&if_isnotsmi);
  {
    TNode<HeapNumber> heap_number_value = CAST(value);

    // Load the HeapNumber value.
    var_fdec_value = LoadHeapNumberValue(heap_number_value);
    Goto(&do_fdec);
  }

  BIND(&do_fdec);
  {
    TNode<Float64T> fdec_value = var_fdec_value.value();
    TNode<Float64T> minus_one = Float64Constant(-1.0);
    TNode<Float64T> fdec_result = Float64Add(fdec_value, minus_one);
    var_result = AllocateHeapNumberWithValue(fdec_result);
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::NumberAdd(TNode<Number> a, TNode<Number> b) {
  TVARIABLE(Number, var_result);
  Label float_add(this, Label::kDeferred), end(this);
  GotoIf(TaggedIsNotSmi(a), &float_add);
  GotoIf(TaggedIsNotSmi(b), &float_add);

  // Try fast Smi addition first.
  var_result = TrySmiAdd(CAST(a), CAST(b), &float_add);
  Goto(&end);

  BIND(&float_add);
  {
    var_result = ChangeFloat64ToTagged(
        Float64Add(ChangeNumberToFloat64(a), ChangeNumberToFloat64(b)));
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

TNode<Number> CodeStubAssembler::NumberSub(TNode<Number> a, TNode<Number> b) {
  TVARIABLE(Number, var_result);
  Label float_sub(this, Label::kDeferred), end(this);
  GotoIf(TaggedIsNotSmi(a), &float_sub);
  GotoIf(TaggedIsNotSmi(b), &float_sub);

  // Try fast Smi subtraction first.
  var_result = TrySmiSub(CAST(a), CAST(b), &float_sub);
  Goto(&end);

  BIND(&float_sub);
  {
    var_result = ChangeFloat64ToTagged(
        Float64Sub(ChangeNumberToFloat64(a), ChangeNumberToFloat64(b)));
    Goto(&end);
  }

  BIND(&end);
  return var_result.value();
}

void CodeStubAssembler::GotoIfNotNumber(TNode<Object> input,
                                        Label* is_not_number) {
  Label is_number(this);
  GotoIf(TaggedIsSmi(input), &is_number);
  Branch(IsHeapNumber(CAST(input)), &is_number, is_not_number);
  BIND(&is_number);
}

void CodeStubAssembler::GotoIfNumber(TNode<Object> input, Label* is_number) {
  GotoIf(TaggedIsSmi(input), is_number);
  GotoIf(IsHeapNumber(CAST(input)), is_number);
}

TNode<Number> CodeStubAssembler::BitwiseOp(TNode<Word32T> left32,
                                           TNode<Word32T> right32,
                                           Operation bitwise_op) {
  switch (bitwise_op) {
    case Operation::kBitwiseAnd:
      return ChangeInt32ToTagged(Signed(Word32And(left32, right32)));
    case Operation::kBitwiseOr:
      return ChangeInt32ToTagged(Signed(Word32Or(left32, right32)));
    case Operation::kBitwiseXor:
      return ChangeInt32ToTagged(Signed(Word32Xor(left32, right32)));
    case Operation::kShiftLeft:
      if (!Word32ShiftIsSafe()) {
        right32 = Word32And(right32, Int32Constant(0x1F));
      }
      return ChangeInt32ToTagged(Signed(Word32Shl(left32, right32)));
    case Operation::kShiftRight:
      if (!Word32ShiftIsSafe()) {
        right32 = Word32And(right32, Int32Constant(0x1F));
      }
      return ChangeInt32ToTagged(Signed(Word32Sar(left32, right32)));
    case Operation::kShiftRightLogical:
      if (!Word32ShiftIsSafe()) {
        right32 = Word32And(right32, Int32Constant(0x1F));
      }
      return ChangeUint32ToTagged(Unsigned(Word32Shr(left32, right32)));
    default:
      break;
  }
  UNREACHABLE();
}

TNode<JSObject> CodeStubAssembler::AllocateJSIteratorResult(
    TNode<Context> context, SloppyTNode<Object> value,
    SloppyTNode<Oddball> done) {
  CSA_ASSERT(this, IsBoolean(done));
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Map> map = CAST(
      LoadContextElement(native_context, Context::ITERATOR_RESULT_MAP_INDEX));
  TNode<HeapObject> result = Allocate(JSIteratorResult::kSize);
  StoreMapNoWriteBarrier(result, map);
  StoreObjectFieldRoot(result, JSIteratorResult::kPropertiesOrHashOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldRoot(result, JSIteratorResult::kElementsOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldNoWriteBarrier(result, JSIteratorResult::kValueOffset, value);
  StoreObjectFieldNoWriteBarrier(result, JSIteratorResult::kDoneOffset, done);
  return CAST(result);
}

TNode<JSObject> CodeStubAssembler::AllocateJSIteratorResultForEntry(
    TNode<Context> context, TNode<Object> key, SloppyTNode<Object> value) {
  TNode<NativeContext> native_context = LoadNativeContext(context);
  TNode<Smi> length = SmiConstant(2);
  int const elements_size = FixedArray::SizeFor(2);
  TNode<FixedArray> elements = UncheckedCast<FixedArray>(
      Allocate(elements_size + JSArray::kHeaderSize + JSIteratorResult::kSize));
  StoreObjectFieldRoot(elements, FixedArray::kMapOffset,
                       RootIndex::kFixedArrayMap);
  StoreObjectFieldNoWriteBarrier(elements, FixedArray::kLengthOffset, length);
  StoreFixedArrayElement(elements, 0, key);
  StoreFixedArrayElement(elements, 1, value);
  TNode<Map> array_map = CAST(LoadContextElement(
      native_context, Context::JS_ARRAY_PACKED_ELEMENTS_MAP_INDEX));
  TNode<HeapObject> array = InnerAllocate(elements, elements_size);
  StoreMapNoWriteBarrier(array, array_map);
  StoreObjectFieldRoot(array, JSArray::kPropertiesOrHashOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldNoWriteBarrier(array, JSArray::kElementsOffset, elements);
  StoreObjectFieldNoWriteBarrier(array, JSArray::kLengthOffset, length);
  TNode<Map> iterator_map = CAST(
      LoadContextElement(native_context, Context::ITERATOR_RESULT_MAP_INDEX));
  TNode<HeapObject> result = InnerAllocate(array, JSArray::kHeaderSize);
  StoreMapNoWriteBarrier(result, iterator_map);
  StoreObjectFieldRoot(result, JSIteratorResult::kPropertiesOrHashOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldRoot(result, JSIteratorResult::kElementsOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldNoWriteBarrier(result, JSIteratorResult::kValueOffset, array);
  StoreObjectFieldRoot(result, JSIteratorResult::kDoneOffset,
                       RootIndex::kFalseValue);
  return CAST(result);
}

TNode<JSReceiver> CodeStubAssembler::ArraySpeciesCreate(TNode<Context> context,
                                                        TNode<Object> o,
                                                        TNode<Number> len) {
  TNode<JSReceiver> constructor =
      CAST(CallRuntime(Runtime::kArraySpeciesConstructor, context, o));
  return Construct(context, constructor, len);
}

void CodeStubAssembler::ThrowIfArrayBufferIsDetached(
    TNode<Context> context, TNode<JSArrayBuffer> array_buffer,
    const char* method_name) {
  Label if_detached(this, Label::kDeferred), if_not_detached(this);
  Branch(IsDetachedBuffer(array_buffer), &if_detached, &if_not_detached);
  BIND(&if_detached);
  ThrowTypeError(context, MessageTemplate::kDetachedOperation, method_name);
  BIND(&if_not_detached);
}

void CodeStubAssembler::ThrowIfArrayBufferViewBufferIsDetached(
    TNode<Context> context, TNode<JSArrayBufferView> array_buffer_view,
    const char* method_name) {
  TNode<JSArrayBuffer> buffer = LoadJSArrayBufferViewBuffer(array_buffer_view);
  ThrowIfArrayBufferIsDetached(context, buffer, method_name);
}

TNode<RawPtrT> CodeStubAssembler::LoadJSArrayBufferBackingStorePtr(
    TNode<JSArrayBuffer> array_buffer) {
  return LoadExternalPointerFromObject(array_buffer,
                                       JSArrayBuffer::kBackingStoreOffset,
                                       kArrayBufferBackingStoreTag);
}

TNode<JSArrayBuffer> CodeStubAssembler::LoadJSArrayBufferViewBuffer(
    TNode<JSArrayBufferView> array_buffer_view) {
  return LoadObjectField<JSArrayBuffer>(array_buffer_view,
                                        JSArrayBufferView::kBufferOffset);
}

TNode<UintPtrT> CodeStubAssembler::LoadJSArrayBufferViewByteLength(
    TNode<JSArrayBufferView> array_buffer_view) {
  return LoadObjectField<UintPtrT>(array_buffer_view,
                                   JSArrayBufferView::kByteLengthOffset);
}

TNode<UintPtrT> CodeStubAssembler::LoadJSArrayBufferViewByteOffset(
    TNode<JSArrayBufferView> array_buffer_view) {
  return LoadObjectField<UintPtrT>(array_buffer_view,
                                   JSArrayBufferView::kByteOffsetOffset);
}

TNode<UintPtrT> CodeStubAssembler::LoadJSTypedArrayLength(
    TNode<JSTypedArray> typed_array) {
  return LoadObjectField<UintPtrT>(typed_array, JSTypedArray::kLengthOffset);
}

TNode<JSArrayBuffer> CodeStubAssembler::GetTypedArrayBuffer(
    TNode<Context> context, TNode<JSTypedArray> array) {
  Label call_runtime(this), done(this);
  TVARIABLE(Object, var_result);

  TNode<JSArrayBuffer> buffer = LoadJSArrayBufferViewBuffer(array);
  GotoIf(IsDetachedBuffer(buffer), &call_runtime);
  TNode<RawPtrT> backing_store = LoadJSArrayBufferBackingStorePtr(buffer);
  GotoIf(WordEqual(backing_store, IntPtrConstant(0)), &call_runtime);
  var_result = buffer;
  Goto(&done);

  BIND(&call_runtime);
  {
    var_result = CallRuntime(Runtime::kTypedArrayGetBuffer, context, array);
    Goto(&done);
  }

  BIND(&done);
  return CAST(var_result.value());
}

CodeStubArguments::CodeStubArguments(CodeStubAssembler* assembler,
                                     TNode<IntPtrT> argc, TNode<RawPtrT> fp)
    : assembler_(assembler),
      argc_(argc),
      base_(),
      fp_(fp != nullptr ? fp : assembler_->LoadFramePointer()) {
  TNode<IntPtrT> offset = assembler_->IntPtrConstant(
      (StandardFrameConstants::kFixedSlotCountAboveFp + 1) *
      kSystemPointerSize);
  // base_ points to the first argument, not the receiver
  // whether present or not.
  base_ = assembler_->RawPtrAdd(fp_, offset);
}

TNode<Object> CodeStubArguments::GetReceiver() const {
  intptr_t offset = -kSystemPointerSize;
  return assembler_->LoadFullTagged(base_, assembler_->IntPtrConstant(offset));
}

void CodeStubArguments::SetReceiver(TNode<Object> object) const {
  intptr_t offset = -kSystemPointerSize;
  assembler_->StoreFullTaggedNoWriteBarrier(
      base_, assembler_->IntPtrConstant(offset), object);
}

TNode<RawPtrT> CodeStubArguments::AtIndexPtr(TNode<IntPtrT> index) const {
  TNode<IntPtrT> offset =
      assembler_->ElementOffsetFromIndex(index, SYSTEM_POINTER_ELEMENTS, 0);
  return assembler_->RawPtrAdd(base_, offset);
}

TNode<Object> CodeStubArguments::AtIndex(TNode<IntPtrT> index) const {
  CSA_ASSERT(assembler_, assembler_->UintPtrOrSmiLessThan(index, GetLength()));
  return assembler_->UncheckedCast<Object>(
      assembler_->LoadFullTagged(AtIndexPtr(index)));
}

TNode<Object> CodeStubArguments::AtIndex(int index) const {
  return AtIndex(assembler_->IntPtrConstant(index));
}

TNode<Object> CodeStubArguments::GetOptionalArgumentValue(
    TNode<IntPtrT> index, TNode<Object> default_value) {
  CodeStubAssembler::TVariable<Object> result(assembler_);
  CodeStubAssembler::Label argument_missing(assembler_),
      argument_done(assembler_, &result);

  assembler_->GotoIf(assembler_->UintPtrGreaterThanOrEqual(index, argc_),
                     &argument_missing);
  result = AtIndex(index);
  assembler_->Goto(&argument_done);

  assembler_->BIND(&argument_missing);
  result = default_value;
  assembler_->Goto(&argument_done);

  assembler_->BIND(&argument_done);
  return result.value();
}

void CodeStubArguments::ForEach(
    const CodeStubAssembler::VariableList& vars,
    const CodeStubArguments::ForEachBodyFunction& body, TNode<IntPtrT> first,
    TNode<IntPtrT> last) const {
  assembler_->Comment("CodeStubArguments::ForEach");
  if (first == nullptr) {
    first = assembler_->IntPtrConstant(0);
  }
  if (last == nullptr) {
    last = argc_;
  }
  TNode<RawPtrT> start = AtIndexPtr(first);
  TNode<RawPtrT> end = AtIndexPtr(last);
  const int increment = kSystemPointerSize;
  assembler_->BuildFastLoop<RawPtrT>(
      vars, start, end,
      [&](TNode<RawPtrT> current) {
        TNode<Object> arg = assembler_->LoadFullTagged(current);
        body(arg);
      },
      increment, CodeStubAssembler::IndexAdvanceMode::kPost);
}

void CodeStubArguments::PopAndReturn(TNode<Object> value) {
  TNode<IntPtrT> pop_count =
      assembler_->IntPtrAdd(argc_, assembler_->IntPtrConstant(1));
  assembler_->PopAndReturn(pop_count, value);
}

TNode<BoolT> CodeStubAssembler::IsFastElementsKind(
    TNode<Int32T> elements_kind) {
  STATIC_ASSERT(FIRST_ELEMENTS_KIND == FIRST_FAST_ELEMENTS_KIND);
  return Uint32LessThanOrEqual(elements_kind,
                               Int32Constant(LAST_FAST_ELEMENTS_KIND));
}

TNode<BoolT> CodeStubAssembler::IsFastOrNonExtensibleOrSealedElementsKind(
    TNode<Int32T> elements_kind) {
  STATIC_ASSERT(FIRST_ELEMENTS_KIND == FIRST_FAST_ELEMENTS_KIND);
  STATIC_ASSERT(LAST_FAST_ELEMENTS_KIND + 1 == PACKED_NONEXTENSIBLE_ELEMENTS);
  STATIC_ASSERT(PACKED_NONEXTENSIBLE_ELEMENTS + 1 ==
                HOLEY_NONEXTENSIBLE_ELEMENTS);
  STATIC_ASSERT(HOLEY_NONEXTENSIBLE_ELEMENTS + 1 == PACKED_SEALED_ELEMENTS);
  STATIC_ASSERT(PACKED_SEALED_ELEMENTS + 1 == HOLEY_SEALED_ELEMENTS);
  return Uint32LessThanOrEqual(elements_kind,
                               Int32Constant(HOLEY_SEALED_ELEMENTS));
}

TNode<BoolT> CodeStubAssembler::IsDoubleElementsKind(
    TNode<Int32T> elements_kind) {
  STATIC_ASSERT(FIRST_ELEMENTS_KIND == FIRST_FAST_ELEMENTS_KIND);
  STATIC_ASSERT((PACKED_DOUBLE_ELEMENTS & 1) == 0);
  STATIC_ASSERT(PACKED_DOUBLE_ELEMENTS + 1 == HOLEY_DOUBLE_ELEMENTS);
  return Word32Equal(Word32Shr(elements_kind, Int32Constant(1)),
                     Int32Constant(PACKED_DOUBLE_ELEMENTS / 2));
}

TNode<BoolT> CodeStubAssembler::IsFastSmiOrTaggedElementsKind(
    TNode<Int32T> elements_kind) {
  STATIC_ASSERT(FIRST_ELEMENTS_KIND == FIRST_FAST_ELEMENTS_KIND);
  STATIC_ASSERT(PACKED_DOUBLE_ELEMENTS > TERMINAL_FAST_ELEMENTS_KIND);
  STATIC_ASSERT(HOLEY_DOUBLE_ELEMENTS > TERMINAL_FAST_ELEMENTS_KIND);
  return Uint32LessThanOrEqual(elements_kind,
                               Int32Constant(TERMINAL_FAST_ELEMENTS_KIND));
}

TNode<BoolT> CodeStubAssembler::IsFastSmiElementsKind(
    SloppyTNode<Int32T> elements_kind) {
  return Uint32LessThanOrEqual(elements_kind,
                               Int32Constant(HOLEY_SMI_ELEMENTS));
}

TNode<BoolT> CodeStubAssembler::IsHoleyFastElementsKind(
    TNode<Int32T> elements_kind) {
  CSA_ASSERT(this, IsFastElementsKind(elements_kind));

  STATIC_ASSERT(HOLEY_SMI_ELEMENTS == (PACKED_SMI_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_ELEMENTS == (PACKED_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_DOUBLE_ELEMENTS == (PACKED_DOUBLE_ELEMENTS | 1));
  return IsSetWord32(elements_kind, 1);
}

TNode<BoolT> CodeStubAssembler::IsHoleyFastElementsKindForRead(
    TNode<Int32T> elements_kind) {
  CSA_ASSERT(this, Uint32LessThanOrEqual(
                       elements_kind,
                       Int32Constant(LAST_ANY_NONEXTENSIBLE_ELEMENTS_KIND)));

  STATIC_ASSERT(HOLEY_SMI_ELEMENTS == (PACKED_SMI_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_ELEMENTS == (PACKED_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_DOUBLE_ELEMENTS == (PACKED_DOUBLE_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_NONEXTENSIBLE_ELEMENTS ==
                (PACKED_NONEXTENSIBLE_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_SEALED_ELEMENTS == (PACKED_SEALED_ELEMENTS | 1));
  STATIC_ASSERT(HOLEY_FROZEN_ELEMENTS == (PACKED_FROZEN_ELEMENTS | 1));
  return IsSetWord32(elements_kind, 1);
}

TNode<BoolT> CodeStubAssembler::IsElementsKindGreaterThan(
    TNode<Int32T> target_kind, ElementsKind reference_kind) {
  return Int32GreaterThan(target_kind, Int32Constant(reference_kind));
}

TNode<BoolT> CodeStubAssembler::IsElementsKindLessThanOrEqual(
    TNode<Int32T> target_kind, ElementsKind reference_kind) {
  return Int32LessThanOrEqual(target_kind, Int32Constant(reference_kind));
}

TNode<BoolT> CodeStubAssembler::IsDebugActive() {
  TNode<Uint8T> is_debug_active = Load<Uint8T>(
      ExternalConstant(ExternalReference::debug_is_active_address(isolate())));
  return Word32NotEqual(is_debug_active, Int32Constant(0));
}

TNode<BoolT> CodeStubAssembler::IsPromiseHookEnabled() {
  const TNode<RawPtrT> promise_hook = Load<RawPtrT>(
      ExternalConstant(ExternalReference::promise_hook_address(isolate())));
  return WordNotEqual(promise_hook, IntPtrConstant(0));
}

TNode<BoolT> CodeStubAssembler::HasAsyncEventDelegate() {
  const TNode<RawPtrT> async_event_delegate = Load<RawPtrT>(ExternalConstant(
      ExternalReference::async_event_delegate_address(isolate())));
  return WordNotEqual(async_event_delegate, IntPtrConstant(0));
}

TNode<BoolT> CodeStubAssembler::IsPromiseHookEnabledOrHasAsyncEventDelegate() {
  const TNode<Uint8T> promise_hook_or_async_event_delegate =
      Load<Uint8T>(ExternalConstant(
          ExternalReference::promise_hook_or_async_event_delegate_address(
              isolate())));
  return Word32NotEqual(promise_hook_or_async_event_delegate, Int32Constant(0));
}

TNode<BoolT> CodeStubAssembler::
    IsPromiseHookEnabledOrDebugIsActiveOrHasAsyncEventDelegate() {
  const TNode<Uint8T> promise_hook_or_debug_is_active_or_async_event_delegate =
      Load<Uint8T>(ExternalConstant(
          ExternalReference::
              promise_hook_or_debug_is_active_or_async_event_delegate_address(
                  isolate())));
  return Word32NotEqual(promise_hook_or_debug_is_active_or_async_event_delegate,
                        Int32Constant(0));
}

TNode<Code> CodeStubAssembler::LoadBuiltin(TNode<Smi> builtin_id) {
  CSA_ASSERT(this, SmiBelow(builtin_id, SmiConstant(Builtins::builtin_count)));

  TNode<IntPtrT> offset =
      ElementOffsetFromIndex(SmiToBInt(builtin_id), SYSTEM_POINTER_ELEMENTS);

  return CAST(BitcastWordToTagged(
      Load(MachineType::Pointer(),
           ExternalConstant(ExternalReference::builtins_address(isolate())),
           offset)));
}

TNode<Code> CodeStubAssembler::GetSharedFunctionInfoCode(
    TNode<SharedFunctionInfo> shared_info, Label* if_compile_lazy) {
  TNode<Object> sfi_data =
      LoadObjectField(shared_info, SharedFunctionInfo::kFunctionDataOffset);

  TVARIABLE(Code, sfi_code);

  Label done(this);
  Label check_instance_type(this);

  // IsSmi: Is builtin
  GotoIf(TaggedIsNotSmi(sfi_data), &check_instance_type);
  if (if_compile_lazy) {
    GotoIf(SmiEqual(CAST(sfi_data), SmiConstant(Builtins::kCompileLazy)),
           if_compile_lazy);
  }
  sfi_code = LoadBuiltin(CAST(sfi_data));
  Goto(&done);

  // Switch on data's instance type.
  BIND(&check_instance_type);
  TNode<Uint16T> data_type = LoadInstanceType(CAST(sfi_data));

  int32_t case_values[] = {BYTECODE_ARRAY_TYPE,
                           WASM_EXPORTED_FUNCTION_DATA_TYPE,
                           ASM_WASM_DATA_TYPE,
                           UNCOMPILED_DATA_WITHOUT_PREPARSE_DATA_TYPE,
                           UNCOMPILED_DATA_WITH_PREPARSE_DATA_TYPE,
                           FUNCTION_TEMPLATE_INFO_TYPE,
                           WASM_JS_FUNCTION_DATA_TYPE,
                           WASM_CAPI_FUNCTION_DATA_TYPE};
  Label check_is_bytecode_array(this);
  Label check_is_exported_function_data(this);
  Label check_is_asm_wasm_data(this);
  Label check_is_uncompiled_data_without_preparse_data(this);
  Label check_is_uncompiled_data_with_preparse_data(this);
  Label check_is_function_template_info(this);
  Label check_is_interpreter_data(this);
  Label check_is_wasm_js_function_data(this);
  Label check_is_wasm_capi_function_data(this);
  Label* case_labels[] = {&check_is_bytecode_array,
                          &check_is_exported_function_data,
                          &check_is_asm_wasm_data,
                          &check_is_uncompiled_data_without_preparse_data,
                          &check_is_uncompiled_data_with_preparse_data,
                          &check_is_function_template_info,
                          &check_is_wasm_js_function_data,
                          &check_is_wasm_capi_function_data};
  STATIC_ASSERT(arraysize(case_values) == arraysize(case_labels));
  Switch(data_type, &check_is_interpreter_data, case_values, case_labels,
         arraysize(case_labels));

  // IsBytecodeArray: Interpret bytecode
  BIND(&check_is_bytecode_array);
  sfi_code = HeapConstant(BUILTIN_CODE(isolate(), InterpreterEntryTrampoline));
  Goto(&done);

  // IsWasmExportedFunctionData: Use the wrapper code
  BIND(&check_is_exported_function_data);
  sfi_code = CAST(LoadObjectField(
      CAST(sfi_data), WasmExportedFunctionData::kWrapperCodeOffset));
  Goto(&done);

  // IsAsmWasmData: Instantiate using AsmWasmData
  BIND(&check_is_asm_wasm_data);
  sfi_code = HeapConstant(BUILTIN_CODE(isolate(), InstantiateAsmJs));
  Goto(&done);

  // IsUncompiledDataWithPreparseData | IsUncompiledDataWithoutPreparseData:
  // Compile lazy
  BIND(&check_is_uncompiled_data_with_preparse_data);
  Goto(&check_is_uncompiled_data_without_preparse_data);
  BIND(&check_is_uncompiled_data_without_preparse_data);
  sfi_code = HeapConstant(BUILTIN_CODE(isolate(), CompileLazy));
  Goto(if_compile_lazy ? if_compile_lazy : &done);

  // IsFunctionTemplateInfo: API call
  BIND(&check_is_function_template_info);
  sfi_code = HeapConstant(BUILTIN_CODE(isolate(), HandleApiCall));
  Goto(&done);

  // IsInterpreterData: Interpret bytecode
  BIND(&check_is_interpreter_data);
  // This is the default branch, so assert that we have the expected data type.
  CSA_ASSERT(this,
             Word32Equal(data_type, Int32Constant(INTERPRETER_DATA_TYPE)));
  sfi_code = CAST(LoadObjectField(
      CAST(sfi_data), InterpreterData::kInterpreterTrampolineOffset));
  Goto(&done);

  // IsWasmJSFunctionData: Use the wrapper code.
  BIND(&check_is_wasm_js_function_data);
  sfi_code = CAST(
      LoadObjectField(CAST(sfi_data), WasmJSFunctionData::kWrapperCodeOffset));
  Goto(&done);

  // IsWasmCapiFunctionData: Use the wrapper code.
  BIND(&check_is_wasm_capi_function_data);
  sfi_code = CAST(LoadObjectField(CAST(sfi_data),
                                  WasmCapiFunctionData::kWrapperCodeOffset));
  Goto(&done);

  BIND(&done);
  return sfi_code.value();
}

TNode<JSFunction> CodeStubAssembler::AllocateFunctionWithMapAndContext(
    TNode<Map> map, TNode<SharedFunctionInfo> shared_info,
    TNode<Context> context) {
  const TNode<Code> code = GetSharedFunctionInfoCode(shared_info);

  // TODO(ishell): All the callers of this function pass map loaded from
  // Context::STRICT_FUNCTION_WITHOUT_PROTOTYPE_MAP_INDEX. So we can remove
  // map parameter.
  CSA_ASSERT(this, Word32BinaryNot(IsConstructorMap(map)));
  CSA_ASSERT(this, Word32BinaryNot(IsFunctionWithPrototypeSlotMap(map)));
  const TNode<HeapObject> fun = Allocate(JSFunction::kSizeWithoutPrototype);
  STATIC_ASSERT(JSFunction::kSizeWithoutPrototype == 7 * kTaggedSize);
  StoreMapNoWriteBarrier(fun, map);
  StoreObjectFieldRoot(fun, JSObject::kPropertiesOrHashOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldRoot(fun, JSObject::kElementsOffset,
                       RootIndex::kEmptyFixedArray);
  StoreObjectFieldRoot(fun, JSFunction::kFeedbackCellOffset,
                       RootIndex::kManyClosuresCell);
  StoreObjectFieldNoWriteBarrier(fun, JSFunction::kSharedFunctionInfoOffset,
                                 shared_info);
  StoreObjectFieldNoWriteBarrier(fun, JSFunction::kContextOffset, context);
  StoreObjectFieldNoWriteBarrier(fun, JSFunction::kCodeOffset, code);
  return CAST(fun);
}

void CodeStubAssembler::CheckPrototypeEnumCache(TNode<JSReceiver> receiver,
                                                TNode<Map> receiver_map,
                                                Label* if_fast,
                                                Label* if_slow) {
  TVARIABLE(JSReceiver, var_object, receiver);
  TVARIABLE(Map, object_map, receiver_map);

  Label loop(this, {&var_object, &object_map}), done_loop(this);
  Goto(&loop);
  BIND(&loop);
  {
    // Check that there are no elements on the current {var_object}.
    Label if_no_elements(this);

    // The following relies on the elements only aliasing with JSProxy::target,
    // which is a JavaScript value and hence cannot be confused with an elements
    // backing store.
    STATIC_ASSERT(static_cast<int>(JSObject::kElementsOffset) ==
                  static_cast<int>(JSProxy::kTargetOffset));
    TNode<Object> object_elements =
        LoadObjectField(var_object.value(), JSObject::kElementsOffset);
    GotoIf(IsEmptyFixedArray(object_elements), &if_no_elements);
    GotoIf(IsEmptySlowElementDictionary(object_elements), &if_no_elements);

    // It might still be an empty JSArray.
    GotoIfNot(IsJSArrayMap(object_map.value()), if_slow);
    TNode<Number> object_length = LoadJSArrayLength(CAST(var_object.value()));
    Branch(TaggedEqual(object_length, SmiConstant(0)), &if_no_elements,
           if_slow);

    // Continue with {var_object}'s prototype.
    BIND(&if_no_elements);
    TNode<HeapObject> object = LoadMapPrototype(object_map.value());
    GotoIf(IsNull(object), if_fast);

    // For all {object}s but the {receiver}, check that the cache is empty.
    var_object = CAST(object);
    object_map = LoadMap(object);
    TNode<WordT> object_enum_length = LoadMapEnumLength(object_map.value());
    Branch(WordEqual(object_enum_length, IntPtrConstant(0)), &loop, if_slow);
  }
}

TNode<Map> CodeStubAssembler::CheckEnumCache(TNode<JSReceiver> receiver,
                                             Label* if_empty,
                                             Label* if_runtime) {
  Label if_fast(this), if_cache(this), if_no_cache(this, Label::kDeferred);
  TNode<Map> receiver_map = LoadMap(receiver);

  // Check if the enum length field of the {receiver} is properly initialized,
  // indicating that there is an enum cache.
  TNode<WordT> receiver_enum_length = LoadMapEnumLength(receiver_map);
  Branch(WordEqual(receiver_enum_length,
                   IntPtrConstant(kInvalidEnumCacheSentinel)),
         &if_no_cache, &if_cache);

  BIND(&if_no_cache);
  {
    // Avoid runtime-call for empty dictionary receivers.
    GotoIfNot(IsDictionaryMap(receiver_map), if_runtime);
    TNode<HashTableBase> properties =
        UncheckedCast<HashTableBase>(LoadSlowProperties(receiver));
    CSA_ASSERT(this, Word32Or(IsNameDictionary(properties),
                              IsGlobalDictionary(properties)));
    STATIC_ASSERT(static_cast<int>(NameDictionary::kNumberOfElementsIndex) ==
                  static_cast<int>(GlobalDictionary::kNumberOfElementsIndex));
    TNode<Smi> length = GetNumberOfElements(properties);
    GotoIfNot(TaggedEqual(length, SmiConstant(0)), if_runtime);
    // Check that there are no elements on the {receiver} and its prototype
    // chain. Given that we do not create an EnumCache for dict-mode objects,
    // directly jump to {if_empty} if there are no elements and no properties
    // on the {receiver}.
    CheckPrototypeEnumCache(receiver, receiver_map, if_empty, if_runtime);
  }

  // Check that there are no elements on the fast {receiver} and its
  // prototype chain.
  BIND(&if_cache);
  CheckPrototypeEnumCache(receiver, receiver_map, &if_fast, if_runtime);

  BIND(&if_fast);
  return receiver_map;
}

TNode<Object> CodeStubAssembler::GetArgumentValue(TorqueStructArguments args,
                                                  TNode<IntPtrT> index) {
  return CodeStubArguments(this, args).GetOptionalArgumentValue(index);
}

TorqueStructArguments CodeStubAssembler::GetFrameArguments(
    TNode<RawPtrT> frame, TNode<IntPtrT> argc) {
  return CodeStubArguments(this, argc, frame).GetTorqueArguments();
}

void CodeStubAssembler::Print(const char* s) {
  std::string formatted(s);
  formatted += "\n";
  CallRuntime(Runtime::kGlobalPrint, NoContextConstant(),
              StringConstant(formatted.c_str()));
}

void CodeStubAssembler::Print(const char* prefix,
                              TNode<MaybeObject> tagged_value) {
  if (prefix != nullptr) {
    std::string formatted(prefix);
    formatted += ": ";
    Handle<String> string = isolate()->factory()->NewStringFromAsciiChecked(
        formatted.c_str(), AllocationType::kOld);
    CallRuntime(Runtime::kGlobalPrint, NoContextConstant(),
                HeapConstant(string));
  }
  // CallRuntime only accepts Objects, so do an UncheckedCast to object.
  // DebugPrint explicitly checks whether the tagged value is a MaybeObject.
  TNode<Object> arg = UncheckedCast<Object>(tagged_value);
  CallRuntime(Runtime::kDebugPrint, NoContextConstant(), arg);
}

void CodeStubAssembler::PerformStackCheck(TNode<Context> context) {
  Label ok(this), stack_check_interrupt(this, Label::kDeferred);

  TNode<UintPtrT> stack_limit = UncheckedCast<UintPtrT>(
      Load(MachineType::Pointer(),
           ExternalConstant(ExternalReference::address_of_jslimit(isolate()))));
  TNode<BoolT> sp_within_limit = StackPointerGreaterThan(stack_limit);

  Branch(sp_within_limit, &ok, &stack_check_interrupt);

  BIND(&stack_check_interrupt);
  CallRuntime(Runtime::kStackGuard, context);
  Goto(&ok);

  BIND(&ok);
}

TNode<Object> CodeStubAssembler::CallApiCallback(
    TNode<Object> context, TNode<RawPtrT> callback, TNode<IntPtrT> argc,
    TNode<Object> data, TNode<Object> holder, TNode<Object> receiver) {
  Callable callable = CodeFactory::CallApiCallback(isolate());
  return CallStub(callable, context, callback, argc, data, holder, receiver);
}

TNode<Object> CodeStubAssembler::CallApiCallback(
    TNode<Object> context, TNode<RawPtrT> callback, TNode<IntPtrT> argc,
    TNode<Object> data, TNode<Object> holder, TNode<Object> receiver,
    TNode<Object> value) {
  Callable callable = CodeFactory::CallApiCallback(isolate());
  return CallStub(callable, context, callback, argc, data, holder, receiver,
                  value);
}

TNode<Object> CodeStubAssembler::CallRuntimeNewArray(
    TNode<Context> context, TNode<Object> receiver, TNode<Object> length,
    TNode<Object> new_target, TNode<Object> allocation_site) {
  // Runtime_NewArray receives arguments in the JS order (to avoid unnecessary
  // copy). Except the last two (new_target and allocation_site) which are add
  // on top of the stack later.
  return CallRuntime(Runtime::kNewArray, context, length, receiver, new_target,
                     allocation_site);
}

void CodeStubAssembler::TailCallRuntimeNewArray(TNode<Context> context,
                                                TNode<Object> receiver,
                                                TNode<Object> length,
                                                TNode<Object> new_target,
                                                TNode<Object> allocation_site) {
  // Runtime_NewArray receives arguments in the JS order (to avoid unnecessary
  // copy). Except the last two (new_target and allocation_site) which are add
  // on top of the stack later.
  return TailCallRuntime(Runtime::kNewArray, context, length, receiver,
                         new_target, allocation_site);
}

TNode<JSArray> CodeStubAssembler::ArrayCreate(TNode<Context> context,
                                              TNode<Number> length) {
  TVARIABLE(JSArray, array);
  Label allocate_js_array(this);

  Label done(this), next(this), runtime(this, Label::kDeferred);
  TNode<Smi> limit = SmiConstant(JSArray::kInitialMaxFastElementArray);
  CSA_ASSERT_BRANCH(this, [=](Label* ok, Label* not_ok) {
    BranchIfNumberRelationalComparison(Operation::kGreaterThanOrEqual, length,
                                       SmiConstant(0), ok, not_ok);
  });
  // This check also transitively covers the case where length is too big
  // to be representable by a SMI and so is not usable with
  // AllocateJSArray.
  BranchIfNumberRelationalComparison(Operation::kGreaterThanOrEqual, length,
                                     limit, &runtime, &next);

  BIND(&runtime);
  {
    TNode<NativeContext> native_context = LoadNativeContext(context);
    TNode<JSFunction> array_function =
        CAST(LoadContextElement(native_context, Context::ARRAY_FUNCTION_INDEX));
    array = CAST(CallRuntimeNewArray(context, array_function, length,
                                     array_function, UndefinedConstant()));
    Goto(&done);
  }

  BIND(&next);
  TNode<Smi> length_smi = CAST(length);

  TNode<Map> array_map = CAST(LoadContextElement(
      context, Context::JS_ARRAY_PACKED_SMI_ELEMENTS_MAP_INDEX));

  // TODO(delphick): Consider using
  // AllocateUninitializedJSArrayWithElements to avoid initializing an
  // array and then writing over it.
  array = AllocateJSArray(PACKED_SMI_ELEMENTS, array_map, length_smi,
                          SmiConstant(0));
  Goto(&done);

  BIND(&done);
  return array.value();
}

void CodeStubAssembler::SetPropertyLength(TNode<Context> context,
                                          TNode<Object> array,
                                          TNode<Number> length) {
  Label fast(this), runtime(this), done(this);
  // There's no need to set the length, if
  // 1) the array is a fast JS array and
  // 2) the new length is equal to the old length.
  // as the set is not observable. Otherwise fall back to the run-time.

  // 1) Check that the array has fast elements.
  // TODO(delphick): Consider changing this since it does an an unnecessary
  // check for SMIs.
  // TODO(delphick): Also we could hoist this to after the array construction
  // and copy the args into array in the same way as the Array constructor.
  BranchIfFastJSArray(array, context, &fast, &runtime);

  BIND(&fast);
  {
    TNode<JSArray> fast_array = CAST(array);

    TNode<Smi> length_smi = CAST(length);
    TNode<Smi> old_length = LoadFastJSArrayLength(fast_array);
    CSA_ASSERT(this, TaggedIsPositiveSmi(old_length));

    // 2) If the created array's length matches the required length, then
    //    there's nothing else to do. Otherwise use the runtime to set the
    //    property as that will insert holes into excess elements or shrink
    //    the backing store as appropriate.
    Branch(SmiNotEqual(length_smi, old_length), &runtime, &done);
  }

  BIND(&runtime);
  {
    SetPropertyStrict(context, array, CodeStubAssembler::LengthStringConstant(),
                      length);
    Goto(&done);
  }

  BIND(&done);
}

TNode<Smi> CodeStubAssembler::RefillMathRandom(
    TNode<NativeContext> native_context) {
  // Cache exhausted, populate the cache. Return value is the new index.
  const TNode<ExternalReference> refill_math_random =
      ExternalConstant(ExternalReference::refill_math_random());
  const TNode<ExternalReference> isolate_ptr =
      ExternalConstant(ExternalReference::isolate_address(isolate()));
  MachineType type_tagged = MachineType::AnyTagged();
  MachineType type_ptr = MachineType::Pointer();

  return CAST(CallCFunction(refill_math_random, type_tagged,
                            std::make_pair(type_ptr, isolate_ptr),
                            std::make_pair(type_tagged, native_context)));
}

TNode<String> CodeStubAssembler::TaggedToDirectString(TNode<Object> value,
                                                      Label* fail) {
  ToDirectStringAssembler to_direct(state(), CAST(value));
  to_direct.TryToDirect(fail);
  to_direct.PointerToData(fail);
  return CAST(value);
}

void CodeStubAssembler::RemoveFinalizationRegistryCellFromUnregisterTokenMap(
    TNode<JSFinalizationRegistry> finalization_registry,
    TNode<WeakCell> weak_cell) {
  const TNode<ExternalReference> remove_cell = ExternalConstant(
      ExternalReference::
          js_finalization_registry_remove_cell_from_unregister_token_map());
  const TNode<ExternalReference> isolate_ptr =
      ExternalConstant(ExternalReference::isolate_address(isolate()));

  CallCFunction(remove_cell, MachineType::Pointer(),
                std::make_pair(MachineType::Pointer(), isolate_ptr),
                std::make_pair(MachineType::AnyTagged(), finalization_registry),
                std::make_pair(MachineType::AnyTagged(), weak_cell));
}

PrototypeCheckAssembler::PrototypeCheckAssembler(
    compiler::CodeAssemblerState* state, Flags flags,
    TNode<NativeContext> native_context, TNode<Map> initial_prototype_map,
    Vector<DescriptorIndexNameValue> properties)
    : CodeStubAssembler(state),
      flags_(flags),
      native_context_(native_context),
      initial_prototype_map_(initial_prototype_map),
      properties_(properties) {}

void PrototypeCheckAssembler::CheckAndBranch(TNode<HeapObject> prototype,
                                             Label* if_unmodified,
                                             Label* if_modified) {
  TNode<Map> prototype_map = LoadMap(prototype);
  TNode<DescriptorArray> descriptors = LoadMapDescriptors(prototype_map);

  // The continuation of a failed fast check: if property identity checks are
  // enabled, we continue there (since they may still classify the prototype as
  // fast), otherwise we bail out.
  Label property_identity_check(this, Label::kDeferred);
  Label* if_fast_check_failed =
      ((flags_ & kCheckPrototypePropertyIdentity) == 0)
          ? if_modified
          : &property_identity_check;

  if ((flags_ & kCheckPrototypePropertyConstness) != 0) {
    // A simple prototype map identity check. Note that map identity does not
    // guarantee unmodified properties. It does guarantee that no new properties
    // have been added, or old properties deleted.

    GotoIfNot(TaggedEqual(prototype_map, initial_prototype_map_),
              if_fast_check_failed);

    // We need to make sure that relevant properties in the prototype have
    // not been tampered with. We do this by checking that their slots
    // in the prototype's descriptor array are still marked as const.

    TNode<Uint32T> combined_details;
    for (int i = 0; i < properties_.length(); i++) {
      // Assert the descriptor index is in-bounds.
      int descriptor = properties_[i].descriptor_index;
      CSA_ASSERT(this, Int32LessThan(Int32Constant(descriptor),
                                     LoadNumberOfDescriptors(descriptors)));

      // Assert that the name is correct. This essentially checks that
      // the descriptor index corresponds to the insertion order in
      // the bootstrapper.
      CSA_ASSERT(
          this,
          TaggedEqual(LoadKeyByDescriptorEntry(descriptors, descriptor),
                      CodeAssembler::LoadRoot(properties_[i].name_root_index)));

      TNode<Uint32T> details =
          DescriptorArrayGetDetails(descriptors, Uint32Constant(descriptor));

      if (i == 0) {
        combined_details = details;
      } else {
        combined_details = Word32And(combined_details, details);
      }
    }

    TNode<Uint32T> constness =
        DecodeWord32<PropertyDetails::ConstnessField>(combined_details);

    Branch(
        Word32Equal(constness,
                    Int32Constant(static_cast<int>(PropertyConstness::kConst))),
        if_unmodified, if_fast_check_failed);
  }

  if ((flags_ & kCheckPrototypePropertyIdentity) != 0) {
    // The above checks have failed, for whatever reason (maybe the prototype
    // map has changed, or a property is no longer const). This block implements
    // a more thorough check that can also accept maps which 1. do not have the
    // initial map, 2. have mutable relevant properties, but 3. still match the
    // expected value for all relevant properties.

    BIND(&property_identity_check);

    int max_descriptor_index = -1;
    for (int i = 0; i < properties_.length(); i++) {
      max_descriptor_index =
          std::max(max_descriptor_index, properties_[i].descriptor_index);
    }

    // If the greatest descriptor index is out of bounds, the map cannot be
    // fast.
    GotoIfNot(Int32LessThan(Int32Constant(max_descriptor_index),
                            LoadNumberOfDescriptors(descriptors)),
              if_modified);

    // Logic below only handles maps with fast properties.
    GotoIfMapHasSlowProperties(prototype_map, if_modified);

    for (int i = 0; i < properties_.length(); i++) {
      const DescriptorIndexNameValue& p = properties_[i];
      const int descriptor = p.descriptor_index;

      // Check if the name is correct. This essentially checks that
      // the descriptor index corresponds to the insertion order in
      // the bootstrapper.
      GotoIfNot(TaggedEqual(LoadKeyByDescriptorEntry(descriptors, descriptor),
                            CodeAssembler::LoadRoot(p.name_root_index)),
                if_modified);

      // Finally, check whether the actual value equals the expected value.
      TNode<Uint32T> details =
          DescriptorArrayGetDetails(descriptors, Uint32Constant(descriptor));
      TVARIABLE(Uint32T, var_details, details);
      TVARIABLE(Object, var_value);

      const int key_index = DescriptorArray::ToKeyIndex(descriptor);
      LoadPropertyFromFastObject(prototype, prototype_map, descriptors,
                                 IntPtrConstant(key_index), &var_details,
                                 &var_value);

      TNode<Object> actual_value = var_value.value();
      TNode<Object> expected_value =
          LoadContextElement(native_context_, p.expected_value_context_index);
      GotoIfNot(TaggedEqual(actual_value, expected_value), if_modified);
    }

    Goto(if_unmodified);
  }
}

}  // namespace internal
}  // namespace v8
