#pragma once

#include "common_utils.h"
#include "memory_ptr.h"
#include "vm.h"
#include "vm_string.h"
#include "structure.h"
#include "table_object.h"

namespace ToyLang {

using namespace CommonUtils;

class IRNode
{
public:
    virtual ~IRNode() { }

};

class IRLogicalVariable
{
public:

};

class IRBasicBlock
{
public:
    std::vector<IRNode*> m_nodes;
    std::vector<IRNode*> m_varAtHead;
    std::vector<IRNode*> m_varAvailableAtTail;
};

class IRConstant : public IRNode
{
public:

};

class IRGetLocal : public IRNode
{
public:
    int m_slot;
    IRLogicalVariable* m_vinfo;
};

class IRSetLocal : public IRNode
{
public:
    IRNode* m_value;
    int m_slot;
    IRLogicalVariable* m_vinfo;
};

class IRAdd : public IRNode
{
public:
    IRNode* m_lhs;
    IRNode* m_rhs;
};

class IRReturn : public IRNode
{
public:
    IRNode* m_value;
};

class IRCheckIsConstant : public IRNode
{
public:
    IRNode* m_value;
    TValue m_constant;
};

class BytecodeSlot
{
public:
    constexpr BytecodeSlot() : m_value(x_invalidValue) { }

    static constexpr BytecodeSlot WARN_UNUSED Local(int ord)
    {
        assert(ord >= 0);
        return BytecodeSlot(ord);
    }
    static constexpr BytecodeSlot WARN_UNUSED Constant(int ord)
    {
        assert(ord < 0);
        return BytecodeSlot(ord);
    }

    bool IsInvalid() const { return m_value == x_invalidValue; }
    bool IsLocal() const { assert(!IsInvalid()); return m_value >= 0; }
    bool IsConstant() const { assert(!IsInvalid()); return m_value < 0; }

    int WARN_UNUSED LocalOrd() const { assert(IsLocal()); return m_value; }
    int WARN_UNUSED ConstantOrd() const { assert(IsConstant()); return m_value; }

    explicit operator int() const { return m_value; }

private:
    constexpr BytecodeSlot(int value) : m_value(value) { }

    static constexpr int x_invalidValue = 0x7fffffff;
    int m_value;
};

class alignas(64) CoroutineRuntimeContext
{
public:
    // The constant table of the current function, if interpreter
    //
    TValue* m_constants;

    // The global object, if interpreter
    //
    UserHeapPointer<TableObject> m_globalObject;

    // slot [m_variadicRetSlotBegin + ord] holds variadic return value 'ord'
    //
    uint32_t m_numVariadicRets;
    uint32_t m_variadicRetSlotBegin;

    // The stack object
    //
    uint64_t* m_stackObject;


};

using InterpreterFn = void(*)(CoroutineRuntimeContext* /*rc*/, RestrictPtr<void> /*stackframe*/, ConstRestrictPtr<uint8_t> /*instr*/, uint64_t /*unused*/);

// Base class for some executable, either an intrinsic, or a bytecode function with some fixed global object, or a user C function
//
class ExecutableCode : public SystemHeapGcObjectHeader
{
public:
    bool IsIntrinsic() const { return m_bytecode == nullptr; }
    bool IsUserCFunction() const { return reinterpret_cast<intptr_t>(m_bytecode) < 0; }
    bool IsBytecodeFunction() const { return reinterpret_cast<intptr_t>(m_bytecode) > 0; }

    using UserCFunctionPrototype = int(*)(void*);

    UserCFunctionPrototype GetCFunctionPtr() const
    {
        assert(IsUserCFunction());
        return reinterpret_cast<UserCFunctionPrototype>(~reinterpret_cast<uintptr_t>(m_bytecode));
    }

    uint8_t m_reserved;

    // The # of fixed arguments and whether it accepts variadic arguments
    // User C function always have m_numFixedArguments == 0 and m_hasVariadicArguments == true
    //
    bool m_hasVariadicArguments;
    uint32_t m_numFixedArguments;

    // This is nullptr iff it is an intrinsic, and negative iff it is a user-provided C function
    //
    uint8_t* m_bytecode;

