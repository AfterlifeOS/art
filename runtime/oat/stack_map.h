/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_OAT_STACK_MAP_H_
#define ART_RUNTIME_OAT_STACK_MAP_H_

#include <limits>

#include "arch/instruction_set.h"
#include "base/array_ref.h"
#include "base/bit_memory_region.h"
#include "base/bit_table.h"
#include "base/bit_utils.h"
#include "base/globals.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory_region.h"
#include "dex/dex_file_types.h"
#include "dex_register_location.h"
#include "quick/quick_method_frame_info.h"

namespace art HIDDEN {

namespace linker {
class CodeInfoTableDeduper;
}  //  namespace linker

class OatQuickMethodHeader;
class VariableIndentationOutputStream;

// Size of a frame slot, in bytes.  This constant is a signed value,
// to please the compiler in arithmetic operations involving int32_t
// (signed) values.
static constexpr ssize_t kFrameSlotSize = 4;

// The delta compression of dex register maps means we need to scan the stackmaps backwards.
// We compress the data in such a way so that there is an upper bound on the search distance.
// Max distance 0 means each stack map must be fully defined and no scanning back is allowed.
// If this value is changed, the oat file version should be incremented (for DCHECK to pass).
static constexpr size_t kMaxDexRegisterMapSearchDistance = 32;

class ArtMethod;
class CodeInfo;
class Stats;

std::ostream& operator<<(std::ostream& stream, const DexRegisterLocation& reg);

// Information on Dex register locations for a specific PC.
// Effectively just a convenience wrapper for DexRegisterLocation vector.
// If the size is small enough, it keeps the data on the stack.
// TODO: Replace this with generic purpose "small-vector" implementation.
class DexRegisterMap {
 public:
  using iterator = DexRegisterLocation*;
  using const_iterator = const DexRegisterLocation*;

  // Create map for given number of registers and initialize them to the given value.
  DexRegisterMap(size_t count, DexRegisterLocation value) : count_(count), regs_small_{} {
    if (count_ <= kSmallCount) {
      std::fill_n(regs_small_.begin(), count, value);
    } else {
      regs_large_.resize(count, value);
    }
  }

  DexRegisterLocation* data() {
    return count_ <= kSmallCount ? regs_small_.data() : regs_large_.data();
  }
  const DexRegisterLocation* data() const {
    return count_ <= kSmallCount ? regs_small_.data() : regs_large_.data();
  }

  iterator begin() { return data(); }
  iterator end() { return data() + count_; }
  const_iterator begin() const { return data(); }
  const_iterator end() const { return data() + count_; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  DexRegisterLocation& operator[](size_t index) {
    DCHECK_LT(index, count_);
    return data()[index];
  }
  const DexRegisterLocation& operator[](size_t index) const {
    DCHECK_LT(index, count_);
    return data()[index];
  }

  size_t GetNumberOfLiveDexRegisters() const {
    return std::count_if(begin(), end(), [](auto& loc) { return loc.IsLive(); });
  }

  bool HasAnyLiveDexRegisters() const {
    return std::any_of(begin(), end(), [](auto& loc) { return loc.IsLive(); });
  }

  void Dump(VariableIndentationOutputStream* vios) const;

 private:
  // Store the data inline if the number of registers is small to avoid memory allocations.
  // If count_ <= kSmallCount, we use the regs_small_ array, and regs_large_ otherwise.
  static constexpr size_t kSmallCount = 16;
  size_t count_;
  std::array<DexRegisterLocation, kSmallCount> regs_small_;
  dchecked_vector<DexRegisterLocation> regs_large_;
};

/**
 * A Stack Map holds compilation information for a specific PC necessary for:
 * - Mapping it to a dex PC,
 * - Knowing which stack entries are objects,
 * - Knowing which registers hold objects,
 * - Knowing the inlining information,
 * - Knowing the values of dex registers.
 */
class StackMap : public BitTableAccessor<8> {
 public:
  enum Kind {
    Default = -1,
    Catch = 0,
    OSR = 1,
    Debug = 2,
  };
  BIT_TABLE_HEADER(StackMap)
  BIT_TABLE_COLUMN(0, Kind)
  BIT_TABLE_COLUMN(1, PackedNativePc)
  BIT_TABLE_COLUMN(2, DexPc)
  BIT_TABLE_COLUMN(3, RegisterMaskIndex)
  BIT_TABLE_COLUMN(4, StackMaskIndex)
  BIT_TABLE_COLUMN(5, InlineInfoIndex)
  BIT_TABLE_COLUMN(6, DexRegisterMaskIndex)
  BIT_TABLE_COLUMN(7, DexRegisterMapIndex)

