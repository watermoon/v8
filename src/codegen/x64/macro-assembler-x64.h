// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDED_FROM_MACRO_ASSEMBLER_H
#error This header must be included via macro-assembler.h
#endif

#ifndef V8_CODEGEN_X64_MACRO_ASSEMBLER_X64_H_
#define V8_CODEGEN_X64_MACRO_ASSEMBLER_X64_H_

#include "src/base/flags.h"
#include "src/codegen/bailout-reason.h"
#include "src/codegen/x64/assembler-x64.h"
#include "src/common/globals.h"
#include "src/objects/contexts.h"

namespace v8 {
namespace internal {

// Convenience for platform-independent signatures.
using MemOperand = Operand;

class StringConstantBase;

enum RememberedSetAction { EMIT_REMEMBERED_SET, OMIT_REMEMBERED_SET };
enum SmiCheck { INLINE_SMI_CHECK, OMIT_SMI_CHECK };

struct SmiIndex {
  SmiIndex(Register index_register, ScaleFactor scale)
      : reg(index_register), scale(scale) {}
  Register reg;
  ScaleFactor scale;
};

// TODO(victorgomes): Move definition to macro-assembler.h, once all other
// platforms are updated.
enum class StackLimitKind { kInterruptStackLimit, kRealStackLimit };

// Convenient class to access arguments below the stack pointer.
class StackArgumentsAccessor {
 public:
  // argc = the number of arguments not including the receiver.
  explicit StackArgumentsAccessor(Register argc) : argc_(argc) {
    DCHECK_NE(argc_, no_reg);
  }

  // Argument 0 is the receiver (despite argc not including the receiver).
  Operand operator[](int index) const { return GetArgumentOperand(index); }

  Operand GetArgumentOperand(int index) const;
  Operand GetReceiverOperand() const { return GetArgumentOperand(0); }

 private:
  const Register argc_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(StackArgumentsAccessor);
};

class V8_EXPORT_PRIVATE TurboAssembler : public TurboAssemblerBase {
 public:
  using TurboAssemblerBase::TurboAssemblerBase;

  template <typename Dst, typename... Args>
  struct AvxHelper {
    Assembler* assm;
    base::Optional<CpuFeature> feature = base::nullopt;
    // Call a method where the AVX version expects the dst argument to be
    // duplicated.
    template <void (Assembler::*avx)(Dst, Dst, Args...),
              void (Assembler::*no_avx)(Dst, Args...)>
    void emit(Dst dst, Args... args) {
      if (CpuFeatures::IsSupported(AVX)) {
        CpuFeatureScope scope(assm, AVX);
        (assm->*avx)(dst, dst, args...);
      } else if (feature.has_value()) {
        DCHECK(CpuFeatures::IsSupported(*feature));
        CpuFeatureScope scope(assm, *feature);
        (assm->*no_avx)(dst, args...);
      } else {
        (assm->*no_avx)(dst, args...);
      }
    }