    // For intrinsic, this is the entrypoint of the intrinsic function
    // For bytecode function, this is the most optimized implementation (interpreter or some JIT tier)
    // For user C function, this is a trampoline that calls the function
    // The 'codeBlock' parameter and 'curBytecode' parameter is not needed for intrinsic or JIT but we have them anyway for a unified interface
    //
    InterpreterFn m_bestEntryPoint;
};
static_assert(sizeof(ExecutableCode) == 24);

class BaselineCodeBlock;
class FLOCodeBlock;

class FunctionExecutable;

// This uniquely corresponds to each pair of <FunctionExecutable, GlobalObject>
// It owns the bytecode and the corresponding metadata (the bytecode is copied from the FunctionExecutable,
// we need our own copy because we do bytecode opcode specialization optimization)
//
class CodeBlock final : public ExecutableCode
{
public:
    UserHeapPointer<TableObject> m_globalObject;

    uint32_t m_stackFrameNumSlots;
    uint32_t m_numUpValues;
    uint32_t m_bytecodeLength;
    uint32_t m_bytecodeMetadataLength;

    BaselineCodeBlock* m_baselineCodeBlock;
    FLOCodeBlock* m_floCodeBlock;

    FunctionExecutable* m_owner;

    uint64_t m_bytecodeMetadata[0];

    static constexpr size_t x_trailingArrayOffset = offsetof_member_v<&CodeBlock::m_bytecodeMetadata>;
};

// This uniquely corresponds to a piece of source code that defines a function
//
class FunctionExecutable
{
public:
    template<typename T, typename = std::enable_if_t<IsPtrOrHeapPtr<T, FunctionExecutable>>>
    static CodeBlock* ALWAYS_INLINE GetCodeBlock(T self, GeneralHeapPointer<void> globalObject)
    {
        if (likely(globalObject == self->m_defaultGlobalObject))
        {
            return self->m_defaultCodeBlock;
        }
        assert(self->m_rareGOtoCBMap != nullptr);
        RareGlobalObjectToCodeBlockMap* rareMap = self->m_rareGOtoCBMap;
        auto iter = rareMap->find(globalObject.m_value);
        assert(iter != rareMap->end());
        return iter->second;
    }

    uint8_t* m_bytecode;
    uint32_t m_bytecodeLength;
    GeneralHeapPointer<void> m_defaultGlobalObject;
    CodeBlock* m_defaultCodeBlock;
    using RareGlobalObjectToCodeBlockMap = std::unordered_map<int32_t, CodeBlock*>;
    RareGlobalObjectToCodeBlockMap* m_rareGOtoCBMap;

    uint32_t m_numUpValues;
    uint32_t m_bytecodeMetadataLength;
    uint32_t m_stackFrameNumSlots;


};

class FunctionObject
{
public:
    // Object header
    //
    // Note that a CodeBlock defines both FunctionExecutable and GlobalObject,
    // so the upValue list does not contain the global object (if the ExecutableCode is not a CodeBlock, then the global object doesn't matter either)
    //
    SystemHeapPointer<ExecutableCode> m_executable;
    Type m_type;
    GcCellState m_cellState;

    uint16_t m_reserved;

    TValue m_upValues[0];
};
static_assert(sizeof(FunctionObject) == 8);

// stack frame format:
//     [... VarArgs ...] [Header] [... Locals ...]
//                                ^
//                                stack frame pointer (sfp)
//
class alignas(8) StackFrameHeader
{
public:
    // The address of the caller stack frame
    //
    StackFrameHeader* m_caller;
    // The return address
    //
    void* m_retAddr;
    // The function corresponding to this stack frame
    //
    HeapPtr<FunctionObject> m_func;
    // If the function is calling (i.e. not topmost frame), denotes the offset of the bytecode that performed the call
    //
    uint32_t m_callerBytecodeOffset;
    // Total number of variadic arguments passed to the function
    //
    uint32_t m_numVariadicArguments;

    static StackFrameHeader* GetStackFrameHeader(void* sfp)
    {
        return reinterpret_cast<StackFrameHeader*>(sfp) - 1;
    }

    static TValue* GetLocalAddr(void* sfp, BytecodeSlot slot)
    {
        assert(slot.IsLocal());
        int ord = slot.LocalOrd();
        return reinterpret_cast<TValue*>(sfp) + ord;
    }

