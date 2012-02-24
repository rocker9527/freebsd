//===-- Mips/MipsCodeEmitter.cpp - Convert Mips code to machine code -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// This file contains the pass that transforms the Mips machine instructions
// into relocatable machine code.
//
//===---------------------------------------------------------------------===//

#define DEBUG_TYPE "jit"
#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsRelocations.h"
#include "MipsSubtarget.h"
#include "MipsTargetMachine.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/PassManager.h"
#include "llvm/CodeGen/JITCodeEmitter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#ifndef NDEBUG
#include <iomanip>
#endif

#include "llvm/CodeGen/MachineOperand.h"

using namespace llvm;

STATISTIC(NumEmitted, "Number of machine instructions emitted");

namespace {

class MipsCodeEmitter : public MachineFunctionPass {
  MipsJITInfo *JTI;
  const MipsInstrInfo *II;
  const TargetData *TD;
  const MipsSubtarget *Subtarget;
  TargetMachine &TM;
  JITCodeEmitter &MCE;
  const std::vector<MachineConstantPoolEntry> *MCPEs;
  const std::vector<MachineJumpTableEntry> *MJTEs;
  bool IsPIC;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<MachineModuleInfo> ();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  static char ID;

  public:
    MipsCodeEmitter(TargetMachine &tm, JITCodeEmitter &mce) :
      MachineFunctionPass(ID), JTI(0),
        II((const MipsInstrInfo *) tm.getInstrInfo()),
        TD(tm.getTargetData()), TM(tm), MCE(mce), MCPEs(0), MJTEs(0),
        IsPIC(TM.getRelocationModel() == Reloc::PIC_) {
    }

    bool runOnMachineFunction(MachineFunction &MF);

    virtual const char *getPassName() const {
      return "Mips Machine Code Emitter";
    }

    /// getBinaryCodeForInstr - This function, generated by the
    /// CodeEmitterGenerator using TableGen, produces the binary encoding for
    /// machine instructions.
    unsigned getBinaryCodeForInstr(const MachineInstr &MI) const;

    void emitInstruction(const MachineInstr &MI);

  private:

    void emitWordLE(unsigned Word);

    /// Routines that handle operands which add machine relocations which are
    /// fixed up by the relocation stage.
    void emitGlobalAddress(const GlobalValue *GV, unsigned Reloc,
                                bool MayNeedFarStub) const;
    void emitExternalSymbolAddress(const char *ES, unsigned Reloc) const;
    void emitConstPoolAddress(unsigned CPI, unsigned Reloc) const;
    void emitJumpTableAddress(unsigned JTIndex, unsigned Reloc) const;
    void emitMachineBasicBlock(MachineBasicBlock *BB, unsigned Reloc) const;

    /// getMachineOpValue - Return binary encoding of operand. If the machine
    /// operand requires relocation, record the relocation and return zero.
    unsigned getMachineOpValue(const MachineInstr &MI,
                               const MachineOperand &MO) const;

    unsigned getRelocation(const MachineInstr &MI,
                           const MachineOperand &MO) const;

    unsigned getMemEncoding(const MachineInstr &MI, unsigned OpNo) const;
    unsigned getSizeExtEncoding(const MachineInstr &MI, unsigned OpNo) const;
    unsigned getSizeInsEncoding(const MachineInstr &MI, unsigned OpNo) const;
  };
}

char MipsCodeEmitter::ID = 0;

bool MipsCodeEmitter::runOnMachineFunction(MachineFunction &MF) {
  JTI = ((MipsTargetMachine&) MF.getTarget()).getJITInfo();
  II = ((const MipsTargetMachine&) MF.getTarget()).getInstrInfo();
  TD = ((const MipsTargetMachine&) MF.getTarget()).getTargetData();
  Subtarget = &TM.getSubtarget<MipsSubtarget> ();
  MCPEs = &MF.getConstantPool()->getConstants();
  MJTEs = 0;
  if (MF.getJumpTableInfo()) MJTEs = &MF.getJumpTableInfo()->getJumpTables();
  JTI->Initialize(MF, IsPIC);
  MCE.setModuleInfo(&getAnalysis<MachineModuleInfo> ());

  do {
    DEBUG(errs() << "JITTing function '"
        << MF.getFunction()->getName() << "'\n");
    MCE.startFunction(MF);

    for (MachineFunction::iterator MBB = MF.begin(), E = MF.end();
        MBB != E; ++MBB){
      MCE.StartMachineBasicBlock(MBB);
      for (MachineBasicBlock::const_iterator I = MBB->begin(), E = MBB->end();
          I != E; ++I)
        emitInstruction(*I);
    }
  } while (MCE.finishFunction(MF));

  return false;
}