  ALWAYS_INLINE uint32_t GetNativePcOffset(InstructionSet instruction_set) const {
    return UnpackNativePc(GetPackedNativePc(), instruction_set);
  }

  ALWAYS_INLINE bool HasInlineInfo() const {
    return HasInlineInfoIndex();
  }

  ALWAYS_INLINE bool HasDexRegisterMap() const {
    return HasDexRegisterMapIndex();
  }

  static uint32_t PackNativePc(uint32_t native_pc, InstructionSet isa) {
    DCHECK_ALIGNED_PARAM(native_pc, GetInstructionSetInstructionAlignment(isa));
    return native_pc / GetInstructionSetInstructionAlignment(isa);
  }

  static uint32_t UnpackNativePc(uint32_t packed_native_pc, InstructionSet isa) {
    uint32_t native_pc = packed_native_pc * GetInstructionSetInstructionAlignment(isa);
    DCHECK_EQ(native_pc / GetInstructionSetInstructionAlignment(isa), packed_native_pc);
    return native_pc;
  }

  EXPORT void Dump(VariableIndentationOutputStream* vios,
                   const CodeInfo& code_info,
                   uint32_t code_offset,
                   InstructionSet instruction_set) const;
};

/**
 * Inline information for a specific PC.
 * The row referenced from the StackMap holds information at depth 0.
 * Following rows hold information for further depths.
 */
class InlineInfo : public BitTableAccessor<6> {
 public:
  BIT_TABLE_HEADER(InlineInfo)
  BIT_TABLE_COLUMN(0, IsLast)  // Determines if there are further rows for further depths.
  BIT_TABLE_COLUMN(1, DexPc)
  BIT_TABLE_COLUMN(2, MethodInfoIndex)
  BIT_TABLE_COLUMN(3, ArtMethodHi)  // High bits of ArtMethod*.
  BIT_TABLE_COLUMN(4, ArtMethodLo)  // Low bits of ArtMethod*.
  BIT_TABLE_COLUMN(5, NumberOfDexRegisters)  // Includes outer levels and the main method.

  static constexpr uint32_t kLast = -1;
  static constexpr uint32_t kMore = 0;

  bool EncodesArtMethod() const {
    return HasArtMethodLo();
  }

  ArtMethod* GetArtMethod() const {
    uint64_t lo = GetArtMethodLo();
    uint64_t hi = GetArtMethodHi();
    return reinterpret_cast<ArtMethod*>((hi << 32) | lo);
  }

  void Dump(VariableIndentationOutputStream* vios,
            const CodeInfo& info,
            const StackMap& stack_map) const;
};

class StackMask : public BitTableAccessor<1> {
 public:
  BIT_TABLE_HEADER(StackMask)
  BIT_TABLE_COLUMN(0, Mask)
};

class DexRegisterMask : public BitTableAccessor<1> {
 public:
  BIT_TABLE_HEADER(DexRegisterMask)
  BIT_TABLE_COLUMN(0, Mask)
};

class DexRegisterMapInfo : public BitTableAccessor<1> {
 public:
  BIT_TABLE_HEADER(DexRegisterMapInfo)
  BIT_TABLE_COLUMN(0, CatalogueIndex)
};

class DexRegisterInfo : public BitTableAccessor<2> {
 public:
  BIT_TABLE_HEADER(DexRegisterInfo)
  BIT_TABLE_COLUMN(0, Kind)
  BIT_TABLE_COLUMN(1, PackedValue)

  ALWAYS_INLINE DexRegisterLocation GetLocation() const {
    DexRegisterLocation::Kind kind = static_cast<DexRegisterLocation::Kind>(GetKind());
    return DexRegisterLocation(kind, UnpackValue(kind, GetPackedValue()));
  }