    // Call a method where the AVX version expects no duplicated dst argument.
    template <void (Assembler::*avx)(Dst, Args...),
              void (Assembler::*no_avx)(Dst, Args...)>
    void emit(Dst dst, Args... args) {
      if (CpuFeatures::IsSupported(AVX)) {
        CpuFeatureScope scope(assm, AVX);
        (assm->*avx)(dst, args...);
      } else if (feature.has_value()) {
        DCHECK(CpuFeatures::IsSupported(*feature));
        CpuFeatureScope scope(assm, *feature);
        (assm->*no_avx)(dst, args...);
      } else {
        (assm->*no_avx)(dst, args...);
      }
    }
  };

#define AVX_OP(macro_name, name)                                             \
  template <typename Dst, typename... Args>                                  \
  void macro_name(Dst dst, Args... args) {                                   \
    AvxHelper<Dst, Args...>{this}                                            \
        .template emit<&Assembler::v##name, &Assembler::name>(dst, args...); \
  }

#define AVX_OP_SSE3(macro_name, name)                                        \
  template <typename Dst, typename... Args>                                  \
  void macro_name(Dst dst, Args... args) {                                   \
    AvxHelper<Dst, Args...>{this, base::Optional<CpuFeature>(SSE3)}          \
        .template emit<&Assembler::v##name, &Assembler::name>(dst, args...); \
  }

#define AVX_OP_SSSE3(macro_name, name)                                       \
  template <typename Dst, typename... Args>                                  \
  void macro_name(Dst dst, Args... args) {                                   \
    AvxHelper<Dst, Args...>{this, base::Optional<CpuFeature>(SSSE3)}         \
        .template emit<&Assembler::v##name, &Assembler::name>(dst, args...); \
  }

#define AVX_OP_SSE4_1(macro_name, name)                                      \
  template <typename Dst, typename... Args>                                  \
  void macro_name(Dst dst, Args... args) {                                   \
    AvxHelper<Dst, Args...>{this, base::Optional<CpuFeature>(SSE4_1)}        \
        .template emit<&Assembler::v##name, &Assembler::name>(dst, args...); \
  }
#define AVX_OP_SSE4_2(macro_name, name)                                      \
  template <typename Dst, typename... Args>                                  \
  void macro_name(Dst dst, Args... args) {                                   \
    AvxHelper<Dst, Args...>{this, base::Optional<CpuFeature>(SSE4_2)}        \
        .template emit<&Assembler::v##name, &Assembler::name>(dst, args...); \
  }
  AVX_OP(Subsd, subsd)
  AVX_OP(Divss, divss)
  AVX_OP(Divsd, divsd)
  AVX_OP(Orps, orps)
  AVX_OP(Xorps, xorps)
  AVX_OP(Xorpd, xorpd)
  AVX_OP(Movd, movd)
  AVX_OP(Movq, movq)
  AVX_OP(Movaps, movaps)
  AVX_OP(Movapd, movapd)
  AVX_OP(Movups, movups)
  AVX_OP(Movmskps, movmskps)
  AVX_OP(Movmskpd, movmskpd)
  AVX_OP(Pmovmskb, pmovmskb)
  AVX_OP(Movss, movss)
  AVX_OP(Movsd, movsd)
  AVX_OP(Movdqu, movdqu)
  AVX_OP(Movlps, movlps)
  AVX_OP(Movhps, movhps)
  AVX_OP(Pcmpeqb, pcmpeqb)
  AVX_OP(Pcmpeqw, pcmpeqw)
  AVX_OP(Pcmpeqd, pcmpeqd)
  AVX_OP(Pcmpgtb, pcmpgtb)
  AVX_OP(Pcmpgtw, pcmpgtw)
  AVX_OP(Pmaxsw, pmaxsw)
  AVX_OP(Pmaxub, pmaxub)
  AVX_OP(Pminsw, pminsw)
  AVX_OP(Pminub, pminub)
  AVX_OP(Addss, addss)
  AVX_OP(Addsd, addsd)
  AVX_OP(Mulsd, mulsd)
  AVX_OP(Andps, andps)
  AVX_OP(Andnps, andnps)
  AVX_OP(Andpd, andpd)
  AVX_OP(Andnpd, andnpd)
  AVX_OP(Orpd, orpd)
  AVX_OP(Cmpeqps, cmpeqps)
  AVX_OP(Cmpltps, cmpltps)
  AVX_OP(Cmpleps, cmpleps)
  AVX_OP(Cmpneqps, cmpneqps)
  AVX_OP(Cmpnltps, cmpnltps)
  AVX_OP(Cmpnleps, cmpnleps)
  AVX_OP(Cmpeqpd, cmpeqpd)
  AVX_OP(Cmpltpd, cmpltpd)
  AVX_OP(Cmplepd, cmplepd)
  AVX_OP(Cmpneqpd, cmpneqpd)
  AVX_OP(Cmpnltpd, cmpnltpd)
  AVX_OP(Cmpnlepd, cmpnlepd)
  AVX_OP(Sqrtss, sqrtss)
  AVX_OP(Sqrtsd, sqrtsd)
  AVX_OP(Sqrtps, sqrtps)
  AVX_OP(Sqrtpd, sqrtpd)
  AVX_OP(Cvttps2dq, cvttps2dq)
  AVX_OP(Ucomiss, ucomiss)
  AVX_OP(Ucomisd, ucomisd)
  AVX_OP(Pand, pand)
  AVX_OP(Por, por)
  AVX_OP(Pxor, pxor)
  AVX_OP(Psubb, psubb)
  AVX_OP(Psubw, psubw)
  AVX_OP(Psubd, psubd)
  AVX_OP(Psubq, psubq)
  AVX_OP(Psubsb, psubsb)
  AVX_OP(Psubsw, psubsw)
  AVX_OP(Psubusb, psubusb)
  AVX_OP(Psubusw, psubusw)
  AVX_OP(Pslld, pslld)
  AVX_OP(Pavgb, pavgb)
  AVX_OP(Pavgw, pavgw)
  AVX_OP(Psraw, psraw)
  AVX_OP(Psrad, psrad)
  AVX_OP(Psllw, psllw)
  AVX_OP(Psllq, psllq)
  AVX_OP(Psrlw, psrlw)
  AVX_OP(Psrld, psrld)
  AVX_OP(Psrlq, psrlq)
  AVX_OP(Pmaddwd, pmaddwd)
  AVX_OP(Paddb, paddb)
  AVX_OP(Paddw, paddw)
  AVX_OP(Paddd, paddd)
  AVX_OP(Paddq, paddq)
  AVX_OP(Paddsb, paddsb)
  AVX_OP(Paddsw, paddsw)
  AVX_OP(Paddusb, paddusb)
  AVX_OP(Paddusw, paddusw)
  AVX_OP(Pcmpgtd, pcmpgtd)
  AVX_OP(Pmullw, pmullw)
  AVX_OP(Pmuludq, pmuludq)
  AVX_OP(Addpd, addpd)
  AVX_OP(Subpd, subpd)
  AVX_OP(Mulpd, mulpd)
  AVX_OP(Minps, minps)
  AVX_OP(Minpd, minpd)
  AVX_OP(Divpd, divpd)
  AVX_OP(Maxps, maxps)
  AVX_OP(Maxpd, maxpd)
  AVX_OP(Cvtdq2ps, cvtdq2ps)
  AVX_OP(Rcpps, rcpps)
  AVX_OP(Rsqrtps, rsqrtps)
  AVX_OP(Addps, addps)
  AVX_OP(Subps, subps)
  AVX_OP(Mulps, mulps)
  AVX_OP(Divps, divps)
  AVX_OP(Pshuflw, pshuflw)
  AVX_OP(Pshufhw, pshufhw)
  AVX_OP(Packsswb, packsswb)
  AVX_OP(Packuswb, packuswb)
  AVX_OP(Packssdw, packssdw)
  AVX_OP(Punpcklbw, punpcklbw)
  AVX_OP(Punpcklwd, punpcklwd)
  AVX_OP(Punpckldq, punpckldq)
  AVX_OP(Punpckhbw, punpckhbw)
  AVX_OP(Punpckhwd, punpckhwd)
  AVX_OP(Punpckhdq, punpckhdq)
  AVX_OP(Punpcklqdq, punpcklqdq)
  AVX_OP(Punpckhqdq, punpckhqdq)
  AVX_OP(Pshufd, pshufd)
  AVX_OP(Cmpps, cmpps)
  AVX_OP(Cmppd, cmppd)
  AVX_OP(Movlhps, movlhps)
  AVX_OP_SSE3(Haddps, haddps)
  AVX_OP_SSE3(Movddup, movddup)
  AVX_OP_SSSE3(Phaddd, phaddd)
  AVX_OP_SSSE3(Phaddw, phaddw)
  AVX_OP_SSSE3(Pshufb, pshufb)
  AVX_OP_SSSE3(Psignb, psignb)
  AVX_OP_SSSE3(Psignw, psignw)
  AVX_OP_SSSE3(Psignd, psignd)
  AVX_OP_SSSE3(Palignr, palignr)
  AVX_OP_SSSE3(Pabsb, pabsb)
  AVX_OP_SSSE3(Pabsw, pabsw)
  AVX_OP_SSSE3(Pabsd, pabsd)
  AVX_OP_SSE4_1(Pcmpeqq, pcmpeqq)
  AVX_OP_SSE4_1(Packusdw, packusdw)
  AVX_OP_SSE4_1(Pminsb, pminsb)
  AVX_OP_SSE4_1(Pminsd, pminsd)
  AVX_OP_SSE4_1(Pminuw, pminuw)
  AVX_OP_SSE4_1(Pminud, pminud)
  AVX_OP_SSE4_1(Pmaxsb, pmaxsb)
  AVX_OP_SSE4_1(Pmaxsd, pmaxsd)
  AVX_OP_SSE4_1(Pmaxuw, pmaxuw)
  AVX_OP_SSE4_1(Pmaxud, pmaxud)
  AVX_OP_SSE4_1(Pmulld, pmulld)
  AVX_OP_SSE4_1(Extractps, extractps)
  AVX_OP_SSE4_1(Insertps, insertps)
  AVX_OP_SSE4_1(Pinsrq, pinsrq)
  AVX_OP_SSE4_1(Pblendw, pblendw)
  AVX_OP_SSE4_1(Ptest, ptest)
  AVX_OP_SSE4_1(Pmovsxbw, pmovsxbw)
  AVX_OP_SSE4_1(Pmovsxwd, pmovsxwd)
  AVX_OP_SSE4_1(Pmovsxdq, pmovsxdq)
  AVX_OP_SSE4_1(Pmovzxbw, pmovzxbw)
  AVX_OP_SSE4_1(Pmovzxwd, pmovzxwd)
  AVX_OP_SSE4_1(Pmovzxdq, pmovzxdq)
  AVX_OP_SSE4_1(Pextrb, pextrb)
  AVX_OP_SSE4_1(Pextrw, pextrw)
  AVX_OP_SSE4_1(Pextrq, pextrq)
  AVX_OP_SSE4_1(Roundps, roundps)
  AVX_OP_SSE4_1(Roundpd, roundpd)
  AVX_OP_SSE4_1(Roundss, roundss)
  AVX_OP_SSE4_1(Roundsd, roundsd)
  AVX_OP_SSE4_2(Pcmpgtq, pcmpgtq)

#undef AVX_OP