    static TValue GetLocal(void* sfp, BytecodeSlot slot)
    {
        return *GetLocalAddr(sfp, slot);
    }
};

static_assert(sizeof(StackFrameHeader) % sizeof(TValue) == 0);
static constexpr size_t x_sizeOfStackFrameHeaderInTermsOfTValue = sizeof(StackFrameHeader) / sizeof(TValue);

// The varg part of each inlined function can always
// be represented as a list of locals plus a suffix of the original function's varg
//
class InlinedFunctionVarArgRepresentation
{
public:
    // The prefix ordinals
    //
    std::vector<int> m_prefix;
    // The suffix of the original function's varg beginning at that ordinal (inclusive)
    //
    int m_suffix;
};

class InliningStackEntry
{
public:
    // The base ordinal of stack frame header
    //
    int m_baseOrd;
    // Number of fixed arguments for this function
    //
    int m_numArguments;
    // Number of locals for this function
    //
    int m_numLocals;
    // Varargs of this function
    //
    InlinedFunctionVarArgRepresentation m_varargs;

};

class BytecodeToIRTransformer
{
public:
    // Remap a slot in bytecode to the physical slot for the interpreter/baseline JIT
    //
    void RemapSlot(BytecodeSlot /*slot*/)
    {

    }

    void TransformFunctionImpl(IRBasicBlock* /*bb*/)
    {

    }

    std::vector<InliningStackEntry> m_inlineStack;
};

#define OPCODE_LIST     \
    BcTableGetById,     \
    BcTablePutById,     \
    BcTableGetByVal,    \
    BcTablePutByVal,    \
    BcGlobalGet,        \
    BcGlobalPut,        \
    BcReturn,           \
    BcCall,             \
    BcAddVV,            \
    BcSubVV,            \
    BcIsLTVV,           \
    BcConstant

#define macro(opcodeCppName) class opcodeCppName;
PP_FOR_EACH(macro, OPCODE_LIST)
#undef macro

#define macro(opcodeCppName) + 1
constexpr size_t x_numOpcodes = 0 PP_FOR_EACH(macro, OPCODE_LIST);
#undef macro

namespace internal
{

template<typename T>
struct opcode_for_type_impl;

#define macro(ord, opcodeCppName) template<> struct opcode_for_type_impl<opcodeCppName> { static constexpr uint8_t value = ord; };
PP_FOR_EACH_UNPACK_TUPLE(macro, PP_ZIP_TWO_LISTS((PP_NATURAL_NUMBERS_LIST), (OPCODE_LIST)))
#undef macro

}   // namespace internal

template<typename T>
constexpr uint8_t x_opcodeId = internal::opcode_for_type_impl<T>::value;

extern const InterpreterFn x_interpreter_dispatches[x_numOpcodes];

#define Dispatch(rc, stackframe, instr)                                                                                          \
    do {                                                                                                                         \
        uint8_t dispatch_nextopcode = *reinterpret_cast<const uint8_t*>(instr);                                                  \
        assert(dispatch_nextopcode < x_numOpcodes);                                                                              \
_Pragma("clang diagnostic push")                                                                                                 \
_Pragma("clang diagnostic ignored \"-Wuninitialized\"")                                                                          \
        uint64_t dispatch_unused;                                                                                                \
        [[clang::musttail]] return x_interpreter_dispatches[dispatch_nextopcode]((rc), (stackframe), (instr), dispatch_unused);  \
_Pragma("clang diagnostic pop")                                                                                                  \
    } while (false)

inline void EnterInterpreter(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
{
    Dispatch(rc, sfp, bcu);
}

// The return statement is required to fill nil up to x_minNilFillReturnValues values even if it returns less than that many values
//
constexpr uint32_t x_minNilFillReturnValues = 3;

class BcTableGetById
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_base;
    BytecodeSlot m_dst;
    uint32_t m_index;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcTableGetById* bc = reinterpret_cast<const BcTableGetById*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcTableGetById>);
        assert(bc->m_base.IsLocal());
        TValue tvbase = *StackFrameHeader::GetLocalAddr(sfp, bc->m_base);

        assert(rc->m_constants[bc->m_index].IsPointer(TValue::x_mivTag));
        UserHeapPointer<HeapString> index = rc->m_constants[bc->m_index].AsPointer<HeapString>();

