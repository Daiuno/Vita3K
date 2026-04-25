// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

// M12: PPSSPP-style IR cache-interpreter for Vita3K.
//
// Unlike a hand-written ARMv7 interpreter, this backend reuses Dynarmic's
// A32 -> IR translation pipeline (including optimization passes) and only
// implements an evaluator over the IR::Opcode enum. That means all Thumb/ARM
// decoding, IT-block handling, VFP/NEON register aliasing, conditional
// execution, and exclusive-monitor semantics are inherited from Dynarmic
// for free; what this backend owns is just the IR evaluator, a location
// -> IR::Block cache, and a CPU state store.
//
// Coverage status (M12.3 / M12.4):
//   - Integer ALU (Add/Sub/Mul/And/Or/Eor/Not/Shifts/Extends/ByteRev/CLZ)
//   - A32 state (GetRegister/SetRegister/GetCpsr/SetCpsr*, BXWritePC)
//   - Memory (ReadMemory8/16/32/64 + WriteMemory* + Exclusive variants +
//     ClearExclusive + DataMemoryBarrier)
//   - Condition / select / pseudo-ops (GetCarryFromOp/GetOverflowFromOp/
//     GetNZCVFromOp/GetNZFromOp/GetCFlagFromNZCV/ConditionalSelect32/...)
//   - Exceptions (CallSupervisor, ExceptionRaised, Breakpoint)
//   - All FP/NEON/Vector opcodes trap into a diagnostic UNIMPLEMENTED path
//     that halts the CPU cleanly (M12.5 will flesh them out).
//
// Any inst whose opcode hits the default branch of the switch causes the
// CPU to halt with an error including block PC + opcode name, so games
// needing unimplemented ops fail loudly rather than silently misbehaving.

#include <cpu/common.h>
#include <cpu/functions.h>
#include <cpu/impl/interface.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid leaking Dynarmic headers to consumers.
namespace Dynarmic {
class ExclusiveMonitor;
namespace IR {
class Block;
}
}  // namespace Dynarmic

class ArmInterpCallback;
class ArmDynarmicCP15;

// Internal types used by the interpreter -- forward-declared here so that the
// header doesn't need to pull in <boost/variant.hpp> or Dynarmic::IR::Term.
// Full definitions live in ir_interpreter_cpu.cpp.
struct CachedBlock;
struct IRValue;

class IRInterpreterCPU : public CPUInterface {
    friend class ArmInterpCallback;

protected:
    // NOTE: these are `protected` (not private) so that the anonymous-namespace
    // helpers in ir_interpreter_cpu.cpp can reach them via a trivial derived
    // "Friend" struct pattern without sprinkling friend declarations for every
    // local function.
    CPUState *parent;

    // Architectural state -- the interpreter owns the canonical copy.
    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 64> ext_regs{}; // S0..S31 / D0..D31 (shared banking)
    uint32_t cpsr = 0x000001D0; // ARM mode, interrupts masked (matches DynarmicCPU initial state)
    uint32_t fpscr = 0;
    uint32_t exclusive_state = 0; // 0 = no exclusive tag active
    bool check_bit = false;       // set via A32SetCheckBit, consumed by Term::CheckBit

    // Callback bridge (memory + SVC + exceptions).
    std::unique_ptr<ArmInterpCallback> cb;
    std::shared_ptr<ArmDynarmicCP15> cp15;
    Dynarmic::ExclusiveMonitor *monitor;

    std::size_t core_id = 0;

    // Halt / break flags consulted by the outer Run() loop.
    std::atomic<bool> halt_requested{false};
    bool halted = false;
    bool break_ = false;

    bool log_mem = false;
    bool log_code = false;

    // M12.7.3 hotfix -- log-rate-limit + runaway guard:
    //
    // When guest code goes off the rails (e.g. module init failure leaves the
    // heap uninitialised) it can produce thousands of Invalid read/write hits
    // per second.  Previously every hit went through spdlog with a formatted
    // 13-register dump, which (a) saturated the CPU in log formatting and
    // (b) kept the per-block halt_requested check from firing promptly, so
    // retro_unload_game never landed.
    //
    // `mem_fault_log_budget` throttles logging after the first kMemFaultLogCap
    // reports.  `consecutive_mem_faults` counts back-to-back invalid accesses
    // with no successful memory op in between; once it crosses
    // kRunawayMemFaultLimit we flag the CPU as halted so the thread unwinds
    // cleanly even without a frontend retro_unload_game.
    static constexpr uint32_t kMemFaultLogCap = 16;
    static constexpr uint32_t kRunawayMemFaultLimit = 4096;
    uint32_t mem_fault_log_budget = kMemFaultLogCap;
    uint32_t consecutive_mem_faults = 0;

    // Block cache keyed on A32::LocationDescriptor::UniqueHash().  Value is a
    // CachedBlock (IR::Block + pre-analyzed terminal + direct-link pointer
    // cache).  We wrap in unique_ptr so rehashing is cheap and so that
    // link_cached pointers stored inside each CachedBlock remain stable for
    // the block's lifetime.
    std::unordered_map<uint64_t, std::unique_ptr<CachedBlock>> block_cache;

    // L0 direct-mapped cache used by the hot dispatch loop to avoid the
    // unordered_map lookup when we return to one of the recently executed
    // blocks (common in tight loops and post-SVC re-entry).
    //
    // Indexed by (key & (kL0Size - 1)); each slot stores the full 64-bit key
    // + pointer to a CachedBlock owned by block_cache.  An empty slot is
    // denoted by key=0, ptr=nullptr.  The array is flushed whenever the
    // code cache is invalidated.
    static constexpr std::size_t kL0Size = 32;
    std::array<std::pair<uint64_t, CachedBlock *>, kL0Size> l0_cache{};

    // Reusable SSA value vector.  Grows to fit the largest block we've run;
    // we deliberately do NOT zero-initialize between blocks -- each inst in
    // a block writes its dst before any later inst reads it, and the read
    // sites resolve immediates and identities before touching this array.
    std::vector<IRValue> reusable_values;

    CachedBlock *get_or_translate_block_cached(uint32_t pc, bool thumb);
    CachedBlock *lookup_cached_block(uint64_t key);
    int run_block_body(CachedBlock &cb);

public:
    IRInterpreterCPU(CPUState *state, std::size_t processor_id,
                     Dynarmic::ExclusiveMonitor *monitor);
    ~IRInterpreterCPU() override;

    int run() override;
    int step() override;
    void stop() override;

    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t val) override;

    uint32_t get_sp() override;
    void set_sp(uint32_t val) override;

    uint32_t get_pc() override;
    void set_pc(uint32_t val) override;

    uint32_t get_lr() override;
    void set_lr(uint32_t val) override;

    uint32_t get_cpsr() override;
    void set_cpsr(uint32_t val) override;

    uint32_t get_tpidruro() override;
    void set_tpidruro(uint32_t val) override;

    float get_float_reg(uint8_t idx) override;
    void set_float_reg(uint8_t idx, float val) override;

    uint32_t get_fpscr() override;
    void set_fpscr(uint32_t val) override;

    CPUContext save_context() override;
    void load_context(const CPUContext &ctx) override;
    void invalidate_jit_cache(Address start, size_t length) override;

    bool is_thumb_mode() override;

    bool hit_breakpoint() override;
    void trigger_breakpoint() override;
    void set_log_code(bool log) override;
    void set_log_mem(bool log) override;
    bool get_log_code() override;
    bool get_log_mem() override;

    std::size_t processor_id() const override;
};