  void PushReturnAddressFrom(Register src) { pushq(src); }
  void PopReturnAddressTo(Register dst) { popq(dst); }

  void Ret();

  // Return and drop arguments from stack, where the number of arguments
  // may be bigger than 2^16 - 1.  Requires a scratch register.
  void Ret(int bytes_dropped, Register scratch);

  // Load a register with a long value as efficiently as possible.
  void Set(Register dst, int64_t x);
  void Set(Operand dst, intptr_t x);

  // Operations on roots in the root-array.
  void LoadRoot(Register destination, RootIndex index) override;
  void LoadRoot(Operand destination, RootIndex index) {
    LoadRoot(kScratchRegister, index);
    movq(destination, kScratchRegister);
  }

  void Push(Register src);
  void Push(Operand src);
  void Push(Immediate value);
  void Push(Smi smi);
  void Push(Handle<HeapObject> source);

  enum class PushArrayOrder { kNormal, kReverse };
  // `array` points to the first element (the lowest address).
  // `array` and `size` are not modified.
  void PushArray(Register array, Register size, Register scratch,
                 PushArrayOrder order = PushArrayOrder::kNormal);

  // Before calling a C-function from generated code, align arguments on stack.
  // After aligning the frame, arguments must be stored in rsp[0], rsp[8],
  // etc., not pushed. The argument count assumes all arguments are word sized.
  // The number of slots reserved for arguments depends on platform. On Windows
  // stack slots are reserved for the arguments passed in registers. On other
  // platforms stack slots are only reserved for the arguments actually passed
  // on the stack.
  void PrepareCallCFunction(int num_arguments);

  // Calls a C function and cleans up the space for arguments allocated
  // by PrepareCallCFunction. The called function is not allowed to trigger a
  // garbage collection, since that might move the code and invalidate the
  // return address (unless this is somehow accounted for by the called
  // function).
  void CallCFunction(ExternalReference function, int num_arguments);
  void CallCFunction(Register function, int num_arguments);

  // Calculate the number of stack slots to reserve for arguments when calling a
  // C function.
  int ArgumentStackSlotsForCFunctionCall(int num_arguments);

  void CheckPageFlag(Register object, Register scratch, int mask, Condition cc,
                     Label* condition_met,
                     Label::Distance condition_met_distance = Label::kFar);

  void Cvtss2sd(XMMRegister dst, XMMRegister src);
  void Cvtss2sd(XMMRegister dst, Operand src);
  void Cvtsd2ss(XMMRegister dst, XMMRegister src);
  void Cvtsd2ss(XMMRegister dst, Operand src);
  void Cvttsd2si(Register dst, XMMRegister src);
  void Cvttsd2si(Register dst, Operand src);
  void Cvttsd2siq(Register dst, XMMRegister src);
  void Cvttsd2siq(Register dst, Operand src);
  void Cvttss2si(Register dst, XMMRegister src);
  void Cvttss2si(Register dst, Operand src);
  void Cvttss2siq(Register dst, XMMRegister src);
  void Cvttss2siq(Register dst, Operand src);
  void Cvtlui2ss(XMMRegister dst, Register src);
  void Cvtlui2ss(XMMRegister dst, Operand src);
  void Cvtlui2sd(XMMRegister dst, Register src);
  void Cvtlui2sd(XMMRegister dst, Operand src);
  void Cvtqui2ss(XMMRegister dst, Register src);
  void Cvtqui2ss(XMMRegister dst, Operand src);
  void Cvtqui2sd(XMMRegister dst, Register src);
  void Cvtqui2sd(XMMRegister dst, Operand src);
  void Cvttsd2uiq(Register dst, Operand src, Label* fail = nullptr);
  void Cvttsd2uiq(Register dst, XMMRegister src, Label* fail = nullptr);
  void Cvttss2uiq(Register dst, Operand src, Label* fail = nullptr);
  void Cvttss2uiq(Register dst, XMMRegister src, Label* fail = nullptr);

