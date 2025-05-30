//===-- AArch64AsmBackend.cpp - AArch64 Assembler Backend -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCRegisterInfo.h"
#include "Utils/AArch64BaseInfo.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "MCTargetDesc/AArch64FixupKinds.h"
#include "llvm/ADT/Triple.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MachO.h"

#include <keystone/keystone.h>

using namespace llvm_ks;

namespace {

class AArch64AsmBackend : public MCAsmBackend {
  static const unsigned PCRelFlagVal =
      MCFixupKindInfo::FKF_IsAlignedDownTo32Bits | MCFixupKindInfo::FKF_IsPCRel;
public:
  bool IsLittleEndian;

public:
  AArch64AsmBackend(const Target &T, bool IsLittleEndian)
     : MCAsmBackend(), IsLittleEndian(IsLittleEndian) {}

  unsigned getNumFixupKinds() const override {
    return AArch64::NumTargetFixupKinds;
  }

  const MCFixupKindInfo &getFixupKindInfo(MCFixupKind Kind) const override {
    const static MCFixupKindInfo Infos[AArch64::NumTargetFixupKinds] = {
      // This table *must* be in the order that the fixup_* kinds are defined in
      // AArch64FixupKinds.h.
      //
      // Name                           Offset (bits) Size (bits)     Flags
      { "fixup_aarch64_pcrel_adr_imm21", 0, 32, PCRelFlagVal },
      { "fixup_aarch64_pcrel_adrp_imm21", 0, 32, PCRelFlagVal },
      { "fixup_aarch64_add_imm12", 10, 12, 0 },
      { "fixup_aarch64_ldst_imm12_scale1", 10, 12, 0 },
      { "fixup_aarch64_ldst_imm12_scale2", 10, 12, 0 },
      { "fixup_aarch64_ldst_imm12_scale4", 10, 12, 0 },
      { "fixup_aarch64_ldst_imm12_scale8", 10, 12, 0 },
      { "fixup_aarch64_ldst_imm12_scale16", 10, 12, 0 },
      { "fixup_aarch64_ldr_pcrel_imm19", 5, 19, PCRelFlagVal },
      { "fixup_aarch64_movw", 5, 16, 0 },
      { "fixup_aarch64_pcrel_branch14", 5, 14, PCRelFlagVal },
      { "fixup_aarch64_pcrel_branch19", 5, 19, PCRelFlagVal },
      { "fixup_aarch64_pcrel_branch26", 0, 26, PCRelFlagVal },
      { "fixup_aarch64_pcrel_call26", 0, 26, PCRelFlagVal },
      { "fixup_aarch64_tlsdesc_call", 0, 0, 0 }
    };

    if (Kind < FirstTargetFixupKind)
      return MCAsmBackend::getFixupKindInfo(Kind);

    assert(unsigned(Kind - FirstTargetFixupKind) < getNumFixupKinds() &&
           "Invalid kind!");
    return Infos[Kind - FirstTargetFixupKind];
  }

  void applyFixup(const MCFixup &Fixup, char *Data, unsigned DataSize,
                  uint64_t Value, bool IsPCRel, unsigned int &KsError) const override;

  bool mayNeedRelaxation(const MCInst &Inst) const override;
  bool fixupNeedsRelaxation(const MCFixup &Fixup, uint64_t Value,
                            const MCRelaxableFragment *DF,
                            const MCAsmLayout &Layout, unsigned &KsError) const override;
  void relaxInstruction(const MCInst &Inst, MCInst &Res) const override;
  bool writeNopData(uint64_t Count, MCObjectWriter *OW) const override;

  void HandleAssemblerFlag(MCAssemblerFlag Flag) {}

  unsigned getPointerSize() const { return 8; }

  unsigned getFixupKindContainereSizeInBytes(unsigned Kind) const;
};

} // end anonymous namespace