  static uint32_t PackValue(DexRegisterLocation::Kind kind, uint32_t value) {
    uint32_t packed_value = value;
    if (kind == DexRegisterLocation::Kind::kInStack) {
      DCHECK(IsAligned<kFrameSlotSize>(packed_value));
      packed_value /= kFrameSlotSize;
    }
    return packed_value;
  }

  static uint32_t UnpackValue(DexRegisterLocation::Kind kind, uint32_t packed_value) {
    uint32_t value = packed_value;
    if (kind == DexRegisterLocation::Kind::kInStack) {
      value *= kFrameSlotSize;
    }
    return value;
  }
};

// Register masks tend to have many trailing zero bits (caller-saves are usually not encoded),
// therefore it is worth encoding the mask as value+shift.
class RegisterMask : public BitTableAccessor<2> {
 public:
  BIT_TABLE_HEADER(RegisterMask)
  BIT_TABLE_COLUMN(0, Value)
  BIT_TABLE_COLUMN(1, Shift)

  ALWAYS_INLINE uint32_t GetMask() const {
    return GetValue() << GetShift();
  }
};

// Method indices are not very dedup friendly.
// Separating them greatly improves dedup efficiency of the other tables.
class MethodInfo : public BitTableAccessor<3> {
 public:
  BIT_TABLE_HEADER(MethodInfo)
  BIT_TABLE_COLUMN(0, MethodIndex)
  BIT_TABLE_COLUMN(1, DexFileIndexKind)
  BIT_TABLE_COLUMN(2, DexFileIndex)

  static constexpr uint32_t kKindNonBCP = -1;
  static constexpr uint32_t kKindBCP = 0;

  static constexpr uint32_t kSameDexFile = -1;
};

/**
 * Wrapper around all compiler information collected for a method.
 * See the Decode method at the end for the precise binary format.
 */
class CodeInfo {
 public:
  ALWAYS_INLINE CodeInfo() {}
  EXPORT ALWAYS_INLINE explicit CodeInfo(const uint8_t* data, size_t* num_read_bits = nullptr);
  EXPORT ALWAYS_INLINE explicit CodeInfo(const OatQuickMethodHeader* header);

  // The following methods decode only part of the data.
  static CodeInfo DecodeGcMasksOnly(const OatQuickMethodHeader* header);
  static CodeInfo DecodeInlineInfoOnly(const OatQuickMethodHeader* header);

  ALWAYS_INLINE static uint32_t DecodeCodeSize(const uint8_t* code_info_data) {
    return DecodeHeaderOnly(code_info_data).code_size_;
  }

  ALWAYS_INLINE static QuickMethodFrameInfo DecodeFrameInfo(const uint8_t* code_info_data) {
    CodeInfo code_info = DecodeHeaderOnly(code_info_data);
    return QuickMethodFrameInfo(code_info.packed_frame_size_ * kStackAlignment,
                                code_info.core_spill_mask_,
                                code_info.fp_spill_mask_);
  }

  ALWAYS_INLINE static CodeInfo DecodeHeaderOnly(const uint8_t* code_info_data) {
    CodeInfo code_info;
    BitMemoryReader reader(code_info_data);
    std::array<uint32_t, kNumHeaders> header = reader.ReadInterleavedVarints<kNumHeaders>();
    ForEachHeaderField([&code_info, &header](size_t i, auto member_pointer) {
      code_info.*member_pointer = header[i];
    });
    return code_info;
  }

  ALWAYS_INLINE const BitTable<StackMap>& GetStackMaps() const {
    return stack_maps_;
  }

  ALWAYS_INLINE StackMap GetStackMapAt(size_t index) const {
    return stack_maps_.GetRow(index);
  }

  BitMemoryRegion GetStackMask(size_t index) const {
    return stack_masks_.GetBitMemoryRegion(index);
  }

  BitMemoryRegion GetStackMaskOf(const StackMap& stack_map) const {
    uint32_t index = stack_map.GetStackMaskIndex();
    return (index == StackMap::kNoValue) ? BitMemoryRegion() : GetStackMask(index);
  }