  // cvtsi2sd and cvtsi2ss instructions only write to the low 64/32-bit of dst
  // register, which hinders register renaming and makes dependence chains
  // longer. So we use xorpd to clear the dst register before cvtsi2sd for
  // non-AVX and a scratch XMM register as first src for AVX to solve this
  // issue.
  void Cvtqsi2ss(XMMRegister dst, Register src);
  void Cvtqsi2ss(XMMRegister dst, Operand src);
  void Cvtqsi2sd(XMMRegister dst, Register src);
  void Cvtqsi2sd(XMMRegister dst, Operand src);
  void Cvtlsi2ss(XMMRegister dst, Register src);
  void Cvtlsi2ss(XMMRegister dst, Operand src);
  void Cvtlsi2sd(XMMRegister dst, Register src);
  void Cvtlsi2sd(XMMRegister dst, Operand src);

  void Lzcntq(Register dst, Register src);
  void Lzcntq(Register dst, Operand src);
  void Lzcntl(Register dst, Register src);
  void Lzcntl(Register dst, Operand src);
  void Tzcntq(Register dst, Register src);
  void Tzcntq(Register dst, Operand src);
  void Tzcntl(Register dst, Register src);
  void Tzcntl(Register dst, Operand src);
  void Popcntl(Register dst, Register src);
  void Popcntl(Register dst, Operand src);
  void Popcntq(Register dst, Register src);
  void Popcntq(Register dst, Operand src);

  // Is the value a tagged smi.
  Condition CheckSmi(Register src);
  Condition CheckSmi(Operand src);

  // Jump to label if the value is a tagged smi.
  void JumpIfSmi(Register src, Label* on_smi,
                 Label::Distance near_jump = Label::kFar);

  void JumpIfEqual(Register a, int32_t b, Label* dest) {
    cmpl(a, Immediate(b));
    j(equal, dest);
  }

  void JumpIfLessThan(Register a, int32_t b, Label* dest) {
    cmpl(a, Immediate(b));
    j(less, dest);
  }

  void LoadMap(Register destination, Register object);

  void Move(Register dst, Smi source);

  void Move(Operand dst, Smi source) {
    Register constant = GetSmiConstant(source);
    movq(dst, constant);
  }

  void Move(Register dst, ExternalReference ext);

  void Move(XMMRegister dst, uint32_t src);
  void Move(XMMRegister dst, uint64_t src);
  void Move(XMMRegister dst, float src) { Move(dst, bit_cast<uint32_t>(src)); }
  void Move(XMMRegister dst, double src) { Move(dst, bit_cast<uint64_t>(src)); }
  void Move(XMMRegister dst, uint64_t high, uint64_t low);

  // Move if the registers are not identical.
  void Move(Register target, Register source);

  void Move(Register dst, Handle<HeapObject> source,
            RelocInfo::Mode rmode = RelocInfo::FULL_EMBEDDED_OBJECT);
  void Move(Operand dst, Handle<HeapObject> source,
            RelocInfo::Mode rmode = RelocInfo::FULL_EMBEDDED_OBJECT);

  // Loads a pointer into a register with a relocation mode.
  void Move(Register dst, Address ptr, RelocInfo::Mode rmode) {
    // This method must not be used with heap object references. The stored
    // address is not GC safe. Use the handle version instead.
    DCHECK(rmode == RelocInfo::NONE || rmode > RelocInfo::LAST_GCED_ENUM);
    movq(dst, Immediate64(ptr, rmode));
  }

  // Move src0 to dst0 and src1 to dst1, handling possible overlaps.
  void MovePair(Register dst0, Register src0, Register dst1, Register src1);

  void MoveStringConstant(
      Register result, const StringConstantBase* string,
      RelocInfo::Mode rmode = RelocInfo::FULL_EMBEDDED_OBJECT);

  // Convert smi to word-size sign-extended value.
  void SmiUntag(Register reg);
  // Requires dst != src
  void SmiUntag(Register dst, Register src);
  void SmiUntag(Register dst, Operand src);

  // Loads the address of the external reference into the destination
  // register.
  void LoadAddress(Register destination, ExternalReference source);

  void LoadFromConstantsTable(Register destination,
                              int constant_index) override;
  void LoadRootRegisterOffset(Register destination, intptr_t offset) override;
  void LoadRootRelative(Register destination, int32_t offset) override;

  // Operand pointing to an external reference.
  // May emit code to set up the scratch register. The operand is
  // only guaranteed to be correct as long as the scratch register
  // isn't changed.
  // If the operand is used more than once, use a scratch register
  // that is guaranteed not to be clobbered.
  Operand ExternalReferenceAsOperand(ExternalReference reference,
                                     Register scratch = kScratchRegister);

  void Call(Register reg) { call(reg); }
  void Call(Operand op);
  void Call(Handle<Code> code_object, RelocInfo::Mode rmode);
  void Call(Address destination, RelocInfo::Mode rmode);
  void Call(ExternalReference ext);
  void Call(Label* target) { call(target); }

  Operand EntryFromBuiltinIndexAsOperand(Builtins::Name builtin_index);
  Operand EntryFromBuiltinIndexAsOperand(Register builtin_index);
  void CallBuiltinByIndex(Register builtin_index) override;
  void CallBuiltin(int builtin_index);

  void LoadCodeObjectEntry(Register destination, Register code_object) override;
  void CallCodeObject(Register code_object) override;
  void JumpCodeObject(Register code_object) override;

  void RetpolineCall(Register reg);
  void RetpolineCall(Address destination, RelocInfo::Mode rmode);

  void Jump(Address destination, RelocInfo::Mode rmode);
  void Jump(const ExternalReference& reference) override;
  void Jump(Operand op);
  void Jump(Handle<Code> code_object, RelocInfo::Mode rmode,
            Condition cc = always);

  void RetpolineJump(Register reg);