/// \brief The number of bytes the fixup may change.
static unsigned getFixupKindNumBytes(unsigned Kind) {
  switch (Kind) {
  default:
    llvm_unreachable("Unknown fixup kind!");

  case AArch64::fixup_aarch64_tlsdesc_call:
    return 0;

  case FK_Data_1:
    return 1;

  case FK_Data_2:
  case AArch64::fixup_aarch64_movw:
    return 2;

  case AArch64::fixup_aarch64_pcrel_branch14:
  case AArch64::fixup_aarch64_add_imm12:
  case AArch64::fixup_aarch64_ldst_imm12_scale1:
  case AArch64::fixup_aarch64_ldst_imm12_scale2:
  case AArch64::fixup_aarch64_ldst_imm12_scale4:
  case AArch64::fixup_aarch64_ldst_imm12_scale8:
  case AArch64::fixup_aarch64_ldst_imm12_scale16:
  case AArch64::fixup_aarch64_ldr_pcrel_imm19:
  case AArch64::fixup_aarch64_pcrel_branch19:
    return 3;

  case AArch64::fixup_aarch64_pcrel_adr_imm21:
  case AArch64::fixup_aarch64_pcrel_adrp_imm21:
  case AArch64::fixup_aarch64_pcrel_branch26:
  case AArch64::fixup_aarch64_pcrel_call26:
  case FK_Data_4:
    return 4;

  case FK_Data_8:
    return 8;
  }
}

static unsigned AdrImmBits(unsigned Value) {
  unsigned lo2 = Value & 0x3;
  unsigned hi19 = (Value & 0x1ffffc) >> 2;
  return (hi19 << 5) | (lo2 << 29);
}

static bool isValidFixupValue(unsigned Kind, uint64_t Value) {
  int64_t SignedValue = static_cast<int64_t>(Value);
  switch (Kind) {
  default:
    return false;
  case AArch64::fixup_aarch64_pcrel_adr_imm21:
    if (SignedValue > 2097151 || SignedValue < -2097152)
      return false;
    return true;
  case AArch64::fixup_aarch64_pcrel_adrp_imm21:
    return true;
  case AArch64::fixup_aarch64_ldr_pcrel_imm19:
  case AArch64::fixup_aarch64_pcrel_branch19:
    // Signed 21-bit immediate
    if (SignedValue > 2097151 || SignedValue < -2097152)
      return false;
    // Low two bits are not encoded.
    return true;
  case AArch64::fixup_aarch64_add_imm12:
  case AArch64::fixup_aarch64_ldst_imm12_scale1:
    // Unsigned 12-bit immediate
    if (Value >= 0x1000)
      return false;
    return true;
  case AArch64::fixup_aarch64_ldst_imm12_scale2:
    // Unsigned 12-bit immediate which gets multiplied by 2
    if (Value & 1 || Value >= 0x2000)
      return false;
    return true;
  case AArch64::fixup_aarch64_ldst_imm12_scale4:
    // Unsigned 12-bit immediate which gets multiplied by 4
    if (Value & 3 || Value >= 0x4000)
      return false;
    return true;
  case AArch64::fixup_aarch64_ldst_imm12_scale8:
    // Unsigned 12-bit immediate which gets multiplied by 8
    if (Value & 7 || Value >= 0x8000)
      return false;
    return true;
  case AArch64::fixup_aarch64_ldst_imm12_scale16:
    // Unsigned 12-bit immediate which gets multiplied by 16
    if (Value & 15 || Value >= 0x10000)
      return false;
    return true;
  case AArch64::fixup_aarch64_movw:
    return false;
  case AArch64::fixup_aarch64_pcrel_branch14:
    // Signed 16-bit immediate
    if (SignedValue > 32767 || SignedValue < -32768)
      return false;
    // Low two bits are not encoded (4-byte alignment assumed).
    if (Value & 0x3)
      return false;
    return true;
  case AArch64::fixup_aarch64_pcrel_branch26:
  case AArch64::fixup_aarch64_pcrel_call26:
    // Signed 28-bit immediate
    if (SignedValue > 134217727 || SignedValue < -134217728)
      return false;
    // Low two bits are not encoded (4-byte alignment assumed).
    if (Value & 0x3)
      return false;
    return true;
  case FK_Data_1:
  case FK_Data_2:
  case FK_Data_4:
  case FK_Data_8:
    return true;
  }
}

