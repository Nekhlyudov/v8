// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/codegen/code-stub-assembler.h"
#include "src/objects/objects.h"

namespace v8 {
namespace internal {

using compiler::Node;

class SharedArrayBufferBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit SharedArrayBufferBuiltinsAssembler(
      compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

 protected:
  using AssemblerFunction = Node* (CodeAssembler::*)(MachineType type,
                                                     Node* base, Node* offset,
                                                     Node* value,
                                                     Node* value_high);
  void ValidateSharedTypedArray(TNode<Object> maybe_array,
                                TNode<Context> context,
                                TNode<Int32T>* out_elements_kind,
                                TNode<RawPtrT>* out_backing_store);

  TNode<UintPtrT> ValidateAtomicAccess(TNode<JSTypedArray> array,
                                       TNode<Object> index,
                                       TNode<Context> context);

  inline void DebugSanityCheckAtomicIndex(TNode<JSTypedArray> array,
                                          TNode<UintPtrT> index);

  void AtomicBinopBuiltinCommon(TNode<Object> maybe_array, TNode<Object> index,
                                TNode<Object> value, TNode<Context> context,
                                AssemblerFunction function,
                                Runtime::FunctionId runtime_function);

  // Create a BigInt from the result of a 64-bit atomic operation, using
  // projections on 32-bit platforms.
  TNode<BigInt> BigIntFromSigned64(Node* signed64);
  TNode<BigInt> BigIntFromUnsigned64(Node* unsigned64);
};

// https://tc39.es/ecma262/#sec-validatesharedintegertypedarray
void SharedArrayBufferBuiltinsAssembler::ValidateSharedTypedArray(
    TNode<Object> maybe_array, TNode<Context> context,
    TNode<Int32T>* out_elements_kind, TNode<RawPtrT>* out_backing_store) {
  Label not_float_or_clamped(this), invalid(this);

  // Fail if it is not a heap object.
  GotoIf(TaggedIsSmi(maybe_array), &invalid);

  // Fail if the array's instance type is not JSTypedArray.
  TNode<Map> map = LoadMap(CAST(maybe_array));
  GotoIfNot(IsJSTypedArrayMap(map), &invalid);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // Fail if the array's JSArrayBuffer is not shared.
  TNode<JSArrayBuffer> array_buffer = LoadJSArrayBufferViewBuffer(array);
  TNode<Uint32T> bitfield = LoadJSArrayBufferBitField(array_buffer);
  GotoIfNot(IsSetWord32<JSArrayBuffer::IsSharedBit>(bitfield), &invalid);

  // Fail if the array's element type is float32, float64 or clamped.
  STATIC_ASSERT(INT8_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(INT16_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(INT32_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(UINT8_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(UINT16_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(UINT32_ELEMENTS < FLOAT32_ELEMENTS);
  TNode<Int32T> elements_kind = LoadMapElementsKind(map);
  GotoIf(Int32LessThan(elements_kind, Int32Constant(FLOAT32_ELEMENTS)),
         &not_float_or_clamped);
  STATIC_ASSERT(BIGINT64_ELEMENTS > UINT8_CLAMPED_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > UINT8_CLAMPED_ELEMENTS);
  Branch(Int32GreaterThan(elements_kind, Int32Constant(UINT8_CLAMPED_ELEMENTS)),
         &not_float_or_clamped, &invalid);

  BIND(&invalid);
  {
    ThrowTypeError(context, MessageTemplate::kNotIntegerSharedTypedArray,
                   maybe_array);
  }

  BIND(&not_float_or_clamped);
  *out_elements_kind = elements_kind;

  TNode<RawPtrT> backing_store = LoadJSArrayBufferBackingStorePtr(array_buffer);
  TNode<UintPtrT> byte_offset = LoadJSArrayBufferViewByteOffset(array);
  *out_backing_store = RawPtrAdd(backing_store, Signed(byte_offset));
}

// https://tc39.github.io/ecma262/#sec-validateatomicaccess
// ValidateAtomicAccess( typedArray, requestIndex )
TNode<UintPtrT> SharedArrayBufferBuiltinsAssembler::ValidateAtomicAccess(
    TNode<JSTypedArray> array, TNode<Object> index, TNode<Context> context) {
  Label done(this), range_error(this);

  TNode<UintPtrT> index_uintptr = ToIndex(context, index, &range_error);

  TNode<UintPtrT> array_length = LoadJSTypedArrayLength(array);
  Branch(UintPtrLessThan(index_uintptr, array_length), &done, &range_error);

  BIND(&range_error);
  ThrowRangeError(context, MessageTemplate::kInvalidAtomicAccessIndex);

  BIND(&done);
  return index_uintptr;
}

void SharedArrayBufferBuiltinsAssembler::DebugSanityCheckAtomicIndex(
    TNode<JSTypedArray> array, TNode<UintPtrT> index) {
  // In Debug mode, we re-validate the index as a sanity check because
  // ToInteger above calls out to JavaScript. A SharedArrayBuffer can't be
  // detached and the TypedArray length can't change either, so skipping this
  // check in Release mode is safe.
  CSA_ASSERT(this, Word32BinaryNot(
                       IsDetachedBuffer(LoadJSArrayBufferViewBuffer(array))));
  CSA_ASSERT(this, UintPtrLessThan(index, LoadJSTypedArrayLength(array)));
}

TNode<BigInt> SharedArrayBufferBuiltinsAssembler::BigIntFromSigned64(
    Node* signed64) {
  if (Is64()) {
    return BigIntFromInt64(UncheckedCast<IntPtrT>(signed64));
  } else {
    TNode<IntPtrT> low = UncheckedCast<IntPtrT>(Projection(0, signed64));
    TNode<IntPtrT> high = UncheckedCast<IntPtrT>(Projection(1, signed64));
    return BigIntFromInt32Pair(low, high);
  }
}

TNode<BigInt> SharedArrayBufferBuiltinsAssembler::BigIntFromUnsigned64(
    Node* unsigned64) {
  if (Is64()) {
    return BigIntFromUint64(UncheckedCast<UintPtrT>(unsigned64));
  } else {
    TNode<UintPtrT> low = UncheckedCast<UintPtrT>(Projection(0, unsigned64));
    TNode<UintPtrT> high = UncheckedCast<UintPtrT>(Projection(1, unsigned64));
    return BigIntFromUint32Pair(low, high);
  }
}

// https://tc39.es/ecma262/#sec-atomicload
TF_BUILTIN(AtomicsLoad, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  ValidateSharedTypedArray(maybe_array, context, &elements_kind,
                           &backing_store);
  TNode<JSTypedArray> array = CAST(maybe_array);

  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), other(this);
  int32_t case_values[] = {
      INT8_ELEMENTS,  UINT8_ELEMENTS,  INT16_ELEMENTS,    UINT16_ELEMENTS,
      INT32_ELEMENTS, UINT32_ELEMENTS, BIGINT64_ELEMENTS, BIGUINT64_ELEMENTS,
  };
  Label* case_labels[] = {&i8, &u8, &i16, &u16, &i32, &u32, &i64, &u64};
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(
      SmiFromInt32(AtomicLoad(MachineType::Int8(), backing_store, index_word)));

  BIND(&u8);
  Return(SmiFromInt32(
      AtomicLoad(MachineType::Uint8(), backing_store, index_word)));

  BIND(&i16);
  Return(SmiFromInt32(
      AtomicLoad(MachineType::Int16(), backing_store, WordShl(index_word, 1))));

  BIND(&u16);
  Return(SmiFromInt32(AtomicLoad(MachineType::Uint16(), backing_store,
                                 WordShl(index_word, 1))));

  BIND(&i32);
  Return(ChangeInt32ToTagged(
      AtomicLoad(MachineType::Int32(), backing_store, WordShl(index_word, 2))));

  BIND(&u32);
  Return(ChangeUint32ToTagged(AtomicLoad(MachineType::Uint32(), backing_store,
                                         WordShl(index_word, 2))));
#if V8_TARGET_ARCH_MIPS && !_MIPS_ARCH_MIPS32R6
  BIND(&i64);
  Goto(&u64);

  BIND(&u64);
  {
    TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
    Return(CallRuntime(Runtime::kAtomicsLoad64, context, array, index_number));
  }
#else
  BIND(&i64);
  // This uses Uint64() intentionally: AtomicLoad is not implemented for
  // Int64(), which is fine because the machine instruction only cares
  // about words.
  Return(BigIntFromSigned64(AtomicLoad(MachineType::Uint64(), backing_store,
                                       WordShl(index_word, 3))));

  BIND(&u64);
  Return(BigIntFromUnsigned64(AtomicLoad(MachineType::Uint64(), backing_store,
                                         WordShl(index_word, 3))));
#endif
  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
}

// https://tc39.es/ecma262/#sec-atomics.store
TF_BUILTIN(AtomicsStore, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Object> value = CAST(Parameter(Descriptor::kValue));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  ValidateSharedTypedArray(maybe_array, context, &elements_kind,
                           &backing_store);
  TNode<JSTypedArray> array = CAST(maybe_array);

  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

  Label u8(this), u16(this), u32(this), u64(this), other(this);
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &u64);

  TNode<Number> value_integer = ToInteger_Inline(context, value);
  TNode<Word32T> value_word32 = TruncateTaggedToWord32(context, value_integer);

  DebugSanityCheckAtomicIndex(array, index_word);

  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {&u8, &u8, &u16, &u16, &u32, &u32};
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&u8);
  AtomicStore(MachineRepresentation::kWord8, backing_store, index_word,
              value_word32);
  Return(value_integer);

  BIND(&u16);
  AtomicStore(MachineRepresentation::kWord16, backing_store,
              WordShl(index_word, 1), value_word32);
  Return(value_integer);

  BIND(&u32);
  AtomicStore(MachineRepresentation::kWord32, backing_store,
              WordShl(index_word, 2), value_word32);
  Return(value_integer);

  BIND(&u64);
#if V8_TARGET_ARCH_MIPS && !_MIPS_ARCH_MIPS32R6
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(Runtime::kAtomicsStore64, context, array, index_number,
                     value));
#else
  TNode<BigInt> value_bigint = ToBigInt(context, value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_low);
  TVARIABLE(UintPtrT, var_high);
  BigIntToRawBytes(value_bigint, &var_low, &var_high);
  TNode<UintPtrT> high = Is64() ? TNode<UintPtrT>() : var_high.value();
  AtomicStore(MachineRepresentation::kWord64, backing_store,
              WordShl(index_word, 3), var_low.value(), high);
  Return(value_bigint);
#endif

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
}

// https://tc39.es/ecma262/#sec-atomics.exchange
TF_BUILTIN(AtomicsExchange, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Object> value = CAST(Parameter(Descriptor::kValue));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  ValidateSharedTypedArray(maybe_array, context, &elements_kind,
                           &backing_store);
  TNode<JSTypedArray> array = CAST(maybe_array);

  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_RISCV64 || \
    V8_TARGET_ARCH_RISCV32
  // FIXME(RISCV): Review this special case once atomics are added
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(Runtime::kAtomicsExchange, context, array, index_number,
                     value));
#else

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), big(this), other(this);
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &big);