  uint32_t GetRegisterMaskOf(const StackMap& stack_map) const {
    uint32_t index = stack_map.GetRegisterMaskIndex();
    return (index == StackMap::kNoValue) ? 0 : register_masks_.GetRow(index).GetMask();
  }

  uint32_t GetNumberOfLocationCatalogEntries() const {
    return dex_register_catalog_.NumRows();
  }

  ALWAYS_INLINE DexRegisterLocation GetDexRegisterCatalogEntry(size_t index) const {
    return (index == StackMap::kNoValue)
      ? DexRegisterLocation::None()
      : dex_register_catalog_.GetRow(index).GetLocation();
  }

  bool HasInlineInfo() const {
    return inline_infos_.NumRows() > 0;
  }

  uint32_t GetNumberOfStackMaps() const {
    return stack_maps_.NumRows();
  }

  MethodInfo GetMethodInfoOf(InlineInfo inline_info) const {
    return method_infos_.GetRow(inline_info.GetMethodInfoIndex());
  }

  uint32_t GetMethodIndexOf(InlineInfo inline_info) const {
    return GetMethodInfoOf(inline_info).GetMethodIndex();
  }

  // Returns the dex registers for `stack_map`, ignoring any inlined dex registers.
  ALWAYS_INLINE DexRegisterMap GetDexRegisterMapOf(StackMap stack_map) const {
    return GetDexRegisterMapOf(stack_map, /* first= */ 0, number_of_dex_registers_);
  }

  // Returns the dex register map of `inline_info`, and just those registers.
  ALWAYS_INLINE DexRegisterMap GetInlineDexRegisterMapOf(StackMap stack_map,
                                                         InlineInfo inline_info) const {
    if (stack_map.HasDexRegisterMap()) {
      DCHECK(stack_map.HasInlineInfoIndex());
      uint32_t depth = inline_info.Row() - stack_map.GetInlineInfoIndex();
      // The register counts are commutative and include all outer levels.
      // This allows us to determine the range [first, last) in just two lookups.
      // If we are at depth 0 (the first inlinee), the count from the main method is used.
      uint32_t first = (depth == 0)
          ? number_of_dex_registers_
          : inline_infos_.GetRow(inline_info.Row() - 1).GetNumberOfDexRegisters();
      uint32_t last = inline_info.GetNumberOfDexRegisters();
      return GetDexRegisterMapOf(stack_map, first, last);
    }
    return DexRegisterMap(0, DexRegisterLocation::None());
  }

  // Returns the dex register map of `stack_map` in the range the range [first, last).
  ALWAYS_INLINE DexRegisterMap GetDexRegisterMapOf(StackMap stack_map,
                                                   uint32_t first,
                                                   uint32_t last) const {
    if (stack_map.HasDexRegisterMap()) {
      DCHECK_LE(first, last);
      DexRegisterMap map(last - first, DexRegisterLocation::Invalid());
      DecodeDexRegisterMap(stack_map.Row(), first, &map);
      return map;
    }
    return DexRegisterMap(0, DexRegisterLocation::None());
  }

  BitTableRange<InlineInfo> GetInlineInfosOf(StackMap stack_map) const {
    uint32_t index = stack_map.GetInlineInfoIndex();
    if (index != StackMap::kNoValue) {
      auto begin = inline_infos_.begin() + index;
      auto end = begin;
      while ((*end++).GetIsLast() == InlineInfo::kMore) { }
      return BitTableRange<InlineInfo>(begin, end);
    } else {
      return BitTableRange<InlineInfo>();
    }
  }

  StackMap GetStackMapForDexPc(uint32_t dex_pc) const {
    for (StackMap stack_map : stack_maps_) {
      if (stack_map.GetDexPc() == dex_pc && stack_map.GetKind() != StackMap::Kind::Debug) {
        return stack_map;
      }
    }
    return stack_maps_.GetInvalidRow();
  }

