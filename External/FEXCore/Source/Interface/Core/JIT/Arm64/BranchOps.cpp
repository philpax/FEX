#include "Interface/Core/JIT/Arm64/JITClass.h"
#include "Interface/Core/InternalThreadState.h"

#include <FEXCore/Core/X86Enums.h>

namespace FEXCore::CPU {
using namespace vixl;
using namespace vixl::aarch64;
#define DEF_OP(x) void JITCore::Op_##x(FEXCore::IR::IROp_Header *IROp, uint32_t Node)
DEF_OP(GuestCallDirect) {
  LogMan::Msg::D("Unimplemented");
}

DEF_OP(GuestCallIndirect) {
  LogMan::Msg::D("Unimplemented");
}

DEF_OP(GuestReturn) {
  LogMan::Msg::D("Unimplemented");
}

DEF_OP(SignalReturn) {
  // First we must reset the stack
  //ldp(TMP1, lr, MemOperand(sp, 16, PostIndex));
  //add(sp, TMP1, 0); // Move that supports SP
  add(sp, sp, SpillSlots * 16);

  // Now branch to our signal return helper
  // This can't be a direct branch since the code needs to live at a constant location
  LoadConstant(x0, SignalReturnInstruction);
  br(x0);
}

DEF_OP(CallbackReturn) {
  // First we must reset the stack

  //TODO: FIXME
  //ldp(TMP1, lr, MemOperand(sp, 16, PostIndex));
  //add(sp, TMP1, 0); // Move that supports SP
  add(sp, sp, SpillSlots * 16);

  // We can now lower the ref counter again
  LoadConstant(x0, reinterpret_cast<uint64_t>(&SignalHandlerRefCounter));
  ldr(w2, MemOperand(x0));
  sub(w2, w2, 1);
  str(w2, MemOperand(x0));

  // We need to adjust an additional 8 bytes to get back to the original "misaligned" RSP state
  ldr(x2, MemOperand(STATE, offsetof(FEXCore::Core::InternalThreadState, State.State.gregs[X86State::REG_RSP])));
  add(x2, x2, 8);
  str(x2, MemOperand(STATE, offsetof(FEXCore::Core::InternalThreadState, State.State.gregs[X86State::REG_RSP])));

  PopCalleeSavedRegisters();

  // Return to the thunk
  ret();
}

DEF_OP(ExitFunction) {
  auto Op = IROp->C<IR::IROp_ExitFunction>();

  add(sp, sp, SpillSlots * 16);
  //ldp(TMP1, lr, MemOperand(sp, 16, PostIndex));
  //add(sp, TMP1, 0); // Move that supports SP
  //ret();

  aarch64::Label FullLookup;

  uint64_t Const;
  bool isConst = IsInlineConstant(Op->Header.Args[0], &Const);

  if (isConst) {
    //LoadConstant(x2, 0xDEADBEEF00);
    //str(x2, MemOperand(STATE, offsetof(FEXCore::Core::ThreadState, State.rip)));
    auto RipReg = x2;

    //str(RipReg, MemOperand(STATE, offsetof(FEXCore::Core::ThreadState, State.rip)));

    auto L1Ptr = State->BlockCache->GetL1Pointer() + (Const & (1024 * 1024 - 1)) * 16;
    // L1 Cache
    LoadConstant(x0, L1Ptr);
    ldp(x0, x1, MemOperand(x0));
    LoadConstant(RipReg, Const);
    cmp(x0, RipReg);
    b(&FullLookup, Condition::ne);
    br(x1);

    bind(&FullLookup);
    LoadConstant(x0, AbsoluteFullLookupAddress);
    br(x0);
  }
  else {
    auto RipReg = GetReg<RA_64>(Op->Header.Args[0].ID());

    //LoadConstant(x2, 0xDEADBEEF00);
    //str(x2, MemOperand(STATE, offsetof(FEXCore::Core::ThreadState, State.rip)));

    //str(RipReg, MemOperand(STATE, offsetof(FEXCore::Core::ThreadState, State.rip)));

    // L1 Cache
    LoadConstant(x0, State->BlockCache->GetL1Pointer());

    and_(x3, RipReg, 1 * 1024 * 1024 - 1);
    add(x0, x0, Operand(x3, Shift::LSL, 4));
    ldp(x0, x1, MemOperand(x0));
    cmp(x0, RipReg);
    b(&FullLookup, Condition::ne);
    br(x1);

    bind(&FullLookup);
    mov(x2, RipReg);
    LoadConstant(x0, AbsoluteFullLookupAddress);
    br(x0);
  }
}

DEF_OP(Jump) {
  auto Op = IROp->C<IR::IROp_Jump>();

  Label *TargetLabel;
  auto IsTarget = JumpTargets.find(Op->Header.Args[0].ID());
  if (IsTarget == JumpTargets.end()) {
    TargetLabel = &JumpTargets.try_emplace(Op->Header.Args[0].ID()).first->second;
  }
  else {
    TargetLabel = &IsTarget->second;
  }
  PendingTargetLabel = TargetLabel;
}

#define GRCMP(Node) (Op->CompareSize == 4 ? GetReg<RA_32>(Node) : GetReg<RA_64>(Node))

DEF_OP(CondJump) {
  auto Op = IROp->C<IR::IROp_CondJump>();

  Label *TrueTargetLabel;
  Label *FalseTargetLabel;

  auto TrueIter = JumpTargets.find(Op->Header.Args[3].ID());
  auto FalseIter = JumpTargets.find(Op->Header.Args[4].ID());

  if (TrueIter == JumpTargets.end()) {
    TrueTargetLabel = &JumpTargets.try_emplace(Op->Header.Args[3].ID()).first->second;
  }
  else {
    TrueTargetLabel = &TrueIter->second;
  }

  if (Op->Header.Args[1].IsInvalid()) {
    cbnz(GetReg<RA_64>(Op->Header.Args[0].ID()), TrueTargetLabel);
  } else {
      uint64_t Const;
      bool isConst = IsInlineConstant(Op->Header.Args[1], &Const);
      if (isConst && Const == 0 && Op->Operation.Val == FEXCore::IR::COND_EQ) {
        cbz(GRCMP(Op->Header.Args[0].ID()), TrueTargetLabel);
      } else if (isConst && Const == 0 && Op->Operation.Val == FEXCore::IR::COND_NEQ) {
        cbnz(GRCMP(Op->Header.Args[0].ID()), TrueTargetLabel);
      } else {
      if (isConst)
        cmp(GRCMP(Op->Header.Args[0].ID()), Const);
      else
        cmp(GRCMP(Op->Header.Args[0].ID()), GRCMP(Op->Header.Args[1].ID()));

      switch (Op->Operation.Val) {
      case FEXCore::IR::COND_EQ:
        b(TrueTargetLabel, Condition::eq);
      break;
      case FEXCore::IR::COND_NEQ:
        b(TrueTargetLabel, Condition::ne);
      break;
      case FEXCore::IR::COND_SGE:
        b(TrueTargetLabel, Condition::ge);
      break;
      case FEXCore::IR::COND_SLT:
        b(TrueTargetLabel, Condition::lt);
      break;
      case FEXCore::IR::COND_SGT:
        b(TrueTargetLabel, Condition::gt);
      break;
      case FEXCore::IR::COND_SLE:
        b(TrueTargetLabel, Condition::le);
      break;
      case FEXCore::IR::COND_UGE:
        b(TrueTargetLabel, Condition::cs);
      break;
      case FEXCore::IR::COND_ULT:
        b(TrueTargetLabel, Condition::cc);
      break;
      case FEXCore::IR::COND_UGT:
        b(TrueTargetLabel, Condition::hi);
      break;
      case FEXCore::IR::COND_ULE:
        b(TrueTargetLabel, Condition::ls);
      break;
      case FEXCore::IR::COND_MI:
      case FEXCore::IR::COND_PL:
      case FEXCore::IR::COND_VS:
      case FEXCore::IR::COND_VC:
      default:
      LogMan::Msg::A("Unsupported compare type");
      break;
      }
      }
  }
  
  if (FalseIter == JumpTargets.end()) {
    FalseTargetLabel = &JumpTargets.try_emplace(Op->Header.Args[4].ID()).first->second;
  }
  else {
    FalseTargetLabel = &FalseIter->second;
  }
  PendingTargetLabel = FalseTargetLabel;
}

DEF_OP(Syscall) {
  auto Op = IROp->C<IR::IROp_Syscall>();
  // Arguments are passed as follows:
  // X0: SyscallHandler
  // X1: ThreadState
  // X2: Pointer to SyscallArguments

  uint64_t SPOffset = AlignUp((RA64.size() + 7 + 1) * 8, 16);

  sub(sp, sp, SPOffset);
  for (uint32_t i = 0; i < FEXCore::HLE::SyscallArguments::MAX_ARGS; ++i) {
    if (Op->Header.Args[i].IsInvalid()) continue;
    str(GetReg<RA_64>(Op->Header.Args[i].ID()), MemOperand(sp, 0 + i * 8));
  }

  int i = 0;
  for (auto RA : RA64) {
    str(RA, MemOperand(sp, 7 * 8 + i * 8));
    i++;
  }
  str(lr,       MemOperand(sp, 7 * 8 + RA64.size() * 8 + 0 * 8));

  LoadConstant(x0, reinterpret_cast<uint64_t>(CTX->SyscallHandler.get()));
  mov(x1, STATE);
  mov(x2, sp);

  LoadConstant(x3, reinterpret_cast<uint64_t>(FEXCore::HandleSyscall));
  blr(x3);

  // Result is now in x0
  // Fix the stack and any values that were stepped on
  i = 0;
  for (auto RA : RA64) {
    ldr(RA, MemOperand(sp, 7 * 8 + i * 8));
    i++;
  }

  // Move result to its destination register
  mov(GetReg<RA_64>(Node), x0);

  ldr(lr,       MemOperand(sp, 7 * 8 + RA64.size() * 8 + 0 * 8));

  add(sp, sp, SPOffset);
}

DEF_OP(Thunk) {
  auto Op = IROp->C<IR::IROp_Thunk>();
  // Arguments are passed as follows:
  // X0: CTX
  // X1: Args (from guest stack)

  uint64_t SPOffset = AlignUp((RA64.size() + 1) * 8, 16);

  sub(sp, sp, SPOffset);

  int i = 0;
  for (auto RA : RA64) {
    str(RA, MemOperand(sp, i * 8));
    i++;
  }
  str(lr, MemOperand(sp, RA64.size() * 8 + 0 * 8));

  mov(x0, GetReg<RA_64>(Op->Header.Args[0].ID()));

#if _M_X86_64
  ERROR_AND_DIE("JIT: OP_THUNK not supported with arm simulator")
#else
  LoadConstant(x2, Op->ThunkFnPtr);
  blr(x2);
#endif

  // Fix the stack and any values that were stepped on
  i = 0;
  for (auto RA : RA64) {
    ldr(RA, MemOperand(sp, i * 8));
    i++;
  }

  ldr(lr, MemOperand(sp, RA64.size() * 8 + 0 * 8));

  add(sp, sp, SPOffset);
}


DEF_OP(ValidateCode) {
  auto Op = IROp->C<IR::IROp_ValidateCode>();
  uint8_t *NewCode = (uint8_t *)Op->CodePtr;
  uint8_t *OldCode = (uint8_t *)&Op->CodeOriginal;
  int len = Op->CodeLength;
  int idx = 0;

  LoadConstant(GetReg<RA_64>(Node), 0);
  LoadConstant(x0, Op->CodePtr);
  LoadConstant(x1, 1);

  while (len >= 4)
  {
    ldr(w2, MemOperand(x0, idx));
    LoadConstant(w3, *(uint32_t *)(OldCode + idx));
    cmp(w2, w3);
    csel(GetReg<RA_64>(Node), GetReg<RA_64>(Node), x1, Condition::eq);
    len -= 4;
    idx += 4;
  }
  while (len >= 2)
  {
    ldrh(w2, MemOperand(x0, idx));
    LoadConstant(w3, *(uint16_t *)(OldCode + idx));
    cmp(w2, w3);
    csel(GetReg<RA_64>(Node), GetReg<RA_64>(Node), x1, Condition::eq);
    len -= 2;
    idx += 2;
  }
  while (len >= 1)
  {
    ldrb(w2, MemOperand(x0, idx));
    LoadConstant(w3, *(uint8_t *)(OldCode + idx));
    cmp(w2, w3);
    csel(GetReg<RA_64>(Node), GetReg<RA_64>(Node), x1, Condition::eq);
    len -= 1;
    idx += 1;
  }
}

DEF_OP(RemoveCodeEntry) {
  auto Op = IROp->C<IR::IROp_RemoveCodeEntry>();
  // Arguments are passed as follows:
  // X0: Thread
  // X1: RIP

  uint64_t SPOffset = AlignUp((RA64.size() + 1) * 8, 16);

  sub(sp, sp, SPOffset);

  int i = 0;
  for (auto RA : RA64) {
    str(RA, MemOperand(sp, i * 8));
    i++;
  }
  str(lr, MemOperand(sp, RA64.size() * 8 + 0 * 8));

  mov(x0, STATE);
  LoadConstant(x1, Op->RIP);
 
  LoadConstant(x2, reinterpret_cast<uintptr_t>(&Context::Context::RemoveCodeEntry));
  blr(x2);

  // Fix the stack and any values that were stepped on
  i = 0;
  for (auto RA : RA64) {
    ldr(RA, MemOperand(sp, i * 8));
    i++;
  }

  ldr(lr, MemOperand(sp, RA64.size() * 8 + 0 * 8));

  add(sp, sp, SPOffset);
}

DEF_OP(CPUID) {
  auto Op = IROp->C<IR::IROp_CPUID>();
  uint64_t SPOffset = AlignUp((RA64.size() + 2 + 2) * 8, 16);
  sub(sp, sp, SPOffset);

  int i = 0;
  for (auto RA : RA64) {
    str(RA, MemOperand(sp, 0 + i * 8));
    i++;
  }

  str(lr,       MemOperand(sp, RA64.size() * 8 + 0 * 8));

  // x0 = CPUID Handler
  // x1 = CPUID Function
  LoadConstant(x0, reinterpret_cast<uint64_t>(&CTX->CPUID));
  mov(x1, GetReg<RA_64>(Op->Header.Args[0].ID()));

  using ClassPtrType = FEXCore::CPUIDEmu::FunctionResults (FEXCore::CPUIDEmu::*)(uint32_t);
  union PtrCast {
    ClassPtrType ClassPtr;
    uintptr_t Data;
  };

  PtrCast Ptr;
  Ptr.ClassPtr = &FEXCore::CPUIDEmu::RunFunction;
  LoadConstant(x3, Ptr.Data);
  blr(x3);

  i = 0;
  for (auto RA : RA64) {
    ldr(RA, MemOperand(sp, 0 + i * 8));
    i++;
  }

  // Results are in x0, x1
  // Results want to be in a i64v2 vector
  auto Dst = GetSrcPair<RA_64>(Node);
  mov(Dst.first,  x0);
  mov(Dst.second, x1);

  ldr(lr,       MemOperand(sp, RA64.size() * 8 + 0 * 8));

  add(sp, sp, SPOffset);
}

#undef DEF_OP
void JITCore::RegisterBranchHandlers() {
#define REGISTER_OP(op, x) OpHandlers[FEXCore::IR::IROps::OP_##op] = &JITCore::Op_##x
  REGISTER_OP(GUESTCALLDIRECT,   GuestCallDirect);
  REGISTER_OP(GUESTCALLINDIRECT, GuestCallIndirect);
  REGISTER_OP(GUESTRETURN,       GuestReturn);
  REGISTER_OP(SIGNALRETURN,      SignalReturn);
  REGISTER_OP(CALLBACKRETURN,    CallbackReturn);
  REGISTER_OP(EXITFUNCTION,      ExitFunction);
  REGISTER_OP(JUMP,              Jump);
  REGISTER_OP(CONDJUMP,          CondJump);
  REGISTER_OP(SYSCALL,           Syscall);
  REGISTER_OP(THUNK,             Thunk);
  REGISTER_OP(VALIDATECODE,      ValidateCode);
  REGISTER_OP(REMOVECODEENTRY,   RemoveCodeEntry);
  REGISTER_OP(CPUID,             CPUID);
#undef REGISTER_OP
}
}

