//===--- PMOMemoryUseCollector.cpp - Memory use analysis for PMO ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "definite-init"
#include "PMOMemoryUseCollector.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                     PMOMemoryObjectInfo Implementation
//===----------------------------------------------------------------------===//

PMOMemoryObjectInfo::PMOMemoryObjectInfo(AllocationInst *allocation)
    : MemoryInst(allocation) {
  auto &module = MemoryInst->getModule();

  // Compute the type of the memory object.
  if (auto *abi = dyn_cast<AllocBoxInst>(MemoryInst)) {
    assert(abi->getBoxType()->getLayout()->getFields().size() == 1 &&
           "analyzing multi-field boxes not implemented");
    MemorySILType = abi->getBoxType()->getFieldType(module, 0);
  } else {
    MemorySILType = cast<AllocStackInst>(MemoryInst)->getElementType();
  }
}

SILInstruction *PMOMemoryObjectInfo::getFunctionEntryPoint() const {
  return &*getFunction().begin()->begin();
}

//===----------------------------------------------------------------------===//
//                          Scalarization Logic
//===----------------------------------------------------------------------===//

/// Given a pointer to a tuple type, compute the addresses of each element and
/// add them to the ElementAddrs vector.
static void
getScalarizedElementAddresses(SILValue Pointer, SILBuilder &B, SILLocation Loc,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
  TupleType *TT = Pointer->getType().castTo<TupleType>();
  for (auto Index : indices(TT->getElements())) {
    ElementAddrs.push_back(B.createTupleElementAddr(Loc, Pointer, Index));
  }
}

/// Given an RValue of aggregate type, compute the values of the elements by
/// emitting a series of tuple_element instructions.
static void getScalarizedElements(SILValue V,
                                  SmallVectorImpl<SILValue> &ElementVals,
                                  SILLocation Loc, SILBuilder &B) {
  TupleType *TT = V->getType().castTo<TupleType>();
  for (auto Index : indices(TT->getElements())) {
    ElementVals.push_back(B.emitTupleExtract(Loc, V, Index));
  }
}

/// Scalarize a load down to its subelements.  If NewLoads is specified, this
/// can return the newly generated sub-element loads.
static SILValue scalarizeLoad(LoadInst *LI,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
  SILBuilderWithScope B(LI);
  SmallVector<SILValue, 4> ElementTmps;

  for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i) {
    auto *SubLI = B.createLoad(LI->getLoc(), ElementAddrs[i],
                               LoadOwnershipQualifier::Unqualified);
    ElementTmps.push_back(SubLI);
  }

  if (LI->getType().is<TupleType>())
    return B.createTuple(LI->getLoc(), LI->getType(), ElementTmps);
  return B.createStruct(LI->getLoc(), LI->getType(), ElementTmps);
}

//===----------------------------------------------------------------------===//
//                     ElementUseCollector Implementation
//===----------------------------------------------------------------------===//

namespace {

class ElementUseCollector {
  SILModule &Module;
  const PMOMemoryObjectInfo &TheMemory;
  SmallVectorImpl<PMOMemoryUse> &Uses;
  SmallVectorImpl<SILInstruction *> &Releases;

  /// When walking the use list, if we index into a struct element, keep track
  /// of this, so that any indexes into tuple subelements don't affect the
  /// element we attribute an access to.
  bool InStructSubElement = false;

public:
  ElementUseCollector(const PMOMemoryObjectInfo &TheMemory,
                      SmallVectorImpl<PMOMemoryUse> &Uses,
                      SmallVectorImpl<SILInstruction *> &Releases)
      : Module(TheMemory.MemoryInst->getModule()), TheMemory(TheMemory),
        Uses(Uses), Releases(Releases) {}

  /// This is the main entry point for the use walker.  It collects uses from
  /// the address and the refcount result of the allocation.
  LLVM_NODISCARD bool collectFrom();

private:
  LLVM_NODISCARD bool collectUses(SILValue Pointer);
  LLVM_NODISCARD bool collectContainerUses(AllocBoxInst *ABI);
};
} // end anonymous namespace