  void CallForDeoptimization(Builtins::Name target, int deopt_id, Label* exit,
                             DeoptimizeKind kind,
                             Label* jump_deoptimization_entry_label);

  void Trap() override;
  void DebugBreak() override;

  // Shufps that will mov src into dst if AVX is not supported.
  void Shufps(XMMRegister dst, XMMRegister src, byte imm8);

  // Non-SSE2 instructions.
  void Pextrd(Register dst, XMMRegister src, uint8_t imm8);

  void Pinsrb(XMMRegister dst, XMMRegister src1, Register src2, uint8_t imm8);
  void Pinsrb(XMMRegister dst, XMMRegister src1, Operand src2, uint8_t imm8);
  void Pinsrw(XMMRegister dst, XMMRegister src1, Register src2, uint8_t imm8);
  void Pinsrw(XMMRegister dst, XMMRegister src1, Operand src2, uint8_t imm8);
  void Pinsrd(XMMRegister dst, XMMRegister src1, Register src2, uint8_t imm8);
  void Pinsrd(XMMRegister dst, XMMRegister src1, Operand src2, uint8_t imm8);
  void Pinsrd(XMMRegister dst, Register src2, uint8_t imm8);
  void Pinsrd(XMMRegister dst, Operand src2, uint8_t imm8);
  void Pinsrq(XMMRegister dst, XMMRegister src1, Register src2, uint8_t imm8);
  void Pinsrq(XMMRegister dst, XMMRegister src1, Operand src2, uint8_t imm8);

  void Psllq(XMMRegister dst, int imm8) { Psllq(dst, static_cast<byte>(imm8)); }
  void Psllq(XMMRegister dst, byte imm8);
  void Psrlq(XMMRegister dst, int imm8) { Psrlq(dst, static_cast<byte>(imm8)); }
  void Psrlq(XMMRegister dst, byte imm8);
  void Pslld(XMMRegister dst, byte imm8);
  void Psrld(XMMRegister dst, byte imm8);

  void Pblendvb(XMMRegister dst, XMMRegister src1, XMMRegister src2,
                XMMRegister mask);
  void Blendvps(XMMRegister dst, XMMRegister src1, XMMRegister src2,
                XMMRegister mask);
  void Blendvpd(XMMRegister dst, XMMRegister src1, XMMRegister src2,
                XMMRegister mask);

  // Supports both SSE and AVX. Move src1 to dst if they are not equal on SSE.
  void Pshufb(XMMRegister dst, XMMRegister src1, XMMRegister src2);

  void CompareRoot(Register with, RootIndex index);
  void CompareRoot(Operand with, RootIndex index);

  // Generates function and stub prologue code.
  void StubPrologue(StackFrame::Type type);
  void Prologue();

  // Calls Abort(msg) if the condition cc is not satisfied.
  // Use --debug_code to enable.
  void Assert(Condition cc, AbortReason reason);

  // Like Assert(), but without condition.
  // Use --debug_code to enable.
  void AssertUnreachable(AbortReason reason);

  // Abort execution if a 64 bit register containing a 32 bit payload does not
  // have zeros in the top 32 bits, enabled via --debug-code.
  void AssertZeroExtended(Register reg);

  // Like Assert(), but always enabled.
  void Check(Condition cc, AbortReason reason);

  // Print a message to stdout and abort execution.
  void Abort(AbortReason msg);

  // Check that the stack is aligned.
  void CheckStackAlignment();

  // Activation support.
  void EnterFrame(StackFrame::Type type);
  void EnterFrame(StackFrame::Type type, bool load_constant_pool_pointer_reg) {
    // Out-of-line constant pool not implemented on x64.
    UNREACHABLE();
  }
  void LeaveFrame(StackFrame::Type type);

// Allocate stack space of given size (i.e. decrement {rsp} by the value
// stored in the given register, or by a constant). If you need to perform a
// stack check, do it before calling this function because this function may
// write into the newly allocated space. It may also overwrite the given
// register's value, in the version that takes a register.
#ifdef V8_TARGET_OS_WIN
  void AllocateStackSpace(Register bytes_scratch);
  void AllocateStackSpace(int bytes);
#else
  void AllocateStackSpace(Register bytes) { subq(rsp, bytes); }
  void AllocateStackSpace(int bytes) { subq(rsp, Immediate(bytes)); }
#endif

  // Removes current frame and its arguments from the stack preserving the
  // arguments and a return address pushed to the stack for the next call.  Both
  // |callee_args_count| and |caller_args_count| do not include receiver.
  // |callee_args_count| is not modified. |caller_args_count| is trashed.
  void PrepareForTailCall(Register callee_args_count,
                          Register caller_args_count, Register scratch0,
                          Register scratch1);

  void InitializeRootRegister() {
    ExternalReference isolate_root = ExternalReference::isolate_root(isolate());
    Move(kRootRegister, isolate_root);
  }

  void SaveRegisters(RegList registers);
  void RestoreRegisters(RegList registers);

  void CallRecordWriteStub(Register object, Register address,
                           RememberedSetAction remembered_set_action,
                           SaveFPRegsMode fp_mode);
  void CallRecordWriteStub(Register object, Register address,
                           RememberedSetAction remembered_set_action,
                           SaveFPRegsMode fp_mode, Address wasm_target);
  void CallEphemeronKeyBarrier(Register object, Register address,
                               SaveFPRegsMode fp_mode);

  void MoveNumber(Register dst, double value);
  void MoveNonSmi(Register dst, double value);

  // Calculate how much stack space (in bytes) are required to store caller
  // registers excluding those specified in the arguments.
  int RequiredStackSizeForCallerSaved(SaveFPRegsMode fp_mode,
                                      Register exclusion1 = no_reg,
                                      Register exclusion2 = no_reg,
                                      Register exclusion3 = no_reg) const;

  // PushCallerSaved and PopCallerSaved do not arrange the registers in any
  // particular order so they are not useful for calls that can cause a GC.
  // The caller can exclude up to 3 registers that do not need to be saved and
  // restored.