        if (!tvbase.IsPointer(TValue::x_mivTag))
        {
            ReleaseAssert(false && "unimplemented");
        }
        else
        {
            UserHeapPointer<void> base = tvbase.AsPointer<void>();
            if (base.As<UserHeapGcObjectHeader>()->m_type != Type::TABLE)
            {
                ReleaseAssert(false && "unimplemented");
            }
            GetByIdICInfo icInfo;
            TableObject::PrepareGetById(base.As<TableObject>(), index, icInfo /*out*/);
            TValue result = TableObject::GetById(base.As<TableObject>(), index.As<void>(), icInfo);

            *StackFrameHeader::GetLocalAddr(sfp, bc->m_dst) = result;
            Dispatch(rc, sfp, bcu + sizeof(BcTableGetById));
        }
    }
} __attribute__((__packed__));

class BcTablePutById
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_base;
    BytecodeSlot m_src;
    uint32_t m_index;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcTablePutById* bc = reinterpret_cast<const BcTablePutById*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcTablePutById>);
        assert(bc->m_base.IsLocal());
        TValue tvbase = *StackFrameHeader::GetLocalAddr(sfp, bc->m_base);

        assert(rc->m_constants[bc->m_index].IsPointer(TValue::x_mivTag));
        UserHeapPointer<HeapString> index = rc->m_constants[bc->m_index].AsPointer<HeapString>();

        if (!tvbase.IsPointer(TValue::x_mivTag))
        {
            ReleaseAssert(false && "unimplemented");
        }
        else
        {
            UserHeapPointer<void> base = tvbase.AsPointer<void>();
            if (base.As<UserHeapGcObjectHeader>()->m_type != Type::TABLE)
            {
                ReleaseAssert(false && "unimplemented");
            }
            PutByIdICInfo icInfo;
            TableObject::PreparePutById(base.As<TableObject>(), index, icInfo /*out*/);
            TValue newValue = *StackFrameHeader::GetLocalAddr(sfp, bc->m_src);
            TableObject::PutById(base.As<TableObject>(), index.As<void>(), newValue, icInfo);
            Dispatch(rc, sfp, bcu + sizeof(BcTablePutById));
        }
    }
} __attribute__((__packed__));

class BcTableGetByVal
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_base;
    BytecodeSlot m_index;
    BytecodeSlot m_dst;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcTableGetByVal* bc = reinterpret_cast<const BcTableGetByVal*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcTableGetByVal>);
        assert(bc->m_base.IsLocal());
        TValue tvbase = *StackFrameHeader::GetLocalAddr(sfp, bc->m_base);

        if (!tvbase.IsPointer(TValue::x_mivTag))
        {
            ReleaseAssert(false && "unimplemented");
        }
        else
        {
            UserHeapPointer<void> base = tvbase.AsPointer<void>();
            if (base.As<UserHeapGcObjectHeader>()->m_type != Type::TABLE)
            {
                ReleaseAssert(false && "unimplemented");
            }

            TValue index = *StackFrameHeader::GetLocalAddr(sfp, bc->m_index);
            TValue result;
            if (index.IsInt32(TValue::x_int32Tag))
            {
                GetByIntegerIndexICInfo icInfo;
                TableObject::PrepareGetByIntegerIndex(base.As<TableObject>(), icInfo /*out*/);
                result = TableObject::GetByIntegerIndex(base.As<TableObject>(), index.AsInt32(), icInfo);
            }
            else if (index.IsDouble(TValue::x_int32Tag))
            {
                GetByIntegerIndexICInfo icInfo;
                TableObject::PrepareGetByIntegerIndex(base.As<TableObject>(), icInfo /*out*/);
                result = TableObject::GetByDoubleVal(base.As<TableObject>(), index.AsDouble(), icInfo);
            }
            else if (index.IsPointer(TValue::x_mivTag))
            {
                GetByIdICInfo icInfo;
                TableObject::PrepareGetById(base.As<TableObject>(), index.AsPointer(), icInfo /*out*/);
                result = TableObject::GetById(base.As<TableObject>(), index.AsPointer(), icInfo);
            }
            else
            {
                ReleaseAssert(false && "unimplemented");
            }

            *StackFrameHeader::GetLocalAddr(sfp, bc->m_dst) = result;
            Dispatch(rc, sfp, bcu + sizeof(BcTableGetByVal));
        }
    }
} __attribute__((__packed__));