bool ElementUseCollector::collectFrom() {
  bool shouldOptimize = false;

  if (auto *ABI = TheMemory.getContainer()) {
    shouldOptimize = collectContainerUses(ABI);
  } else {
    shouldOptimize = collectUses(TheMemory.getAddress());
  }

  if (!shouldOptimize)
    return false;

  // Collect information about the retain count result as well.
  for (auto *op : TheMemory.MemoryInst->getUses()) {
    auto *user = op->getUser();

    // If this is a strong_release, stash it.
    if (isa<StrongReleaseInst>(user)) {
      Releases.push_back(user);
    }
  }

  return true;
}

bool ElementUseCollector::collectContainerUses(AllocBoxInst *ABI) {
  for (Operand *UI : ABI->getUses()) {
    auto *User = UI->getUser();

    // Deallocations and retain/release don't affect the value directly.
    if (isa<DeallocBoxInst>(User))
      continue;
    if (isa<StrongRetainInst>(User))
      continue;
    if (isa<StrongReleaseInst>(User))
      continue;

    if (auto project = dyn_cast<ProjectBoxInst>(User)) {
      if (!collectUses(project))
        return false;
      continue;
    }

    // Other uses of the container are considered escapes of the underlying
    // value.
    //
    // This will cause the dataflow to stop propagating any information at the
    // use block.
    Uses.emplace_back(User, PMOUseKind::Escape);
  }

  return true;
}