  // Push caller saved registers on the stack, and return the number of bytes
  // stack pointer is adjusted.
  int PushCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1 = no_reg,
                      Register exclusion2 = no_reg,
                      Register exclusion3 = no_reg);
  // Restore caller saved registers from the stack, and return the number of
  // bytes stack pointer is adjusted.
  int PopCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1 = no_reg,
                     Register exclusion2 = no_reg,
                     Register exclusion3 = no_reg);

  // Compute the start of the generated instruction stream from the current PC.
  // This is an alternative to embedding the {CodeObject} handle as a reference.
  void ComputeCodeStartAddress(Register dst);

  void ResetSpeculationPoisonRegister();

  // Control-flow integrity:

  // Define a function entrypoint. This doesn't emit any code for this
  // architecture, as control-flow integrity is not supported for it.
  void CodeEntry() {}
  // Define an exception handler.
  void ExceptionHandler() {}
  // Define an exception handler and bind a label.
  void BindExceptionHandler(Label* label) { bind(label); }

  // ---------------------------------------------------------------------------
  // Pointer compression support

  // Loads a field containing a HeapObject and decompresses it if pointer
  // compression is enabled.
  void LoadTaggedPointerField(Register destination, Operand field_operand);

  // Loads a field containing any tagged value and decompresses it if necessary.
  void LoadAnyTaggedField(Register destination, Operand field_operand);

  // Loads a field containing a HeapObject, decompresses it if necessary and
  // pushes full pointer to the stack. When pointer compression is enabled,
  // uses |scratch| to decompress the value.
  void PushTaggedPointerField(Operand field_operand, Register scratch);

  // Loads a field containing any tagged value, decompresses it if necessary and
  // pushes the full pointer to the stack. When pointer compression is enabled,
  // uses |scratch| to decompress the value.
  void PushTaggedAnyField(Operand field_operand, Register scratch);

  // Loads a field containing smi value and untags it.
  void SmiUntagField(Register dst, Operand src);

  // Compresses tagged value if necessary and stores it to given on-heap
  // location.
  void StoreTaggedField(Operand dst_field_operand, Immediate immediate);
  void StoreTaggedField(Operand dst_field_operand, Register value);

  // The following macros work even when pointer compression is not enabled.
  void DecompressTaggedSigned(Register destination, Operand field_operand);
  void DecompressTaggedPointer(Register destination, Operand field_operand);
  void DecompressTaggedPointer(Register destination, Register source);
  void DecompressAnyTagged(Register destination, Operand field_operand);

  // ---------------------------------------------------------------------------
  // V8 Heap sandbox support

  // Loads a field containing off-heap pointer and does necessary decoding
  // if V8 heap sandbox is enabled.
  void LoadExternalPointerField(Register destination, Operand field_operand,
                                ExternalPointerTag tag);

 protected:
  static const int kSmiShift = kSmiTagSize + kSmiShiftSize;

  // Returns a register holding the smi value. The register MUST NOT be
  // modified. It may be the "smi 1 constant" register.
  Register GetSmiConstant(Smi value);

  void CallRecordWriteStub(Register object, Register address,
                           RememberedSetAction remembered_set_action,
                           SaveFPRegsMode fp_mode, Handle<Code> code_target,
                           Address wasm_target);
};

// MacroAssembler implements a collection of frequently used macros.
// MacroAssembler 实现了一簇经常用到的宏
class V8_EXPORT_PRIVATE MacroAssembler : public TurboAssembler {
 public:
  using TurboAssembler::TurboAssembler;

  // Loads and stores the value of an external reference.
  // Special case code for load and store to take advantage of
  // load_rax/store_rax if possible/necessary.
  // For other operations, just use:
  //   Operand operand = ExternalReferenceAsOperand(extref);
  //   operation(operand, ..);
  void Load(Register destination, ExternalReference source);
  void Store(ExternalReference destination, Register source);

  // Pushes the address of the external reference onto the stack.
  void PushAddress(ExternalReference source);

  // Operations on roots in the root-array.
  // Load a root value where the index (or part of it) is variable.
  // The variable_offset register is added to the fixed_offset value
  // to get the index into the root-array.
  void PushRoot(RootIndex index);

  // Compare the object in a register to a value and jump if they are equal.
  void JumpIfRoot(Register with, RootIndex index, Label* if_equal,
                  Label::Distance if_equal_distance = Label::kFar) {
    CompareRoot(with, index);
    j(equal, if_equal, if_equal_distance);
  }
  void JumpIfRoot(Operand with, RootIndex index, Label* if_equal,
                  Label::Distance if_equal_distance = Label::kFar) {
    CompareRoot(with, index);
    j(equal, if_equal, if_equal_distance);
  }

  // Compare the object in a register to a value and jump if they are not equal.
  void JumpIfNotRoot(Register with, RootIndex index, Label* if_not_equal,
                     Label::Distance if_not_equal_distance = Label::kFar) {
    CompareRoot(with, index);
    j(not_equal, if_not_equal, if_not_equal_distance);
  }
  void JumpIfNotRoot(Operand with, RootIndex index, Label* if_not_equal,
                     Label::Distance if_not_equal_distance = Label::kFar) {
    CompareRoot(with, index);
    j(not_equal, if_not_equal, if_not_equal_distance);
  }

  // ---------------------------------------------------------------------------
  // GC Support

  // Notify the garbage collector that we wrote a pointer into an object.
  // |object| is the object being stored into, |value| is the object being
  // stored.  value and scratch registers are clobbered by the operation.
  // The offset is the offset from the start of the object, not the offset from
  // the tagged HeapObject pointer.  For use with FieldOperand(reg, off).
  void RecordWriteField(
      Register object, int offset, Register value, Register scratch,
      SaveFPRegsMode save_fp,
      RememberedSetAction remembered_set_action = EMIT_REMEMBERED_SET,
      SmiCheck smi_check = INLINE_SMI_CHECK);