class BcTablePutByVal
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_base;
    BytecodeSlot m_index;
    BytecodeSlot m_src;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcTablePutByVal* bc = reinterpret_cast<const BcTablePutByVal*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcTablePutByVal>);
        assert(bc->m_base.IsLocal());
        TValue tvbase = *StackFrameHeader::GetLocalAddr(sfp, bc->m_base);

        if (!tvbase.IsPointer(TValue::x_mivTag))
        {
            ReleaseAssert(false && "unimplemented");
        }
        else
        {
            UserHeapPointer<void> base = tvbase.AsPointer<void>();
            if (base.As<UserHeapGcObjectHeader>()->m_type != Type::TABLE)
            {
                ReleaseAssert(false && "unimplemented");
            }

            TValue index = *StackFrameHeader::GetLocalAddr(sfp, bc->m_index);
            TValue newValue = *StackFrameHeader::GetLocalAddr(sfp, bc->m_src);
            if (index.IsInt32(TValue::x_int32Tag))
            {
                TableObject::PutByValIntegerIndex(base.As<TableObject>(), index.AsInt32(), newValue);
            }
            else if (index.IsDouble(TValue::x_int32Tag))
            {
                TableObject::PutByValDoubleIndex(base.As<TableObject>(), index.AsDouble(), newValue);
            }
            else if (index.IsPointer(TValue::x_mivTag))
            {
                PutByIdICInfo icInfo;
                TableObject::PreparePutById(base.As<TableObject>(), index.AsPointer(), icInfo /*out*/);
                TableObject::PutById(base.As<TableObject>(), index.AsPointer(), newValue, icInfo);
            }
            else
            {
                ReleaseAssert(false && "unimplemented");
            }

            Dispatch(rc, sfp, bcu + sizeof(BcTablePutByVal));
        }
    }
} __attribute__((__packed__));

class BcGlobalGet
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_dst;
    uint32_t m_index;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcGlobalGet* bc = reinterpret_cast<const BcGlobalGet*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcGlobalGet>);

        assert(rc->m_constants[bc->m_index].IsPointer(TValue::x_mivTag));
        UserHeapPointer<HeapString> index = rc->m_constants[bc->m_index].AsPointer<HeapString>();

        UserHeapPointer<TableObject> base = rc->m_globalObject;
        GetByIdICInfo icInfo;
        TableObject::PrepareGetById(base.As<TableObject>(), index, icInfo /*out*/);
        TValue result = TableObject::GetById(base.As(), index.As<void>(), icInfo);

        *StackFrameHeader::GetLocalAddr(sfp, bc->m_dst) = result;
        Dispatch(rc, sfp, bcu + sizeof(BcGlobalGet));
    }
} __attribute__((__packed__));

class BcGlobalPut
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_src;
    uint32_t m_index;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcGlobalPut* bc = reinterpret_cast<const BcGlobalPut*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcGlobalPut>);

        assert(rc->m_constants[bc->m_index].IsPointer(TValue::x_mivTag));
        UserHeapPointer<HeapString> index = rc->m_constants[bc->m_index].AsPointer<HeapString>();
        TValue newValue = *StackFrameHeader::GetLocalAddr(sfp, bc->m_src);

        UserHeapPointer<TableObject> base = rc->m_globalObject;
        PutByIdICInfo icInfo;
        TableObject::PreparePutById(base.As<TableObject>(), index, icInfo /*out*/);
        TableObject::PutById(base.As(), index.As<void>(), newValue, icInfo);

        Dispatch(rc, sfp, bcu + sizeof(BcGlobalPut));
    }
} __attribute__((__packed__));


class BcReturn
{
public:
    uint8_t m_opcode;
    bool m_isVariadicRet;
    uint16_t m_numReturnValues;
    BytecodeSlot m_slotBegin;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcReturn* bc = reinterpret_cast<const BcReturn*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcReturn>);
        assert(bc->m_slotBegin.IsLocal());
        TValue* pbegin = StackFrameHeader::GetLocalAddr(sfp, bc->m_slotBegin);
        uint32_t numRetValues = bc->m_numReturnValues;
        if (bc->m_isVariadicRet)
        {
            assert(rc->m_numVariadicRets != static_cast<uint32_t>(-1));
            TValue* pdst = pbegin + bc->m_numReturnValues;
            TValue* psrc = reinterpret_cast<TValue*>(sfp) + rc->m_variadicRetSlotBegin;
            numRetValues += rc->m_numVariadicRets;
            SafeMemcpy(pdst, psrc, sizeof(TValue) * rc->m_numVariadicRets);
        }
        // No matter we consumed variadic ret or not, it is no longer valid after the return
        //
        DEBUG_ONLY(rc->m_numVariadicRets = static_cast<uint32_t>(-1);)

        // Fill nil up to x_minNilFillReturnValues values
        // TODO: we can also just do a vectorized write
        //
        {
            uint32_t idx = numRetValues;
            while (idx < x_minNilFillReturnValues)
            {
                pbegin[idx] = TValue::CreateMIV(MiscImmediateValue::CreateNil(), TValue::x_mivTag);
                idx++;
            }
        }

        StackFrameHeader* hdr = StackFrameHeader::GetStackFrameHeader(sfp);
        using RetFn = void(*)(CoroutineRuntimeContext* /*rc*/, void* /*sfp*/, uint8_t* /*retValuesStart*/, uint64_t /*numRetValues*/);
        RetFn retAddr = reinterpret_cast<RetFn>(hdr->m_retAddr);
        StackFrameHeader* callerSf = hdr->m_caller;
        [[clang::musttail]] return retAddr(rc, static_cast<void*>(callerSf), reinterpret_cast<uint8_t*>(pbegin), numRetValues);
    }
} __attribute__((__packed__));