  StackMap GetCatchStackMapForDexPc(ArrayRef<const uint32_t> dex_pcs) const {
    // Searches the stack map list backwards because catch stack maps are stored at the end.
    for (size_t i = GetNumberOfStackMaps(); i > 0; --i) {
      StackMap stack_map = GetStackMapAt(i - 1);
      if (UNLIKELY(stack_map.GetKind() != StackMap::Kind::Catch)) {
        // Early break since we should have catch stack maps only at the end.
        if (kIsDebugBuild) {
          for (size_t j = i - 1; j > 0; --j) {
            DCHECK(GetStackMapAt(j - 1).GetKind() != StackMap::Kind::Catch);
          }
        }
        break;
      }

      // Both the handler dex_pc and all of the inline dex_pcs have to match i.e. we want dex_pcs to
      // be [stack_map_dex_pc, inline_dex_pc_1, ..., inline_dex_pc_n].
      if (stack_map.GetDexPc() != dex_pcs.front()) {
        continue;
      }

      const BitTableRange<InlineInfo>& inline_infos = GetInlineInfosOf(stack_map);
      if (inline_infos.size() == dex_pcs.size() - 1) {
        bool matching_dex_pcs = true;
        for (size_t inline_info_index = 0; inline_info_index < inline_infos.size();
             ++inline_info_index) {
          if (inline_infos[inline_info_index].GetDexPc() != dex_pcs[inline_info_index + 1]) {
            matching_dex_pcs = false;
            break;
          }
        }
        if (matching_dex_pcs) {
          return stack_map;
        }
      }
    }
    return stack_maps_.GetInvalidRow();
  }

  StackMap GetOsrStackMapForDexPc(uint32_t dex_pc) const {
    for (StackMap stack_map : stack_maps_) {
      if (stack_map.GetDexPc() == dex_pc && stack_map.GetKind() == StackMap::Kind::OSR) {
        return stack_map;
      }
    }
    return stack_maps_.GetInvalidRow();
  }

  EXPORT StackMap GetStackMapForNativePcOffset(uintptr_t pc,
                                               InstructionSet isa = kRuntimeISA) const;

  // Dump this CodeInfo object on `vios`.
  // `code_offset` is the (absolute) native PC of the compiled method.
  EXPORT void Dump(VariableIndentationOutputStream* vios,
                   uint32_t code_offset,
                   bool verbose,
                   InstructionSet instruction_set) const;

  // Accumulate code info size statistics into the given Stats tree.
  EXPORT static void CollectSizeStats(const uint8_t* code_info, /*out*/ Stats& parent);

  template <uint32_t kFlag>
  ALWAYS_INLINE static bool HasFlag(const uint8_t* code_info_data) {
    // Fast path - read just the one specific bit from the header.
    bool result;
    uint8_t varint = (*code_info_data) & MaxInt<uint8_t>(kVarintBits);
    if (LIKELY(varint <= kVarintMax)) {
      result = (varint & kFlag) != 0;
    } else {
      DCHECK_EQ(varint, kVarintMax + 1);  // Only up to 8 flags are supported for now.
      constexpr uint32_t bit_offset = kNumHeaders * kVarintBits + WhichPowerOf2(kFlag);
      result = (code_info_data[bit_offset / kBitsPerByte] & (1 << bit_offset % kBitsPerByte)) != 0;
    }
    // Slow path - dcheck that we got the correct result against the naive implementation.
    BitMemoryReader reader(code_info_data);
    DCHECK_EQ(result, (reader.ReadInterleavedVarints<kNumHeaders>()[0] & kFlag) != 0);
    return result;
  }

  ALWAYS_INLINE static bool HasInlineInfo(const uint8_t* code_info_data) {
    return HasFlag<kHasInlineInfo>(code_info_data);
  }

  ALWAYS_INLINE static bool HasShouldDeoptimizeFlag(const uint8_t* code_info_data) {
    return HasFlag<kHasShouldDeoptimizeFlag>(code_info_data);
  }

  ALWAYS_INLINE static bool IsBaseline(const uint8_t* code_info_data) {
    return HasFlag<kIsBaseline>(code_info_data);
  }

  ALWAYS_INLINE static bool IsDebuggable(const uint8_t* code_info_data) {
    return HasFlag<kIsDebuggable>(code_info_data);
  }

  uint32_t GetNumberOfDexRegisters() {
    return number_of_dex_registers_;
  }