  // For page containing |object| mark region covering |address|
  // dirty. |object| is the object being stored into, |value| is the
  // object being stored. The address and value registers are clobbered by the
  // operation.  RecordWrite filters out smis so it does not update
  // the write barrier if the value is a smi.
  void RecordWrite(
      Register object, Register address, Register value, SaveFPRegsMode save_fp,
      RememberedSetAction remembered_set_action = EMIT_REMEMBERED_SET,
      SmiCheck smi_check = INLINE_SMI_CHECK);

  // Frame restart support.
  void MaybeDropFrames();

  // Enter specific kind of exit frame; either in normal or
  // debug mode. Expects the number of arguments in register rax and
  // sets up the number of arguments in register rdi and the pointer
  // to the first argument in register rsi.
  //
  // Allocates arg_stack_space * kSystemPointerSize memory (not GCed) on the
  // stack accessible via StackSpaceOperand.
  void EnterExitFrame(int arg_stack_space = 0, bool save_doubles = false,
                      StackFrame::Type frame_type = StackFrame::EXIT);

  // Enter specific kind of exit frame. Allocates
  // (arg_stack_space * kSystemPointerSize) memory (not GCed) on the stack
  // accessible via StackSpaceOperand.
  void EnterApiExitFrame(int arg_stack_space);

  // Leave the current exit frame. Expects/provides the return value in
  // register rax:rdx (untouched) and the pointer to the first
  // argument in register rsi (if pop_arguments == true).
  void LeaveExitFrame(bool save_doubles = false, bool pop_arguments = true);

  // Leave the current exit frame. Expects/provides the return value in
  // register rax (untouched).
  void LeaveApiExitFrame();

  // ---------------------------------------------------------------------------
  // JavaScript invokes

  // Invoke the JavaScript function code by either calling or jumping.
  void InvokeFunctionCode(Register function, Register new_target,
                          Register expected_parameter_count,
                          Register actual_parameter_count, InvokeFlag flag);

  // On function call, call into the debugger.
  void CallDebugOnFunctionCall(Register fun, Register new_target,
                               Register expected_parameter_count,
                               Register actual_parameter_count);

  // Invoke the JavaScript function in the given register. Changes the
  // current context to the context in the function before invoking.
  void InvokeFunction(Register function, Register new_target,
                      Register actual_parameter_count, InvokeFlag flag);

  void InvokeFunction(Register function, Register new_target,
                      Register expected_parameter_count,
                      Register actual_parameter_count, InvokeFlag flag);

  // ---------------------------------------------------------------------------
  // Conversions between tagged smi values and non-tagged integer values.

  // Tag an word-size value. The result must be known to be a valid smi value.
  void SmiTag(Register reg);
  // Requires dst != src
  void SmiTag(Register dst, Register src);

  // Simple comparison of smis.  Both sides must be known smis to use these,
  // otherwise use Cmp.
  void SmiCompare(Register smi1, Register smi2);
  void SmiCompare(Register dst, Smi src);
  void SmiCompare(Register dst, Operand src);
  void SmiCompare(Operand dst, Register src);
  void SmiCompare(Operand dst, Smi src);

  // Functions performing a check on a known or potential smi. Returns
  // a condition that is satisfied if the check is successful.

  // Test-and-jump functions. Typically combines a check function
  // above with a conditional jump.

  // Jump to label if the value is not a tagged smi.
  void JumpIfNotSmi(Register src, Label* on_not_smi,
                    Label::Distance near_jump = Label::kFar);

  // Jump to label if the value is not a tagged smi.
  void JumpIfNotSmi(Operand src, Label* on_not_smi,
                    Label::Distance near_jump = Label::kFar);

  // Operations on tagged smi values.

  // Smis represent a subset of integers. The subset is always equivalent to
  // a two's complement interpretation of a fixed number of bits.

  // Add an integer constant to a tagged smi, giving a tagged smi as result.
  // No overflow testing on the result is done.
  void SmiAddConstant(Operand dst, Smi constant);

  // Specialized operations

  // Converts, if necessary, a smi to a combination of number and
  // multiplier to be used as a scaled index.
  // The src register contains a *positive* smi value. The shift is the
  // power of two to multiply the index value by (e.g. to index by
  // smi-value * kSystemPointerSize, pass the smi and kSystemPointerSizeLog2).
  // The returned index register may be either src or dst, depending
  // on what is most efficient. If src and dst are different registers,
  // src is always unchanged.
  SmiIndex SmiToIndex(Register dst, Register src, int shift);

  // ---------------------------------------------------------------------------
  // Macro instructions.

  void Cmp(Register dst, Handle<Object> source);
  void Cmp(Operand dst, Handle<Object> source);
  void Cmp(Register dst, Smi src);
  void Cmp(Operand dst, Smi src);
  void Cmp(Register dst, int32_t src);

  // Checks if value is in range [lower_limit, higher_limit] using a single
  // comparison.
  void JumpIfIsInRange(Register value, unsigned lower_limit,
                       unsigned higher_limit, Label* on_in_range,
                       Label::Distance near_jump = Label::kFar);

  // Emit code to discard a non-negative number of pointer-sized elements
  // from the stack, clobbering only the rsp register.
  void Drop(int stack_elements);
  // Emit code to discard a positive number of pointer-sized elements
  // from the stack under the return address which remains on the top,
  // clobbering the rsp register.
  void DropUnderReturnAddress(int stack_elements,
                              Register scratch = kScratchRegister);

  void PushQuad(Operand src);
  void PushImm32(int32_t imm32);
  void Pop(Register dst);
  void Pop(Operand dst);
  void PopQuad(Operand dst);

  // ---------------------------------------------------------------------------
  // SIMD macros.
  void Absps(XMMRegister dst);
  void Negps(XMMRegister dst);
  void Abspd(XMMRegister dst);
  void Negpd(XMMRegister dst);
  // Generates a trampoline to jump to the off-heap instruction stream.
  void JumpToInstructionStream(Address entry);

  // Compare object type for heap object.
  // Always use unsigned comparisons: above and below, not less and greater.
  // Incoming register is heap_object and outgoing register is map.
  // They may be the same register, and may be kScratchRegister.
  void CmpObjectType(Register heap_object, InstanceType type, Register map);