class BcCall
{
public:
    uint8_t m_opcode;
    bool m_keepVariadicRet;
    bool m_passVariadicRetAsParam;
    uint32_t m_numFixedParams;
    uint32_t m_numFixedRets;    // only used when m_keepVariadicRet == false
    BytecodeSlot m_funcSlot;   // params are [m_funcSlot + 1, ... m_funcSlot + m_numFixedParams]

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> sfp, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcCall* bc = reinterpret_cast<const BcCall*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcCall>);
        StackFrameHeader* hdr = StackFrameHeader::GetStackFrameHeader(sfp);

        HeapPtr<ExecutableCode> callerEc = TCGet(hdr->m_func->m_executable).As();
        assert(TranslateToRawPointer(callerEc)->IsBytecodeFunction());
        HeapPtr<CodeBlock> callerCb = static_cast<HeapPtr<CodeBlock>>(callerEc);
        uint8_t* callerBytecodeStart = callerCb->m_bytecode;
        hdr->m_callerBytecodeOffset = SafeIntegerCast<uint32_t>(bcu - callerBytecodeStart);

        assert(bc->m_funcSlot.IsLocal());
        TValue* begin = StackFrameHeader::GetLocalAddr(sfp, bc->m_funcSlot);
        TValue func = *begin;
        begin++;

        if (func.IsPointer(TValue::x_mivTag))
        {
            if (func.AsPointer().As<UserHeapGcObjectHeader>()->m_type == Type::FUNCTION)
            {
                HeapPtr<FunctionObject> target = func.AsPointer().As<FunctionObject>();

                TValue* sfEnd = reinterpret_cast<TValue*>(sfp) + callerCb->m_stackFrameNumSlots;
                TValue* baseForNextFrame = sfEnd + x_sizeOfStackFrameHeaderInTermsOfTValue;

                uint32_t numFixedArgsToPass = bc->m_numFixedParams;
                uint32_t totalArgs = numFixedArgsToPass;
                if (bc->m_passVariadicRetAsParam)
                {
                    totalArgs += rc->m_numVariadicRets;
                }

                HeapPtr<ExecutableCode> calleeEc = TCGet(target->m_executable).As();

                uint32_t numCalleeExpectingArgs = calleeEc->m_numFixedArguments;
                bool calleeTakesVarArgs = calleeEc->m_hasVariadicArguments;

                // If the callee takes varargs and it is not empty, set up the varargs
                //
                if (unlikely(calleeTakesVarArgs))
                {
                    uint32_t actualNumVarArgs = 0;
                    if (totalArgs > numCalleeExpectingArgs)
                    {
                        actualNumVarArgs = totalArgs - numCalleeExpectingArgs;
                        baseForNextFrame += actualNumVarArgs;
                    }

                    // First, if we need to pass varret, move the whole varret to the correct position
                    //
                    if (bc->m_passVariadicRetAsParam)
                    {
                        TValue* varRetbegin = reinterpret_cast<TValue*>(sfp) + rc->m_variadicRetSlotBegin;
                        // TODO: over-moving is fine
                        memmove(baseForNextFrame + numFixedArgsToPass, varRetbegin, sizeof(TValue) * rc->m_numVariadicRets);
                    }

                    // Now, copy the fixed args to the correct position
                    //
                    SafeMemcpy(baseForNextFrame, begin, sizeof(TValue) * numFixedArgsToPass);

                    // Now, set up the vararg part
                    //
                    if (totalArgs > numCalleeExpectingArgs)
                    {
                        SafeMemcpy(sfEnd, baseForNextFrame + numCalleeExpectingArgs, sizeof(TValue) * (totalArgs - numCalleeExpectingArgs));
                    }

                    // Finally, set up the numVarArgs field in the frame header
                    //
                    StackFrameHeader* sfh = reinterpret_cast<StackFrameHeader*>(baseForNextFrame) - 1;
                    sfh->m_numVariadicArguments = actualNumVarArgs;
                }
                else
                {
                    // First, if we need to pass varret, move the whole varret to the correct position, up to the number of args the callee accepts
                    //
                    if (bc->m_passVariadicRetAsParam)
                    {
                        if (numCalleeExpectingArgs > numFixedArgsToPass)
                        {
                            TValue* varRetbegin = reinterpret_cast<TValue*>(sfp) + rc->m_variadicRetSlotBegin;
                            // TODO: over-moving is fine
                            memmove(baseForNextFrame + numFixedArgsToPass, varRetbegin, sizeof(TValue) * std::min(rc->m_numVariadicRets, numCalleeExpectingArgs - numFixedArgsToPass));
                        }
                    }

                    // Now, copy the fixed args to the correct position, up to the number of args the callee accepts
                    //
                    SafeMemcpy(baseForNextFrame, begin, sizeof(TValue) * std::min(numFixedArgsToPass, numCalleeExpectingArgs));
                }

                // Finally, pad in nils if necessary
                //
                if (totalArgs < numCalleeExpectingArgs)
                {
                    TValue* p = baseForNextFrame + totalArgs;
                    TValue* end = baseForNextFrame + numCalleeExpectingArgs;
                    while (p < end)
                    {
                        *p = TValue::CreateMIV(MiscImmediateValue::CreateNil(), TValue::x_mivTag);
                        p++;
                    }
                }

                // Set up the stack frame header
                //
                StackFrameHeader* sfh = reinterpret_cast<StackFrameHeader*>(baseForNextFrame) - 1;
                sfh->m_caller = reinterpret_cast<StackFrameHeader*>(sfp);
                sfh->m_retAddr = reinterpret_cast<void*>(OnReturn);
                sfh->m_func = target;

                _Pragma("clang diagnostic push")
                _Pragma("clang diagnostic ignored \"-Wuninitialized\"")
                uint64_t unused;
                uint8_t* calleeBytecode = calleeEc->m_bytecode;
                InterpreterFn calleeFn = calleeEc->m_bestEntryPoint;
                [[clang::musttail]] return calleeFn(rc, baseForNextFrame, calleeBytecode, unused);
                _Pragma("clang diagnostic pop")
            }
            else
            {
                assert(false && "unimplemented");
            }
        }
        else
        {
            assert(false && "unimplemented");
        }
    }

    static void OnReturn(CoroutineRuntimeContext* rc, RestrictPtr<void> stackframe, ConstRestrictPtr<uint8_t> retValuesU, uint64_t numRetValues)
    {
        const TValue* retValues = reinterpret_cast<const TValue*>(retValuesU);
        StackFrameHeader* hdr = StackFrameHeader::GetStackFrameHeader(stackframe);
        HeapPtr<ExecutableCode> callerEc = TCGet(hdr->m_func->m_executable).As();
        assert(TranslateToRawPointer(callerEc)->IsBytecodeFunction());
        uint8_t* callerBytecodeStart = callerEc->m_bytecode;
        ConstRestrictPtr<uint8_t> bcu = callerBytecodeStart + hdr->m_callerBytecodeOffset;
        const BcCall* bc = reinterpret_cast<const BcCall*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcCall>);
        if (bc->m_keepVariadicRet)
        {
            rc->m_numVariadicRets = SafeIntegerCast<uint32_t>(numRetValues);
            rc->m_variadicRetSlotBegin = SafeIntegerCast<uint32_t>(retValues - reinterpret_cast<TValue*>(stackframe));
        }
        else
        {
            if (bc->m_numFixedRets <= x_minNilFillReturnValues)
            {
                SafeMemcpy(StackFrameHeader::GetLocalAddr(stackframe, bc->m_funcSlot), retValues, sizeof(TValue) * bc->m_numFixedRets);
            }
            else
            {
                TValue* dst = StackFrameHeader::GetLocalAddr(stackframe, bc->m_funcSlot);
                if (numRetValues < bc->m_numFixedRets)
                {
                    SafeMemcpy(dst, retValues, sizeof(TValue) * numRetValues);
                    while (numRetValues < bc->m_numFixedRets)
                    {
                        dst[numRetValues] = TValue::CreateMIV(MiscImmediateValue::CreateNil(), TValue::x_mivTag);
                        numRetValues++;
                    }
                }
                else
                {
                    SafeMemcpy(dst, retValues, sizeof(TValue) * bc->m_numFixedRets);
                }
            }
        }
        Dispatch(rc, stackframe, bcu + sizeof(BcCall));
    }
} __attribute__((__packed__));