  TNode<Number> value_integer = ToInteger_Inline(context, value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TNode<Word32T> value_word32 = TruncateTaggedToWord32(context, value_integer);

  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromInt32(AtomicExchange(MachineType::Int8(), backing_store,
                                     index_word, value_word32)));

  BIND(&u8);
  Return(SmiFromInt32(AtomicExchange(MachineType::Uint8(), backing_store,
                                     index_word, value_word32)));

  BIND(&i16);
  Return(SmiFromInt32(AtomicExchange(MachineType::Int16(), backing_store,
                                     WordShl(index_word, 1), value_word32)));

  BIND(&u16);
  Return(SmiFromInt32(AtomicExchange(MachineType::Uint16(), backing_store,
                                     WordShl(index_word, 1), value_word32)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(AtomicExchange(MachineType::Int32(), backing_store,
                                            WordShl(index_word, 2),
                                            value_word32)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(
      AtomicExchange(MachineType::Uint32(), backing_store,
                     WordShl(index_word, 2), value_word32)));

  BIND(&big);
  TNode<BigInt> value_bigint = ToBigInt(context, value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_low);
  TVARIABLE(UintPtrT, var_high);
  BigIntToRawBytes(value_bigint, &var_low, &var_high);
  TNode<UintPtrT> high = Is64() ? TNode<UintPtrT>() : var_high.value();
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGINT64_ELEMENTS)), &i64);
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGUINT64_ELEMENTS)), &u64);
  Unreachable();

  BIND(&i64);
  // This uses Uint64() intentionally: AtomicExchange is not implemented for
  // Int64(), which is fine because the machine instruction only cares
  // about words.
  Return(BigIntFromSigned64(AtomicExchange(MachineType::Uint64(), backing_store,
                                           WordShl(index_word, 3),
                                           var_low.value(), high)));

  BIND(&u64);
  Return(BigIntFromUnsigned64(
      AtomicExchange(MachineType::Uint64(), backing_store,
                     WordShl(index_word, 3), var_low.value(), high)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 ||
        // V8_TARGET_ARCH_RISCV64 || V8_TARGET_ARCH_RISCV32
}

// https://tc39.es/ecma262/#sec-atomics.compareexchange
TF_BUILTIN(AtomicsCompareExchange, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Object> old_value = CAST(Parameter(Descriptor::kOldValue));
  TNode<Object> new_value = CAST(Parameter(Descriptor::kNewValue));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  ValidateSharedTypedArray(maybe_array, context, &elements_kind,
                           &backing_store);
  TNode<JSTypedArray> array = CAST(maybe_array);

  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64 || \
    V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X ||    \
    V8_TARGET_ARCH_RISCV64 || V8_TARGET_ARCH_RISCV32
  // FIXME(RISCV): Review this special case once atomics are added
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(Runtime::kAtomicsCompareExchange, context, array,
                     index_number, old_value, new_value));
#else
  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), big(this), other(this);
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &big);

  TNode<Number> old_value_integer = ToInteger_Inline(context, old_value);
  TNode<Number> new_value_integer = ToInteger_Inline(context, new_value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TNode<Word32T> old_value_word32 =
      TruncateTaggedToWord32(context, old_value_integer);
  TNode<Word32T> new_value_word32 =
      TruncateTaggedToWord32(context, new_value_integer);

  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromInt32(AtomicCompareExchange(MachineType::Int8(), backing_store,
                                            index_word, old_value_word32,
                                            new_value_word32)));

  BIND(&u8);
  Return(SmiFromInt32(AtomicCompareExchange(MachineType::Uint8(), backing_store,
                                            index_word, old_value_word32,
                                            new_value_word32)));

  BIND(&i16);
  Return(SmiFromInt32(AtomicCompareExchange(
      MachineType::Int16(), backing_store, WordShl(index_word, 1),
      old_value_word32, new_value_word32)));

  BIND(&u16);
  Return(SmiFromInt32(AtomicCompareExchange(
      MachineType::Uint16(), backing_store, WordShl(index_word, 1),
      old_value_word32, new_value_word32)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(AtomicCompareExchange(
      MachineType::Int32(), backing_store, WordShl(index_word, 2),
      old_value_word32, new_value_word32)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(AtomicCompareExchange(
      MachineType::Uint32(), backing_store, WordShl(index_word, 2),
      old_value_word32, new_value_word32)));

  BIND(&big);
  TNode<BigInt> old_value_bigint = ToBigInt(context, old_value);
  TNode<BigInt> new_value_bigint = ToBigInt(context, new_value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_old_low);
  TVARIABLE(UintPtrT, var_old_high);
  TVARIABLE(UintPtrT, var_new_low);
  TVARIABLE(UintPtrT, var_new_high);
  BigIntToRawBytes(old_value_bigint, &var_old_low, &var_old_high);
  BigIntToRawBytes(new_value_bigint, &var_new_low, &var_new_high);
  TNode<UintPtrT> old_high = Is64() ? TNode<UintPtrT>() : var_old_high.value();
  TNode<UintPtrT> new_high = Is64() ? TNode<UintPtrT>() : var_new_high.value();
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGINT64_ELEMENTS)), &i64);
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGUINT64_ELEMENTS)), &u64);
  Unreachable();

  BIND(&i64);
  // This uses Uint64() intentionally: AtomicCompareExchange is not implemented
  // for Int64(), which is fine because the machine instruction only cares
  // about words.
  Return(BigIntFromSigned64(AtomicCompareExchange(
      MachineType::Uint64(), backing_store, WordShl(index_word, 3),
      var_old_low.value(), var_new_low.value(), old_high, new_high)));

  BIND(&u64);
  Return(BigIntFromUnsigned64(AtomicCompareExchange(
      MachineType::Uint64(), backing_store, WordShl(index_word, 3),
      var_old_low.value(), var_new_low.value(), old_high, new_high)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
        // || V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
        // || V8_TARGET_ARCH_RISCV64 || V8_TARGET_ARCH_RISCV32
}

#define BINOP_BUILTIN(op)                                           \
  TF_BUILTIN(Atomics##op, SharedArrayBufferBuiltinsAssembler) {     \
    TNode<Object> array = CAST(Parameter(Descriptor::kArray));      \
    TNode<Object> index = CAST(Parameter(Descriptor::kIndex));      \
    TNode<Object> value = CAST(Parameter(Descriptor::kValue));      \
    TNode<Context> context = CAST(Parameter(Descriptor::kContext)); \
    AtomicBinopBuiltinCommon(array, index, value, context,          \
                             &CodeAssembler::Atomic##op,            \
                             Runtime::kAtomics##op);                \
  }
// https://tc39.es/ecma262/#sec-atomics.add
BINOP_BUILTIN(Add)
// https://tc39.es/ecma262/#sec-atomics.sub
BINOP_BUILTIN(Sub)
// https://tc39.es/ecma262/#sec-atomics.and
BINOP_BUILTIN(And)
// https://tc39.es/ecma262/#sec-atomics.or
BINOP_BUILTIN(Or)
// https://tc39.es/ecma262/#sec-atomics.xor
BINOP_BUILTIN(Xor)
#undef BINOP_BUILTIN

// https://tc39.es/ecma262/#sec-atomicreadmodifywrite
void SharedArrayBufferBuiltinsAssembler::AtomicBinopBuiltinCommon(
    TNode<Object> maybe_array, TNode<Object> index, TNode<Object> value,
    TNode<Context> context, AssemblerFunction function,
    Runtime::FunctionId runtime_function) {
  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  ValidateSharedTypedArray(maybe_array, context, &elements_kind,
                           &backing_store);
  TNode<JSTypedArray> array = CAST(maybe_array);

  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64 || \
    V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X ||    \
    V8_TARGET_ARCH_RISCV64 || V8_TARGET_ARCH_RISCV32
  // FIXME(RISCV): Review this special case once atomics are added
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(runtime_function, context, array, index_number, value));
#else
  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), big(this), other(this);

  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &big);

  TNode<Number> value_integer = ToInteger_Inline(context, value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TNode<Word32T> value_word32 = TruncateTaggedToWord32(context, value_integer);

  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromInt32((this->*function)(MachineType::Int8(), backing_store,
                                        index_word, value_word32, nullptr)));

  BIND(&u8);
  Return(SmiFromInt32((this->*function)(MachineType::Uint8(), backing_store,
                                        index_word, value_word32, nullptr)));

  BIND(&i16);
  Return(SmiFromInt32((this->*function)(MachineType::Int16(), backing_store,
                                        WordShl(index_word, 1), value_word32,
                                        nullptr)));

  BIND(&u16);
  Return(SmiFromInt32((this->*function)(MachineType::Uint16(), backing_store,
                                        WordShl(index_word, 1), value_word32,
                                        nullptr)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(
      (this->*function)(MachineType::Int32(), backing_store,
                        WordShl(index_word, 2), value_word32, nullptr)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(
      (this->*function)(MachineType::Uint32(), backing_store,
                        WordShl(index_word, 2), value_word32, nullptr)));

  BIND(&big);
  TNode<BigInt> value_bigint = ToBigInt(context, value);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_low);
  TVARIABLE(UintPtrT, var_high);
  BigIntToRawBytes(value_bigint, &var_low, &var_high);
  TNode<UintPtrT> high = Is64() ? TNode<UintPtrT>() : var_high.value();
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGINT64_ELEMENTS)), &i64);
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGUINT64_ELEMENTS)), &u64);
  Unreachable();

  BIND(&i64);
  // This uses Uint64() intentionally: Atomic* ops are not implemented for
  // Int64(), which is fine because the machine instructions only care
  // about words.
  Return(BigIntFromSigned64(
      (this->*function)(MachineType::Uint64(), backing_store,
                        WordShl(index_word, 3), var_low.value(), high)));

  BIND(&u64);
  Return(BigIntFromUnsigned64(
      (this->*function)(MachineType::Uint64(), backing_store,
                        WordShl(index_word, 3), var_low.value(), high)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
        // || V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
        // || V8_TARGET_ARCH_RISCV64 || V8_TARGET_ARCH_RISCV32
}

}  // namespace internal
}  // namespace v8