bool ElementUseCollector::collectUses(SILValue Pointer) {
  assert(Pointer->getType().isAddress() &&
         "Walked through the pointer to the value?");
  SILType PointeeType = Pointer->getType().getObjectType();

  /// This keeps track of instructions in the use list that touch multiple tuple
  /// elements and should be scalarized.  This is done as a second phase to
  /// avoid invalidating the use iterator.
  ///
  SmallVector<SILInstruction *, 4> UsesToScalarize;

  for (auto *UI : Pointer->getUses()) {
    auto *User = UI->getUser();

    // struct_element_addr P, #field indexes into the current element.
    if (auto *seai = dyn_cast<StructElementAddrInst>(User)) {
      // Generally, we set the "InStructSubElement" flag and recursively process
      // the uses so that we know that we're looking at something within the
      // current element.
      llvm::SaveAndRestore<bool> X(InStructSubElement, true);
      if (!collectUses(seai))
        return false;
      continue;
    }

    // Instructions that compute a subelement are handled by a helper.
    if (auto *teai = dyn_cast<TupleElementAddrInst>(User)) {
      if (!collectUses(teai))
        return false;
      continue;
    }

    // Look through begin_access.
    if (auto *bai = dyn_cast<BeginAccessInst>(User)) {
      if (!collectUses(bai))
        return false;
      continue;
    }

    // Ignore end_access.
    if (isa<EndAccessInst>(User)) {
      continue;
    }

    // Loads are a use of the value.
    if (isa<LoadInst>(User)) {
      if (PointeeType.is<TupleType>())
        UsesToScalarize.push_back(User);
      else
        Uses.emplace_back(User, PMOUseKind::Load);
      continue;
    }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  if (isa<Load##Name##Inst>(User)) {                                           \
    Uses.emplace_back(User, PMOUseKind::Load);                                 \
    continue;                                                                  \
  }
#include "swift/AST/ReferenceStorage.def"

    // Stores *to* the allocation are writes.
    if (isa<StoreInst>(User) && UI->getOperandNumber() == 1) {
      if (PointeeType.is<TupleType>()) {
        UsesToScalarize.push_back(User);
        continue;
      }

      // Coming out of SILGen, we assume that raw stores are initializations,
      // unless they have trivial type (which we classify as InitOrAssign).
      PMOUseKind Kind;
      if (InStructSubElement)
        Kind = PMOUseKind::PartialStore;
      else if (PointeeType.isTrivial(User->getModule()))
        Kind = PMOUseKind::InitOrAssign;
      else
        Kind = PMOUseKind::Initialization;

      Uses.emplace_back(User, Kind);
      continue;
    }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  if (auto *SI = dyn_cast<Store##Name##Inst>(User)) {                          \
    if (UI->getOperandNumber() == 1) {                                         \
      PMOUseKind Kind;                                                         \
      if (InStructSubElement)                                                  \
        Kind = PMOUseKind::PartialStore;                                       \
      else if (SI->isInitializationOfDest())                                   \
        Kind = PMOUseKind::Initialization;                                     \
      else                                                                     \
        Kind = PMOUseKind::Assign;                                             \
      Uses.emplace_back(User, Kind);                                           \
      continue;                                                                \
    }                                                                          \
  }
#include "swift/AST/ReferenceStorage.def"

    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      // If this is a copy of a tuple, we should scalarize it so that we don't
      // have an access that crosses elements.
      if (PointeeType.is<TupleType>()) {
        UsesToScalarize.push_back(CAI);
        continue;
      }

      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is an unknown assignment.  Note that we'll
      // revisit this instruction and add it to Uses twice if it is both a load
      // and store to the same aggregate.
      PMOUseKind Kind;
      if (UI->getOperandNumber() == 0)
        Kind = PMOUseKind::Load;
      else if (InStructSubElement)
        Kind = PMOUseKind::PartialStore;
      else if (CAI->isInitializationOfDest())
        Kind = PMOUseKind::Initialization;
      else
        Kind = PMOUseKind::Assign;

      Uses.emplace_back(User, Kind);
      continue;
    }

    // The apply instruction does not capture the pointer when it is passed
    // through 'inout' arguments or for indirect returns.  InOut arguments are
    // treated as uses and may-store's, but an indirect return is treated as a
    // full store.
    //
    // Note that partial_apply instructions always close over their argument.
    //
    if (auto *Apply = dyn_cast<ApplyInst>(User)) {
      auto substConv = Apply->getSubstCalleeConv();
      unsigned ArgumentNumber = UI->getOperandNumber() - 1;

      // If this is an out-parameter, it is like a store.
      unsigned NumIndirectResults = substConv.getNumIndirectSILResults();
      if (ArgumentNumber < NumIndirectResults) {
        // We do not support initializing sub members. This is an old
        // restriction from when this code was used by Definite
        // Initialization. With proper code review, we can remove this, but for
        // now, lets be conservative.
        if (InStructSubElement) {
          return false;
        }
        Uses.emplace_back(User, PMOUseKind::Initialization);
        continue;

        // Otherwise, adjust the argument index.
      } else {
        ArgumentNumber -= NumIndirectResults;
      }

      auto ParamConvention =
          substConv.getParameters()[ArgumentNumber].getConvention();

      switch (ParamConvention) {
      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Guaranteed:
        llvm_unreachable("address value passed to indirect parameter");

      // If this is an in-parameter, it is like a load.
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Constant:
      case ParameterConvention::Indirect_In_Guaranteed:
        Uses.emplace_back(User, PMOUseKind::IndirectIn);
        continue;

      // If this is an @inout parameter, it is like both a load and store.
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable: {
        // If we're in the initializer for a struct, and this is a call to a
        // mutating method, we model that as an escape of self.  If an
        // individual sub-member is passed as inout, then we model that as an
        // inout use.
        Uses.emplace_back(User, PMOUseKind::InOutUse);
        continue;
      }
      }
      llvm_unreachable("bad parameter convention");
    }

    // init_existential_addr is modeled as an initialization store.
    if (isa<InitExistentialAddrInst>(User)) {
      // init_existential_addr should not apply to struct subelements.
      if (InStructSubElement) {
        return false;
      }
      Uses.emplace_back(User, PMOUseKind::Initialization);
      continue;
    }

    // open_existential_addr is a use of the protocol value,
    // so it is modeled as a load.
    if (isa<OpenExistentialAddrInst>(User)) {
      Uses.emplace_back(User, PMOUseKind::Load);
      // TODO: Is it safe to ignore all uses of the open_existential_addr?
      continue;
    }

    // We model destroy_addr as a release of the entire value.
    if (isa<DestroyAddrInst>(User)) {
      Releases.push_back(User);
      continue;
    }

    if (isa<DeallocStackInst>(User)) {
      continue;
    }

    // Sanitizer instrumentation is not user visible, so it should not
    // count as a use and must not affect compile-time diagnostics.
    if (isSanitizerInstrumentation(User))
      continue;

    // Otherwise, the use is something complicated, it escapes.
    Uses.emplace_back(User, PMOUseKind::Escape);
  }

  // Now that we've walked all of the immediate uses, scalarize any operations
  // working on tuples if we need to for canonicalization or analysis reasons.
  if (!UsesToScalarize.empty()) {
    SILInstruction *PointerInst = Pointer->getDefiningInstruction();
    SmallVector<SILValue, 4> ElementAddrs;
    SILBuilderWithScope AddrBuilder(++SILBasicBlock::iterator(PointerInst),
                                    PointerInst);
    getScalarizedElementAddresses(Pointer, AddrBuilder, PointerInst->getLoc(),
                                  ElementAddrs);

    SmallVector<SILValue, 4> ElementTmps;
    for (auto *User : UsesToScalarize) {
      ElementTmps.clear();

      LLVM_DEBUG(llvm::errs() << "  *** Scalarizing: " << *User << "\n");

      // Scalarize LoadInst
      if (auto *LI = dyn_cast<LoadInst>(User)) {
        SILValue Result = scalarizeLoad(LI, ElementAddrs);
        LI->replaceAllUsesWith(Result);
        LI->eraseFromParent();
        continue;
      }

      // Scalarize StoreInst
      if (auto *SI = dyn_cast<StoreInst>(User)) {
        SILBuilderWithScope B(User, SI);
        getScalarizedElements(SI->getOperand(0), ElementTmps, SI->getLoc(), B);

        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createStore(SI->getLoc(), ElementTmps[i], ElementAddrs[i],
                        StoreOwnershipQualifier::Unqualified);
        SI->eraseFromParent();
        continue;
      }

      // Scalarize CopyAddrInst.
      auto *CAI = cast<CopyAddrInst>(User);
      SILBuilderWithScope B(User, CAI);

      // Determine if this is a copy *from* or *to* "Pointer".
      if (CAI->getSrc() == Pointer) {
        // Copy from pointer.
        getScalarizedElementAddresses(CAI->getDest(), B, CAI->getLoc(),
                                      ElementTmps);
        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createCopyAddr(CAI->getLoc(), ElementAddrs[i], ElementTmps[i],
                           CAI->isTakeOfSrc(), CAI->isInitializationOfDest());

      } else {
        getScalarizedElementAddresses(CAI->getSrc(), B, CAI->getLoc(),
                                      ElementTmps);
        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createCopyAddr(CAI->getLoc(), ElementTmps[i], ElementAddrs[i],
                           CAI->isTakeOfSrc(), CAI->isInitializationOfDest());
      }
      CAI->eraseFromParent();
    }

    // Now that we've scalarized some stuff, recurse down into the newly created
    // element address computations to recursively process it.  This can cause
    // further scalarization.
    if (llvm::any_of(ElementAddrs, [&](SILValue v) {
          return !collectUses(cast<TupleElementAddrInst>(v));
        })) {
      return false;
    }
  }

  return true;
}

/// collectPMOElementUsesFrom - Analyze all uses of the specified allocation
/// instruction (alloc_box, alloc_stack or mark_uninitialized), classifying them
/// and storing the information found into the Uses and Releases lists.
bool swift::collectPMOElementUsesFrom(
    const PMOMemoryObjectInfo &MemoryInfo, SmallVectorImpl<PMOMemoryUse> &Uses,
    SmallVectorImpl<SILInstruction *> &Releases) {
  return ElementUseCollector(MemoryInfo, Uses, Releases).collectFrom();
}