 private:
  // Scan backward to determine dex register locations at given stack map.
  EXPORT void DecodeDexRegisterMap(uint32_t stack_map_index,
                                   uint32_t first_dex_register,
                                   /*out*/ DexRegisterMap* map) const;

  template<typename DecodeCallback>  // (size_t index, BitTable<...>*, BitMemoryRegion).
  ALWAYS_INLINE CodeInfo(const uint8_t* data, size_t* num_read_bits, DecodeCallback callback);

  // Invokes the callback with index and member pointer of each header field.
  template<typename Callback>
  ALWAYS_INLINE static void ForEachHeaderField(Callback callback) {
    size_t index = 0;
    callback(index++, &CodeInfo::flags_);
    callback(index++, &CodeInfo::code_size_);
    callback(index++, &CodeInfo::packed_frame_size_);
    callback(index++, &CodeInfo::core_spill_mask_);
    callback(index++, &CodeInfo::fp_spill_mask_);
    callback(index++, &CodeInfo::number_of_dex_registers_);
    callback(index++, &CodeInfo::bit_table_flags_);
    DCHECK_EQ(index, kNumHeaders);
  }

  // Invokes the callback with index and member pointer of each BitTable field.
  template<typename Callback>
  ALWAYS_INLINE static void ForEachBitTableField(Callback callback) {
    size_t index = 0;
    callback(index++, &CodeInfo::stack_maps_);
    callback(index++, &CodeInfo::register_masks_);
    callback(index++, &CodeInfo::stack_masks_);
    callback(index++, &CodeInfo::inline_infos_);
    callback(index++, &CodeInfo::method_infos_);
    callback(index++, &CodeInfo::dex_register_masks_);
    callback(index++, &CodeInfo::dex_register_maps_);
    callback(index++, &CodeInfo::dex_register_catalog_);
    DCHECK_EQ(index, kNumBitTables);
  }

  bool HasBitTable(size_t i) { return ((bit_table_flags_ >> i) & 1) != 0; }
  bool IsBitTableDeduped(size_t i) { return ((bit_table_flags_ >> (kNumBitTables + i)) & 1) != 0; }
  void SetBitTableDeduped(size_t i) { bit_table_flags_ |= 1 << (kNumBitTables + i); }
  bool HasDedupedBitTables() { return (bit_table_flags_ >> kNumBitTables) != 0u; }

  // NB: The first three flags should be the most common ones.
  //     Maximum of 8 flags is supported right now (see the HasFlag method).
  enum Flags {
    kHasInlineInfo = 1 << 0,
    kHasShouldDeoptimizeFlag = 1 << 1,
    kIsBaseline = 1 << 2,
    kIsDebuggable = 1 << 3,
  };

  // The CodeInfo starts with sequence of variable-length bit-encoded integers.
  // (Please see kVarintMax for more details about encoding).
  static constexpr size_t kNumHeaders = 7;
  uint32_t flags_ = 0;
  uint32_t code_size_ = 0;  // The size of native PC range in bytes.
  uint32_t packed_frame_size_ = 0;  // Frame size in kStackAlignment units.
  uint32_t core_spill_mask_ = 0;
  uint32_t fp_spill_mask_ = 0;
  uint32_t number_of_dex_registers_ = 0;
  uint32_t bit_table_flags_ = 0;

  // The encoded bit-tables follow the header.  Based on the above flags field,
  // bit-tables might be omitted or replaced by relative bit-offset if deduped.
  static constexpr size_t kNumBitTables = 8;
  BitTable<StackMap> stack_maps_;
  BitTable<RegisterMask> register_masks_;
  BitTable<StackMask> stack_masks_;
  BitTable<InlineInfo> inline_infos_;
  BitTable<MethodInfo> method_infos_;
  BitTable<DexRegisterMask> dex_register_masks_;
  BitTable<DexRegisterMapInfo> dex_register_maps_;
  BitTable<DexRegisterInfo> dex_register_catalog_;

  friend class linker::CodeInfoTableDeduper;
  friend class StackMapStream;
};

#undef ELEMENT_BYTE_OFFSET_AFTER
#undef ELEMENT_BIT_OFFSET_AFTER

}  // namespace art

#endif  // ART_RUNTIME_OAT_STACK_MAP_H_