  // Compare instance type for map.
  // Always use unsigned comparisons: above and below, not less and greater.
  void CmpInstanceType(Register map, InstanceType type);

  template <typename Field>
  void DecodeField(Register reg) {
    static const int shift = Field::kShift;
    static const int mask = Field::kMask >> Field::kShift;
    if (shift != 0) {
      shrq(reg, Immediate(shift));
    }
    andq(reg, Immediate(mask));
  }

  // Abort execution if argument is a smi, enabled via --debug-code.
  void AssertNotSmi(Register object);

  // Abort execution if argument is not a smi, enabled via --debug-code.
  void AssertSmi(Register object);
  void AssertSmi(Operand object);

  // Abort execution if argument is not a Constructor, enabled via --debug-code.
  void AssertConstructor(Register object);

  // Abort execution if argument is not a JSFunction, enabled via --debug-code.
  void AssertFunction(Register object);

  // Abort execution if argument is not a JSBoundFunction,
  // enabled via --debug-code.
  void AssertBoundFunction(Register object);

  // Abort execution if argument is not a JSGeneratorObject (or subclass),
  // enabled via --debug-code.
  void AssertGeneratorObject(Register object);

  // Abort execution if argument is not undefined or an AllocationSite, enabled
  // via --debug-code.
  void AssertUndefinedOrAllocationSite(Register object);

  // ---------------------------------------------------------------------------
  // Exception handling

  // Push a new stack handler and link it into stack handler chain.
  void PushStackHandler();

  // Unlink the stack handler on top of the stack from the stack handler chain.
  void PopStackHandler();

  // ---------------------------------------------------------------------------
  // Support functions.

  // Load the global proxy from the current context.
  void LoadGlobalProxy(Register dst) {
    LoadNativeContextSlot(Context::GLOBAL_PROXY_INDEX, dst);
  }

  // Load the native context slot with the current index.
  void LoadNativeContextSlot(int index, Register dst);

  // ---------------------------------------------------------------------------
  // Runtime calls

  // Call a runtime routine.
  void CallRuntime(const Runtime::Function* f, int num_arguments,
                   SaveFPRegsMode save_doubles = kDontSaveFPRegs);

  // Convenience function: Same as above, but takes the fid instead.
  void CallRuntime(Runtime::FunctionId fid,
                   SaveFPRegsMode save_doubles = kDontSaveFPRegs) {
    const Runtime::Function* function = Runtime::FunctionForId(fid);
    CallRuntime(function, function->nargs, save_doubles);
  }

  // Convenience function: Same as above, but takes the fid instead.
  void CallRuntime(Runtime::FunctionId fid, int num_arguments,
                   SaveFPRegsMode save_doubles = kDontSaveFPRegs) {
    CallRuntime(Runtime::FunctionForId(fid), num_arguments, save_doubles);
  }

  // Convenience function: tail call a runtime routine (jump)
  void TailCallRuntime(Runtime::FunctionId fid);

  // Jump to a runtime routines
  void JumpToExternalReference(const ExternalReference& ext,
                               bool builtin_exit_frame = false);

  // ---------------------------------------------------------------------------
  // StatsCounter support
  void IncrementCounter(StatsCounter* counter, int value);
  void DecrementCounter(StatsCounter* counter, int value);

  // ---------------------------------------------------------------------------
  // Stack limit utilities
  Operand StackLimitAsOperand(StackLimitKind kind);
  void StackOverflowCheck(
      Register num_args, Register scratch, Label* stack_overflow,
      Label::Distance stack_overflow_distance = Label::kFar);

  // ---------------------------------------------------------------------------
  // In-place weak references.
  void LoadWeakValue(Register in_out, Label* target_if_cleared);

  // ---------------------------------------------------------------------------
  // Debugging

  static int SafepointRegisterStackIndex(Register reg) {
    return SafepointRegisterStackIndex(reg.code());
  }

 private:
  // Order general registers are pushed by Pushad.
  // rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r11, r12, r14, r15.
  static const int kSafepointPushRegisterIndices[Register::kNumRegisters];
  static const int kNumSafepointSavedRegisters = 12;

  // Helper functions for generating invokes.
  void InvokePrologue(Register expected_parameter_count,
                      Register actual_parameter_count, Label* done,
                      InvokeFlag flag);

  void EnterExitFramePrologue(bool save_rax, StackFrame::Type frame_type);

  // Allocates arg_stack_space * kSystemPointerSize memory (not GCed) on the
  // stack accessible via StackSpaceOperand.
  void EnterExitFrameEpilogue(int arg_stack_space, bool save_doubles);

  void LeaveExitFrameEpilogue();

  // Compute memory operands for safepoint stack slots.
  static int SafepointRegisterStackIndex(int reg_code) {
    return kNumSafepointRegisters - kSafepointPushRegisterIndices[reg_code] - 1;
  }

  // Needs access to SafepointRegisterStackIndex for compiled frame
  // traversal.
  friend class CommonFrame;

  DISALLOW_IMPLICIT_CONSTRUCTORS(MacroAssembler);
};

// -----------------------------------------------------------------------------
// Static helper functions.

// Generate an Operand for loading a field from an object.
inline Operand FieldOperand(Register object, int offset) {
  return Operand(object, offset - kHeapObjectTag);
}

// Generate an Operand for loading an indexed field from an object.
inline Operand FieldOperand(Register object, Register index, ScaleFactor scale,
                            int offset) {
  return Operand(object, index, scale, offset - kHeapObjectTag);
}

// Provides access to exit frame stack space (not GCed).
inline Operand StackSpaceOperand(int index) {
#ifdef V8_TARGET_OS_WIN
  const int kShaddowSpace = 4;
  return Operand(rsp, (index + kShaddowSpace) * kSystemPointerSize);
#else
  return Operand(rsp, index * kSystemPointerSize);
#endif
}

inline Operand StackOperandForReturnAddress(int32_t disp) {
  return Operand(rsp, disp);
}

#define ACCESS_MASM(masm) masm->

}  // namespace internal
}  // namespace v8

#endif  // V8_CODEGEN_X64_MACRO_ASSEMBLER_X64_H_