class BcAddVV
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_lhs;
    BytecodeSlot m_rhs;
    BytecodeSlot m_result;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> stackframe, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcAddVV* bc = reinterpret_cast<const BcAddVV*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcAddVV>);
        TValue lhs = StackFrameHeader::GetLocal(stackframe, bc->m_lhs);
        TValue rhs = StackFrameHeader::GetLocal(stackframe, bc->m_rhs);
        if (likely(lhs.IsDouble(TValue::x_int32Tag) && rhs.IsDouble(TValue::x_int32Tag)))
        {
            *StackFrameHeader::GetLocalAddr(stackframe, bc->m_result) = TValue::CreateDouble(lhs.AsDouble() + rhs.AsDouble());
            Dispatch(rc, stackframe, bcu + sizeof(BcAddVV));
        }
        else
        {
            assert(false && "unimplemented");
        }
    }
} __attribute__((__packed__));

class BcSubVV
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_lhs;
    BytecodeSlot m_rhs;
    BytecodeSlot m_result;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> stackframe, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcSubVV* bc = reinterpret_cast<const BcSubVV*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcSubVV>);
        TValue lhs = StackFrameHeader::GetLocal(stackframe, bc->m_lhs);
        TValue rhs = StackFrameHeader::GetLocal(stackframe, bc->m_rhs);
        if (likely(lhs.IsDouble(TValue::x_int32Tag) && rhs.IsDouble(TValue::x_int32Tag)))
        {
            *StackFrameHeader::GetLocalAddr(stackframe, bc->m_result) = TValue::CreateDouble(lhs.AsDouble() - rhs.AsDouble());
            Dispatch(rc, stackframe, bcu + sizeof(BcSubVV));
        }
        else
        {
            assert(false && "unimplemented");
        }
    }
} __attribute__((__packed__));