static uint64_t adjustFixupValue(unsigned Kind, uint64_t Value) {
  int64_t SignedValue = static_cast<int64_t>(Value);
  switch (Kind) {
  default:
    llvm_unreachable("Unknown fixup kind!");
  case AArch64::fixup_aarch64_pcrel_adr_imm21:
    if (SignedValue > 2097151 || SignedValue < -2097152)
      report_fatal_error("fixup value out of range");
    return AdrImmBits(Value & 0x1fffffULL);
  case AArch64::fixup_aarch64_pcrel_adrp_imm21:
    return AdrImmBits((Value & 0x1fffff000ULL) >> 12);
  case AArch64::fixup_aarch64_ldr_pcrel_imm19:
  case AArch64::fixup_aarch64_pcrel_branch19:
    // Signed 21-bit immediate
    if (SignedValue > 2097151 || SignedValue < -2097152)
      report_fatal_error("fixup value out of range");
    // Low two bits are not encoded.
    return (Value >> 2) & 0x7ffff;
  case AArch64::fixup_aarch64_add_imm12:
  case AArch64::fixup_aarch64_ldst_imm12_scale1:
    // Unsigned 12-bit immediate
    if (Value >= 0x1000)
      report_fatal_error("invalid imm12 fixup value");
    return Value;
  case AArch64::fixup_aarch64_ldst_imm12_scale2:
    // Unsigned 12-bit immediate which gets multiplied by 2
    if (Value & 1 || Value >= 0x2000)
      report_fatal_error("invalid imm12 fixup value");
    return Value >> 1;
  case AArch64::fixup_aarch64_ldst_imm12_scale4:
    // Unsigned 12-bit immediate which gets multiplied by 4
    if (Value & 3 || Value >= 0x4000)
      report_fatal_error("invalid imm12 fixup value");
    return Value >> 2;
  case AArch64::fixup_aarch64_ldst_imm12_scale8:
    // Unsigned 12-bit immediate which gets multiplied by 8
    if (Value & 7 || Value >= 0x8000)
      report_fatal_error("invalid imm12 fixup value");
    return Value >> 3;
  case AArch64::fixup_aarch64_ldst_imm12_scale16:
    // Unsigned 12-bit immediate which gets multiplied by 16
    if (Value & 15 || Value >= 0x10000)
      report_fatal_error("invalid imm12 fixup value");
    return Value >> 4;
  case AArch64::fixup_aarch64_movw:
    report_fatal_error("no resolvable MOVZ/MOVK fixups supported yet");
    return Value;
  case AArch64::fixup_aarch64_pcrel_branch14:
    // Signed 16-bit immediate
    if (SignedValue > 32767 || SignedValue < -32768)
      report_fatal_error("fixup value out of range");
    // Low two bits are not encoded (4-byte alignment assumed).
    if (Value & 0x3)
      report_fatal_error("fixup not sufficiently aligned");
    return (Value >> 2) & 0x3fff;
  case AArch64::fixup_aarch64_pcrel_branch26:
  case AArch64::fixup_aarch64_pcrel_call26:
    // Signed 28-bit immediate
    if (SignedValue > 134217727 || SignedValue < -134217728)
      report_fatal_error("fixup value out of range");
    // Low two bits are not encoded (4-byte alignment assumed).
    if (Value & 0x3)
      report_fatal_error("fixup not sufficiently aligned");
    return (Value >> 2) & 0x3ffffff;
  case FK_Data_1:
  case FK_Data_2:
  case FK_Data_4:
  case FK_Data_8:
    return Value;
  }
}

/// getFixupKindContainereSizeInBytes - The number of bytes of the
/// container involved in big endian or 0 if the item is little endian
unsigned AArch64AsmBackend::getFixupKindContainereSizeInBytes(unsigned Kind) const {
  if (IsLittleEndian)
    return 0;

  switch (Kind) {
  default:
    llvm_unreachable("Unknown fixup kind!");

  case FK_Data_1:
    return 1;
  case FK_Data_2:
    return 2;
  case FK_Data_4:
    return 4;
  case FK_Data_8:
    return 8;

  case AArch64::fixup_aarch64_tlsdesc_call:
  case AArch64::fixup_aarch64_movw:
  case AArch64::fixup_aarch64_pcrel_branch14:
  case AArch64::fixup_aarch64_add_imm12:
  case AArch64::fixup_aarch64_ldst_imm12_scale1:
  case AArch64::fixup_aarch64_ldst_imm12_scale2:
  case AArch64::fixup_aarch64_ldst_imm12_scale4:
  case AArch64::fixup_aarch64_ldst_imm12_scale8:
  case AArch64::fixup_aarch64_ldst_imm12_scale16:
  case AArch64::fixup_aarch64_ldr_pcrel_imm19:
  case AArch64::fixup_aarch64_pcrel_branch19:
  case AArch64::fixup_aarch64_pcrel_adr_imm21:
  case AArch64::fixup_aarch64_pcrel_adrp_imm21:
  case AArch64::fixup_aarch64_pcrel_branch26:
  case AArch64::fixup_aarch64_pcrel_call26:
    // Instructions are always little endian
    return 0;
  }
}