unsigned MipsCodeEmitter::getRelocation(const MachineInstr &MI,
                                        const MachineOperand &MO) const {
  // NOTE: This relocations are for static.
  uint64_t TSFlags = MI.getDesc().TSFlags;
  uint64_t Form = TSFlags & MipsII::FormMask;
  if (Form == MipsII::FrmJ)
    return Mips::reloc_mips_26;
  if ((Form == MipsII::FrmI || Form == MipsII::FrmFI)
       && MI.getDesc().isBranch())
    return Mips::reloc_mips_branch;
  if (Form == MipsII::FrmI && MI.getOpcode() == Mips::LUi)
    return Mips::reloc_mips_hi;
  return Mips::reloc_mips_lo;
}

unsigned MipsCodeEmitter::getMemEncoding(const MachineInstr &MI,
                                          unsigned OpNo) const {
  // Base register is encoded in bits 20-16, offset is encoded in bits 15-0.
  assert(MI.getOperand(OpNo).isReg());
  unsigned RegBits = getMachineOpValue(MI, MI.getOperand(OpNo)) << 16;
  return
    (getMachineOpValue(MI, MI.getOperand(OpNo+1)) & 0xFFFF) | RegBits;
}

unsigned MipsCodeEmitter::getSizeExtEncoding(const MachineInstr &MI,
                                          unsigned OpNo) const {
  // size is encoded as size-1.
  return getMachineOpValue(MI, MI.getOperand(OpNo)) - 1;
}

unsigned MipsCodeEmitter::getSizeInsEncoding(const MachineInstr &MI,
                                          unsigned OpNo) const {
  // size is encoded as pos+size-1.
  return getMachineOpValue(MI, MI.getOperand(OpNo-1)) +
         getMachineOpValue(MI, MI.getOperand(OpNo)) - 1;
}

/// getMachineOpValue - Return binary encoding of operand. If the machine
/// operand requires relocation, record the relocation and return zero.
unsigned MipsCodeEmitter::getMachineOpValue(const MachineInstr &MI,
                                           const MachineOperand &MO) const {
  if (MO.isReg())
    return MipsRegisterInfo::getRegisterNumbering(MO.getReg());
  else if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());
  else if (MO.isGlobal())
    emitGlobalAddress(MO.getGlobal(), getRelocation(MI, MO), true);
  else if (MO.isSymbol())
    emitExternalSymbolAddress(MO.getSymbolName(), getRelocation(MI, MO));
  else if (MO.isCPI())
    emitConstPoolAddress(MO.getIndex(), getRelocation(MI, MO));
  else if (MO.isJTI())
    emitJumpTableAddress(MO.getIndex(), getRelocation(MI, MO));
  else if (MO.isMBB())
    emitMachineBasicBlock(MO.getMBB(), getRelocation(MI, MO));
  else
    llvm_unreachable("Unable to encode MachineOperand!");
  return 0;
}

void MipsCodeEmitter::emitGlobalAddress(const GlobalValue *GV, unsigned Reloc,
                                                bool MayNeedFarStub) const {
  MCE.addRelocation(MachineRelocation::getGV(MCE.getCurrentPCOffset(), Reloc,
                             const_cast<GlobalValue *>(GV), 0, MayNeedFarStub));
}

void MipsCodeEmitter::
emitExternalSymbolAddress(const char *ES, unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getExtSym(MCE.getCurrentPCOffset(),
                                                 Reloc, ES, 0, 0, false));
}

void MipsCodeEmitter::emitConstPoolAddress(unsigned CPI, unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getConstPool(MCE.getCurrentPCOffset(),
                                                    Reloc, CPI, 0, false));
}

void MipsCodeEmitter::
emitJumpTableAddress(unsigned JTIndex, unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getJumpTable(MCE.getCurrentPCOffset(),
                                                    Reloc, JTIndex, 0, false));
}

void MipsCodeEmitter::emitMachineBasicBlock(MachineBasicBlock *BB,
                                           unsigned Reloc) const {
  MCE.addRelocation(MachineRelocation::getBB(MCE.getCurrentPCOffset(),
                                             Reloc, BB));
}

void MipsCodeEmitter::emitInstruction(const MachineInstr &MI) {
  DEBUG(errs() << "JIT: " << (void*)MCE.getCurrentPCValue() << ":\t" << MI);

  MCE.processDebugLoc(MI.getDebugLoc(), true);

  // Skip pseudo instructions.
  if ((MI.getDesc().TSFlags & MipsII::FormMask) == MipsII::Pseudo)
    return;

  ++NumEmitted;  // Keep track of the # of mi's emitted

  switch (MI.getOpcode()) {
  default:
    emitWordLE(getBinaryCodeForInstr(MI));
    break;
  }

  MCE.processDebugLoc(MI.getDebugLoc(), false);
}

void MipsCodeEmitter::emitWordLE(unsigned Word) {
  DEBUG(errs() << "  0x";
        errs().write_hex(Word) << "\n");
  MCE.emitWordLE(Word);
}

/// createMipsJITCodeEmitterPass - Return a pass that emits the collected Mips
/// code to the specified MCE object.
FunctionPass *llvm::createMipsJITCodeEmitterPass(MipsTargetMachine &TM,
    JITCodeEmitter &JCE) {
  return new MipsCodeEmitter(TM, JCE);
}

#include "MipsGenCodeEmitter.inc"