class BcIsLTVV
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_lhs;
    BytecodeSlot m_rhs;
    int32_t m_offset;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> stackframe, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcIsLTVV* bc = reinterpret_cast<const BcIsLTVV*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcIsLTVV>);
        TValue lhs = StackFrameHeader::GetLocal(stackframe, bc->m_lhs);
        TValue rhs = StackFrameHeader::GetLocal(stackframe, bc->m_rhs);
        if (likely(lhs.IsDouble(TValue::x_int32Tag) && rhs.IsDouble(TValue::x_int32Tag)))
        {
            if (lhs.AsDouble() < rhs.AsDouble())
            {
                Dispatch(rc, stackframe, reinterpret_cast<ConstRestrictPtr<uint8_t>>(reinterpret_cast<intptr_t>(bcu) + bc->m_offset));
            }
            else
            {
                Dispatch(rc, stackframe, bcu + sizeof(BcIsLTVV));
            }
        }
        else
        {
            assert(false && "unimplemented");
        }
    }
} __attribute__((__packed__));

class BcConstant
{
public:
    uint8_t m_opcode;
    BytecodeSlot m_dst;
    TValue m_value;

    static void Execute(CoroutineRuntimeContext* rc, RestrictPtr<void> stackframe, ConstRestrictPtr<uint8_t> bcu, uint64_t /*unused*/)
    {
        const BcConstant* bc = reinterpret_cast<const BcConstant*>(bcu);
        assert(bc->m_opcode == x_opcodeId<BcConstant>);
        *StackFrameHeader::GetLocalAddr(stackframe, bc->m_dst) = bc->m_value;
        Dispatch(rc, stackframe, bcu + sizeof(BcConstant));
    }
} __attribute__((__packed__));

}   // namespace ToyLang