void AArch64AsmBackend::applyFixup(const MCFixup &Fixup, char *Data,
                                   unsigned DataSize, uint64_t Value,
                                   bool IsPCRel, unsigned int &KsError) const {
  unsigned NumBytes = getFixupKindNumBytes(Fixup.getKind());
  if (!Value)
    return; // Doesn't change encoding.
  MCFixupKindInfo Info = getFixupKindInfo(Fixup.getKind());
  if (!isValidFixupValue(Fixup.getKind(), Value)) {
      KsError = KS_ERR_ASM_FIXUP_INVALID;
      return;
  }
  // Apply any target-specific value adjustments.
  Value = adjustFixupValue(Fixup.getKind(), Value);

  // Shift the value into position.
  Value <<= Info.TargetOffset;

  unsigned Offset = Fixup.getOffset();
  //assert(Offset + NumBytes <= DataSize && "Invalid fixup offset!");
  if (Offset + NumBytes > DataSize) {
      KsError = KS_ERR_ASM_FIXUP_INVALID;
      return;
  }

  // Used to point to big endian bytes.
  unsigned FulleSizeInBytes = getFixupKindContainereSizeInBytes(Fixup.getKind());

  // For each byte of the fragment that the fixup touches, mask in the
  // bits from the fixup value.
  if (FulleSizeInBytes == 0) {
    // Handle as little-endian
    for (unsigned i = 0; i != NumBytes; ++i) {
      Data[Offset + i] |= uint8_t((Value >> (i * 8)) & 0xff);
    }
  } else {
    // Handle as big-endian
    //assert((Offset + FulleSizeInBytes) <= DataSize && "Invalid fixup size!");
    //assert(NumBytes <= FulleSizeInBytes && "Invalid fixup size!");
    if ((Offset + FulleSizeInBytes) > DataSize ||
            NumBytes > FulleSizeInBytes) {
        KsError = KS_ERR_ASM_FIXUP_INVALID;
        return;
    }
    for (unsigned i = 0; i != NumBytes; ++i) {
      unsigned Idx = FulleSizeInBytes - 1 - i;
      Data[Offset + Idx] |= uint8_t((Value >> (i * 8)) & 0xff);
    }
  }
}

bool AArch64AsmBackend::mayNeedRelaxation(const MCInst &Inst) const {
  return false;
}

bool AArch64AsmBackend::fixupNeedsRelaxation(const MCFixup &Fixup,
                                             uint64_t Value,
                                             const MCRelaxableFragment *DF,
                                             const MCAsmLayout &Layout, unsigned &KsError) const {
  // FIXME:  This isn't correct for AArch64. Just moving the "generic" logic
  // into the targets for now.
  //
  // Relax if the value is too big for a (signed) i8.
  return int64_t(Value) != int64_t(int8_t(Value));
}

void AArch64AsmBackend::relaxInstruction(const MCInst &Inst,
                                         MCInst &Res) const {
  llvm_unreachable("AArch64AsmBackend::relaxInstruction() unimplemented");
}

bool AArch64AsmBackend::writeNopData(uint64_t Count, MCObjectWriter *OW) const {
  // If the count is not 4-byte aligned, we must be writing data into the text
  // section (otherwise we have unaligned instructions, and thus have far
  // bigger problems), so just write zeros instead.
  OW->WriteZeros(Count % 4);

  // We are properly aligned, so write NOPs as requested.
  Count /= 4;
  for (uint64_t i = 0; i != Count; ++i)
    OW->write32(0xd503201f);
  return true;
}

namespace {

namespace CU {

/// \brief Compact unwind encoding values.
enum CompactUnwindEncodings {
  /// \brief A "frameless" leaf function, where no non-volatile registers are
  /// saved. The return remains in LR throughout the function.
  UNWIND_AArch64_MODE_FRAMELESS = 0x02000000,

  /// \brief No compact unwind encoding available. Instead the low 23-bits of
  /// the compact unwind encoding is the offset of the DWARF FDE in the
  /// __eh_frame section. This mode is never used in object files. It is only
  /// generated by the linker in final linked images, which have only DWARF info
  /// for a function.
  UNWIND_AArch64_MODE_DWARF = 0x03000000,

  /// \brief This is a standard arm64 prologue where FP/LR are immediately
  /// pushed on the stack, then SP is copied to FP. If there are any
  /// non-volatile register saved, they are copied into the stack fame in pairs
  /// in a contiguous ranger right below the saved FP/LR pair. Any subset of the
  /// five X pairs and four D pairs can be saved, but the memory layout must be
  /// in register number order.
  UNWIND_AArch64_MODE_FRAME = 0x04000000,

  /// \brief Frame register pair encodings.
  UNWIND_AArch64_FRAME_X19_X20_PAIR = 0x00000001,
  UNWIND_AArch64_FRAME_X21_X22_PAIR = 0x00000002,
  UNWIND_AArch64_FRAME_X23_X24_PAIR = 0x00000004,
  UNWIND_AArch64_FRAME_X25_X26_PAIR = 0x00000008,
  UNWIND_AArch64_FRAME_X27_X28_PAIR = 0x00000010,
  UNWIND_AArch64_FRAME_D8_D9_PAIR = 0x00000100,
  UNWIND_AArch64_FRAME_D10_D11_PAIR = 0x00000200,
  UNWIND_AArch64_FRAME_D12_D13_PAIR = 0x00000400,
  UNWIND_AArch64_FRAME_D14_D15_PAIR = 0x00000800
};

} // end CU namespace

} // end anonymous namespace

namespace {

class ELFAArch64AsmBackend : public AArch64AsmBackend {
public:
  uint8_t OSABI;

  ELFAArch64AsmBackend(const Target &T, uint8_t OSABI, bool IsLittleEndian)
    : AArch64AsmBackend(T, IsLittleEndian), OSABI(OSABI) {}

  MCObjectWriter *createObjectWriter(raw_pwrite_stream &OS) const override {
    return createAArch64ELFObjectWriter(OS, OSABI, IsLittleEndian);
  }

  void processFixupValue(const MCAssembler &Asm, const MCAsmLayout &Layout,
                         const MCFixup &Fixup, const MCFragment *DF,
                         const MCValue &Target, uint64_t &Value,
                         bool &IsResolved) override;
};

void ELFAArch64AsmBackend::processFixupValue(
    const MCAssembler &Asm, const MCAsmLayout &Layout, const MCFixup &Fixup,
    const MCFragment *DF, const MCValue &Target, uint64_t &Value,
    bool &IsResolved) {
  // The ADRP instruction adds some multiple of 0x1000 to the current PC &
  // ~0xfff. This means that the required offset to reach a symbol can vary by
  // up to one step depending on where the ADRP is in memory. For example:
  //
  //     ADRP x0, there
  //  there:
  //
  // If the ADRP occurs at address 0xffc then "there" will be at 0x1000 and
  // we'll need that as an offset. At any other address "there" will be in the
  // same page as the ADRP and the instruction should encode 0x0. Assuming the
  // section isn't 0x1000-aligned, we therefore need to delegate this decision
  // to the linker -- a relocation!
  if ((uint32_t)Fixup.getKind() == AArch64::fixup_aarch64_pcrel_adrp_imm21)
    IsResolved = false;
}

}

MCAsmBackend *llvm_ks::createAArch64leAsmBackend(const Target &T,
                                              const MCRegisterInfo &MRI,
                                              const Triple &TheTriple,
                                              StringRef CPU) {
  assert(TheTriple.isOSBinFormatELF() && "Expect either MachO or ELF target");
  uint8_t OSABI = MCELFObjectTargetWriter::getOSABI(TheTriple.getOS());
  return new ELFAArch64AsmBackend(T, OSABI, /*IsLittleEndian=*/true);
}

MCAsmBackend *llvm_ks::createAArch64beAsmBackend(const Target &T,
                                              const MCRegisterInfo &MRI,
                                              const Triple &TheTriple,
                                              StringRef CPU) {
  assert(TheTriple.isOSBinFormatELF() &&
         "Big endian is only supported for ELF targets!");
  uint8_t OSABI = MCELFObjectTargetWriter::getOSABI(TheTriple.getOS());
  return new ELFAArch64AsmBackend(T, OSABI,
                                  /*IsLittleEndian=*/false);
}
