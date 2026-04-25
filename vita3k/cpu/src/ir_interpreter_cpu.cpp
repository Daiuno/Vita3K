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

// M12: PPSSPP-style IR cache-interpreter.
//
// Architecture:
//   - A32 -> IR translation is delegated to Dynarmic::A32::Translate.
//   - Dynarmic optimization passes (dead-code elimination, identity removal,
//     naming pass) run on the resulting IR::Block before we cache it.
//   - Blocks are keyed by A32::LocationDescriptor::UniqueHash() in a
//     std::unordered_map owned by IRInterpreterCPU.
//   - Execution: while (!halted) { block = get_or_translate(pc); run_block; }
//     `run_block` iterates IR::Inst in order, writes results into an
//     SSA-indexed value array, then dispatches on the block's Terminal
//     to compute the next PC.
//
// Opcode coverage: see header comment in ir_interpreter_cpu.h for the
// status matrix. Every unimplemented opcode halts the CPU with a diagnostic
// (never silently mutates state).

#include <cpu/common.h>
#include <cpu/disasm/functions.h>
#include <cpu/functions.h>
#include <cpu/impl/ir_interpreter_cpu.h>
#include <cpu/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#ifdef LIBRETRO
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#endif
#include <util/bit_cast.h>
#include <util/log.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/frontend/A32/translate/a32_translate.h>
#include <dynarmic/frontend/A32/translate/translate_callbacks.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/cond.h>
#include <dynarmic/ir/microinstruction.h>
#include <dynarmic/ir/opcodes.h>
#include <dynarmic/ir/opt/passes.h>
#include <dynarmic/ir/terminal.h>
#include <dynarmic/ir/value.h>

#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/get.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
namespace Term = Dynarmic::IR::Term;
namespace Optimization = Dynarmic::Optimization;

// Re-use of the CP15 coprocessor shim declared in dynarmic_cpu.cpp. Defined
// there so that we don't duplicate the implementation; we only need the class
// name + the two public methods we touch (get/set tpidruro and the coprocessor
// interface). To keep linkage simple, we replicate a minimal local CP15
// class here -- the one in dynarmic_cpu.cpp is a separate TU-local symbol.
class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
    uint32_t tpidruro_;

public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    ArmDynarmicCP15()
        : tpidruro_(0) {}
    ~ArmDynarmicCP15() override = default;

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override {
        return CallbackOrAccessOneWord{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return CallbackOrAccessTwoWords{};
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro_;
        }
        return CallbackOrAccessOneWord{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        return CallbackOrAccessTwoWords{};
    }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }

    void set_tpidruro(uint32_t v) { tpidruro_ = v; }
    uint32_t get_tpidruro() const { return tpidruro_; }
};

// Callback bridge: memory + SVC + exceptions. Mirrors ArmDynarmicCallback in
// dynarmic_cpu.cpp but targets IRInterpreterCPU's state rather than the JIT's.
class ArmInterpCallback : public Dynarmic::A32::UserCallbacks {
public:
    CPUState *parent;
    IRInterpreterCPU *cpu;

    ArmInterpCallback(CPUState *parent_, IRInterpreterCPU *cpu_)
        : parent(parent_), cpu(cpu_) {}
    ~ArmInterpCallback() override = default;

    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr addr) override {
        return MemoryRead32(addr);
    }

    // M12.7.3 hotfix: route every Invalid read/write through this helper so
    // we can throttle logs and escalate to a halt when the guest is in a
    // pathological loop. Grants "Friend" access to the protected members of
    // IRInterpreterCPU (halted / halt_requested / counters).
    void report_mem_fault(bool is_write, unsigned bits, A32::VAddr addr,
                          uint64_t value) {
        struct Friend : public IRInterpreterCPU {
            using IRInterpreterCPU::halt_requested;
            using IRInterpreterCPU::halted;
            using IRInterpreterCPU::mem_fault_log_budget;
            using IRInterpreterCPU::consecutive_mem_faults;
            using IRInterpreterCPU::kMemFaultLogCap;
            using IRInterpreterCPU::kRunawayMemFaultLimit;
        };
        auto *self = static_cast<Friend *>(cpu);
        const uint32_t streak = ++self->consecutive_mem_faults;
        // One-shot: same guest PC across runs + Dynarmic-on-desktop working usually means an IR-evaluator
        // mismatch (not missing files). Gives a concrete insn line for issue reports / JIT A/B tests.
        if (streak == 1) {
            const uint32_t guest_pc = cpu->get_pc();
            LOG_WARN("[IR-interp] first fault in streak @PC 0x{:x}: {} "
                     "(compare with Dynarmic JIT on the same build if this PC advances there)",
                     guest_pc, disassemble(*parent, guest_pc, cpu->is_thumb_mode()));
        }
        if (self->mem_fault_log_budget > 0) {
            --self->mem_fault_log_budget;
            if (is_write) {
                LOG_ERROR("[IR-interp] Invalid write of uint{}_t at addr: 0x{:x}, val=0x{:x}\n{}",
                          bits, addr, value, cpu->save_context().description());
            } else {
                LOG_ERROR("[IR-interp] Invalid read of uint{}_t at addr: 0x{:x}\n{}",
                          bits, addr, cpu->save_context().description());
            }
            if (self->mem_fault_log_budget == 0) {
                LOG_WARN("[IR-interp] suppressing further memory-fault logs for "
                         "this run (>{} hits); consecutive streak will still "
                         "trigger a halt after {} misses.",
                         Friend::kMemFaultLogCap, Friend::kRunawayMemFaultLimit);
            }
        }
        if (streak >= Friend::kRunawayMemFaultLimit) {
            // Log once when crossing the threshold — streak keeps growing while the guest
            // tight-loops on invalid addresses, so avoid spamming the same ERROR line.
            if (streak == Friend::kRunawayMemFaultLimit) {
                LOG_ERROR("[IR-interp] runaway memory-fault loop detected "
                          "(>{} consecutive invalid accesses); halting CPU core {} "
                          "to let the frontend regain control.",
                          Friend::kRunawayMemFaultLimit,
                          static_cast<int>(cpu->processor_id()));
            }
            self->halted = true;
            self->halt_requested.store(true, std::memory_order_release);
        }
    }

    void clear_mem_fault_streak() {
        struct Friend : public IRInterpreterCPU {
            using IRInterpreterCPU::consecutive_mem_faults;
        };
        static_cast<Friend *>(cpu)->consecutive_mem_faults = 0;
    }

    template <typename T>
    T MemoryReadT(A32::VAddr addr) {
        Ptr<T> ptr{ addr };
        const Address a = ptr.address();
        const Address range_end = a + sizeof(T);
        if (!ptr || a < parent->mem->page_size || range_end < a || !is_valid_addr_range(*parent->mem, a, range_end)) {
            report_mem_fault(false, sizeof(T) * 8, addr, 0);
            return 0;
        }
        clear_mem_fault_streak();
#ifdef LIBRETRO
        ensure_guest_page_mapped_rw(*parent->mem, a);
#endif
        return *ptr.get(*parent->mem);
    }

    uint8_t MemoryRead8(A32::VAddr addr) override { return MemoryReadT<uint8_t>(addr); }
    uint16_t MemoryRead16(A32::VAddr addr) override { return MemoryReadT<uint16_t>(addr); }
    uint32_t MemoryRead32(A32::VAddr addr) override { return MemoryReadT<uint32_t>(addr); }
    uint64_t MemoryRead64(A32::VAddr addr) override { return MemoryReadT<uint64_t>(addr); }

    template <typename T>
    void MemoryWriteT(A32::VAddr addr, T value) {
        Ptr<T> ptr{ addr };
        const Address a = ptr.address();
        const Address range_end = a + sizeof(T);
        if (!ptr || a < parent->mem->page_size || range_end < a || !is_valid_addr_range(*parent->mem, a, range_end)) {
            report_mem_fault(true, sizeof(T) * 8, addr, static_cast<uint64_t>(value));
            return;
        }
        clear_mem_fault_streak();
#ifdef LIBRETRO
        ensure_guest_page_mapped_rw(*parent->mem, a);
#endif
#if defined(LIBRETRO) && defined(__APPLE__) && TARGET_OS_IOS
        sync_guest_write_if_protected(*parent->mem, a);
#endif
        *ptr.get(*parent->mem) = value;
    }

    void MemoryWrite8(A32::VAddr addr, uint8_t v) override { MemoryWriteT(addr, v); }
    void MemoryWrite16(A32::VAddr addr, uint16_t v) override { MemoryWriteT(addr, v); }
    void MemoryWrite32(A32::VAddr addr, uint32_t v) override { MemoryWriteT(addr, v); }
    void MemoryWrite64(A32::VAddr addr, uint64_t v) override { MemoryWriteT(addr, v); }

    template <typename T>
    bool MemoryWriteExclusiveT(A32::VAddr addr, T value, T expected) {
        Ptr<T> ptr{ addr };
        const Address a = ptr.address();
        const Address range_end = a + sizeof(T);
        if (!ptr || a < parent->mem->page_size || range_end < a || !is_valid_addr_range(*parent->mem, a, range_end)) {
            report_mem_fault(true, sizeof(T) * 8, addr, static_cast<uint64_t>(value));
            return false;
        }
        clear_mem_fault_streak();
#ifdef LIBRETRO
        ensure_guest_page_mapped_rw(*parent->mem, a);
#endif
#if defined(LIBRETRO) && defined(__APPLE__) && TARGET_OS_IOS
        sync_guest_write_if_protected(*parent->mem, a);
#endif
        return Ptr<T>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
    }

    bool MemoryWriteExclusive8(A32::VAddr a, uint8_t v, uint8_t e) override { return MemoryWriteExclusiveT(a, v, e); }
    bool MemoryWriteExclusive16(A32::VAddr a, uint16_t v, uint16_t e) override { return MemoryWriteExclusiveT(a, v, e); }
    bool MemoryWriteExclusive32(A32::VAddr a, uint32_t v, uint32_t e) override { return MemoryWriteExclusiveT(a, v, e); }
    bool MemoryWriteExclusive64(A32::VAddr a, uint64_t v, uint64_t e) override { return MemoryWriteExclusiveT(a, v, e); }

    void InterpreterFallback(A32::VAddr addr, size_t) override {
        LOG_ERROR("[IR-interp] InterpreterFallback requested at pc=0x{:x}; terminal=Interpret would be unreachable with hook_hint_instructions=true",
                  addr);
    }

    void ExceptionRaised(uint32_t pc, A32::Exception ex) override;
    void CallSVC(uint32_t svc) override;

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 60; }
};

// --------------------------------------------------------------------------
// Local IR evaluator: types + helpers
// --------------------------------------------------------------------------

// Storage slot for a single SSA value produced by one IR::Inst.  Lives in the
// global namespace (not anonymous) so that IRInterpreterCPU can hold a
// reusable `std::vector<IRValue>` member referenced from the public header.
//
// Note: pseudo-ops like GetCarryFromOp/GetNZCVFromOp read side-channel state
// from the *producing* inst (e.g. Add32). We therefore always populate
// nzcv/carry/overflow on every ALU inst regardless of the IR return type.
struct IRValue {
    uint64_t lo = 0;   // U1/U8/U16/U32/U64 use this
    uint64_t hi = 0;   // U128 uses hi:lo
    uint32_t nzcv = 0; // packed N/Z/C/V in CPSR bit positions (31..28)
    bool carry_out = false;
    bool overflow_out = false;
    uint32_t ge = 0;    // GE flags (bits 19..16 of CPSR)
    bool produced = false; // set once evaluated (diagnostic aid only)
};

// ----- Cached block wrapper with pre-analyzed terminal -----------------
//
// Lives in the global namespace too so the header's forward declaration
// resolves.  A CachedBlock is owned by block_cache; pointers into it
// (link_cached) are valid until invalidate_jit_cache() clears the map.

struct CachedBlock {
    std::unique_ptr<Dynarmic::IR::Block> ir;

    // Cached block-entry condition info to skip the evaluate_cond call in
    // the common AL case (>95% of blocks on Vita workloads).
    bool cond_al = true;
    Dynarmic::IR::Cond cond = Dynarmic::IR::Cond::AL;
    bool cond_fail_valid = false;
    uint32_t cond_fail_pc = 0;
    bool cond_fail_thumb = false;

    // Pre-analyzed terminal fast path.  Populated at translate time by
    // analyze_terminal().  FP_COMPLEX falls back to boost::apply_visitor.
    //
    // FP_COND_LINK covers the hot If(cond, {Check}Link(A), {Check}Link(B))
    // pattern emitted by the A32 frontend for conditional branches.  Both
    // targets are resolved the same way as FP_LINK (below), but each leaf
    // carries its own halt-check flag because the two arms of the If may
    // have been emitted with / without CheckHalt independently.
    enum FastPath : uint8_t {
        FP_COMPLEX = 0,
        FP_RETURN_DISPATCH,
        FP_LINK,
        FP_LINK_HALT,   // CheckHalt(else: Link{Block,BlockFast}) -- fastest path
        FP_COND_LINK,   // If(cond, [CheckHalt]Link(A), [CheckHalt]Link(B))
    };
    FastPath fast_path = FP_COMPLEX;
    bool term_halt_check = false;  // true when terminal tree begins with CheckHalt

    // Resolved "then" link target.  For FP_LINK / FP_LINK_HALT this is the
    // only link slot.  For FP_COND_LINK it is the branch taken when the
    // pre-cached condition evaluates true.
    uint32_t link_pc = 0;
    bool link_thumb = false;
    uint64_t link_key = 0;
    CachedBlock *link_cached = nullptr;  // resolved on first traversal

    // Resolved "else" link target for FP_COND_LINK only.
    uint32_t link_pc_alt = 0;
    bool link_thumb_alt = false;
    uint64_t link_key_alt = 0;
    CachedBlock *link_cached_alt = nullptr;

    // Per-leaf halt check flags for FP_COND_LINK.  The "then" leaf uses
    // term_halt_check (shared with single-leaf cases); the "else" leaf
    // tracks halt independently.
    bool term_halt_check_alt = false;

    // Pre-cached condition for FP_COND_LINK.  If term_cond_al is true, the
    // "then" branch is always taken at runtime (degenerate If).  Otherwise
    // evaluate_cond(term_cond, cpsr) at dispatch time.
    Dynarmic::IR::Cond term_cond = Dynarmic::IR::Cond::AL;
    bool term_cond_al = true;

    // Full terminal tree kept for the FP_COMPLEX fallback.
    Dynarmic::IR::Term::Terminal terminal;
};

namespace {

static uint32_t flags_nzcv(uint32_t result_value, bool c, bool v) {
    const uint32_t n = (result_value & 0x80000000u) ? 0x80000000u : 0;
    const uint32_t z = (result_value == 0) ? 0x40000000u : 0;
    const uint32_t cbit = c ? 0x20000000u : 0;
    const uint32_t vbit = v ? 0x10000000u : 0;
    return n | z | cbit | vbit;
}

static uint32_t flags_nz_only(uint32_t result_value) {
    const uint32_t n = (result_value & 0x80000000u) ? 0x80000000u : 0;
    const uint32_t z = (result_value == 0) ? 0x40000000u : 0;
    return n | z;
}

struct EvalCtx {
    std::vector<IRValue> values;
    IRInterpreterCPU *cpu;

    // Accessors for the enclosing CPU state. Definitions follow further down
    // once the full class is visible via the friend relationship.
    uint32_t &reg(uint8_t i);
    uint32_t &ext(uint8_t i);
    uint32_t &cpsr();
    uint32_t &fpscr();
};

static IR::Value unwrap_identity(IR::Value v) {
    // After IdentityRemovalPass most Identity insts are gone, but defensively
    // chase through any remaining ones.
    while (v.IsIdentity()) {
        IR::Inst *p = v.GetInst();
        if (!p) break;
        v = p->GetArg(0);
    }
    return v;
}

static uint64_t read_any(const EvalCtx &ctx, const IR::Value &vin) {
    IR::Value v = unwrap_identity(vin);
    if (v.IsImmediate()) {
        return v.GetImmediateAsU64();
    }
    IR::Inst *p = v.GetInst();
    const unsigned name = p->GetName();
    if (name >= ctx.values.size()) {
        LOG_ERROR("[IR-interp] SSA name {} out of range (size={}) for opcode {}",
                  name, ctx.values.size(), IR::GetNameOf(p->GetOpcode()));
        return 0;
    }
    return ctx.values[name].lo;
}

static bool read_u1(const EvalCtx &ctx, const IR::Value &v) {
    return read_any(ctx, v) != 0;
}
static uint8_t read_u8(const EvalCtx &ctx, const IR::Value &v) {
    return static_cast<uint8_t>(read_any(ctx, v));
}
static uint16_t read_u16(const EvalCtx &ctx, const IR::Value &v) {
    return static_cast<uint16_t>(read_any(ctx, v));
}
static uint32_t read_u32(const EvalCtx &ctx, const IR::Value &v) {
    return static_cast<uint32_t>(read_any(ctx, v));
}
static uint64_t read_u64(const EvalCtx &ctx, const IR::Value &v) {
    return read_any(ctx, v);
}

// --------------------------------------------------------------------------
// M12.5: FP + U128 helpers.
// --------------------------------------------------------------------------

// Read a U128 SSA value as a {lo, hi} pair. U128 has no immediate form in the
// IR so we only have to walk SSA references here.
static std::pair<uint64_t, uint64_t> read_u128(const EvalCtx &ctx, const IR::Value &vin) {
    IR::Value v = unwrap_identity(vin);
    if (v.IsImmediate()) return { 0, 0 };
    IR::Inst *p = v.GetInst();
    if (!p) return { 0, 0 };
    const unsigned name = p->GetName();
    if (name >= ctx.values.size()) return { 0, 0 };
    const IRValue &src = ctx.values[name];
    return { src.lo, src.hi };
}

static inline float as_f32(uint32_t bits) { return std::bit_cast<float>(bits); }
static inline double as_f64(uint64_t bits) { return std::bit_cast<double>(bits); }
static inline uint32_t to_u32(float v) { return std::bit_cast<uint32_t>(v); }
static inline uint64_t to_u64(double v) { return std::bit_cast<uint64_t>(v); }

// ARM FPSCR.NZCV comparison encoding (bits 31..28):
//   EQ (==):          0110 -> 0x60000000
//   LT (<):           1000 -> 0x80000000
//   GT (>):           0010 -> 0x20000000
//   Unordered (NaN):  0011 -> 0x30000000
template <typename F>
static inline uint32_t fp_compare_nzcv(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return 0x30000000u;
    if (a == b) return 0x60000000u;
    if (a < b)  return 0x80000000u;
    return 0x20000000u;
}

// Element accessors for U128 stored as { lo, hi } little-endian bytes.
// Element 0 occupies the lowest-addressed bits of `lo`.
template <typename T>
static inline T vec_get_element(uint64_t lo, uint64_t hi, uint8_t idx) {
    constexpr size_t total = 16 / sizeof(T);
    if (idx >= total) return T{};
    uint8_t bytes[16];
    std::memcpy(bytes, &lo, 8);
    std::memcpy(bytes + 8, &hi, 8);
    T out;
    std::memcpy(&out, bytes + idx * sizeof(T), sizeof(T));
    return out;
}

template <typename T>
static inline void vec_set_element(uint64_t &lo, uint64_t &hi, uint8_t idx, T val) {
    constexpr size_t total = 16 / sizeof(T);
    if (idx >= total) return;
    uint8_t bytes[16];
    std::memcpy(bytes, &lo, 8);
    std::memcpy(bytes + 8, &hi, 8);
    std::memcpy(bytes + idx * sizeof(T), &val, sizeof(T));
    std::memcpy(&lo, bytes, 8);
    std::memcpy(&hi, bytes + 8, 8);
}

template <typename T>
static inline std::pair<uint64_t, uint64_t> vec_broadcast(T val, bool lower_only = false) {
    uint8_t bytes[16] = {};
    const size_t count = (lower_only ? 8 : 16) / sizeof(T);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(bytes + i * sizeof(T), &val, sizeof(T));
    }
    uint64_t outlo, outhi;
    std::memcpy(&outlo, bytes, 8);
    std::memcpy(&outhi, bytes + 8, 8);
    return { outlo, outhi };
}

// Apply a per-lane binary op across 16 bytes of { al, ah } and { bl, bh }.
template <typename T, typename F>
static inline std::pair<uint64_t, uint64_t>
vec_binop(uint64_t al, uint64_t ah, uint64_t bl, uint64_t bh, F op) {
    constexpr size_t N = 16 / sizeof(T);
    T a_arr[N]{};
    T b_arr[N]{};
    T r_arr[N]{};
    uint64_t a_buf[2] = { al, ah };
    uint64_t b_buf[2] = { bl, bh };
    std::memcpy(a_arr, a_buf, 16);
    std::memcpy(b_arr, b_buf, 16);
    for (size_t i = 0; i < N; ++i) r_arr[i] = op(a_arr[i], b_arr[i]);
    uint64_t r_buf[2];
    std::memcpy(r_buf, r_arr, 16);
    return { r_buf[0], r_buf[1] };
}

template <typename T, typename F>
static inline std::pair<uint64_t, uint64_t>
vec_unop(uint64_t al, uint64_t ah, F op) {
    constexpr size_t N = 16 / sizeof(T);
    T a_arr[N]{};
    T r_arr[N]{};
    uint64_t a_buf[2] = { al, ah };
    std::memcpy(a_arr, a_buf, 16);
    for (size_t i = 0; i < N; ++i) r_arr[i] = op(a_arr[i]);
    uint64_t r_buf[2];
    std::memcpy(r_buf, r_arr, 16);
    return { r_buf[0], r_buf[1] };
}

// Saturating arithmetic helpers: clamp a wider integer result back into T's
// representable range.
template <typename T>
static inline T sat_clamp_s(int64_t v) {
    constexpr int64_t lo = std::numeric_limits<T>::min();
    constexpr int64_t hi = std::numeric_limits<T>::max();
    if (v < lo) return static_cast<T>(lo);
    if (v > hi) return static_cast<T>(hi);
    return static_cast<T>(v);
}
template <typename T>
static inline T sat_clamp_u(int64_t v) {
    constexpr int64_t hi = static_cast<int64_t>(std::numeric_limits<T>::max());
    if (v < 0) return T{0};
    if (v > hi) return static_cast<T>(hi);
    return static_cast<T>(v);
}

// IEEE 754 binary16 <-> binary32 conversion (round-to-nearest-even, flush-to-
// zero on underflow).  We skip the rmode arg for simplicity; the only rmode
// that matters in practice is RNE which matches native behaviour.
static inline float fp16_to_fp32(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            int e = 0;
            uint32_t m = mant;
            while ((m & 0x400u) == 0) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = (sign << 31) | (static_cast<uint32_t>(112 - e) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(bits);
}

static inline uint16_t fp32_to_fp16(float f) {
    const uint32_t bits = std::bit_cast<uint32_t>(f);
    const uint32_t sign = (bits >> 31) & 1u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = bits & 0x7FFFFFu;
    uint16_t out;
    if ((bits & 0x7F800000u) == 0x7F800000u) {
        // Inf/NaN: preserve NaN-ness in the top mantissa bit.
        out = static_cast<uint16_t>((sign << 15) | 0x7C00u | (mant != 0 ? 0x0200u : 0u));
    } else if (exp <= 0) {
        out = static_cast<uint16_t>(sign << 15);
    } else if (exp >= 31) {
        out = static_cast<uint16_t>((sign << 15) | 0x7C00u);
    } else {
        const uint32_t round = (mant + 0x1000u) >> 13;
        if (round > 0x3FFu) {
            out = static_cast<uint16_t>((sign << 15) | (static_cast<uint32_t>(exp + 1) << 10));
        } else {
            out = static_cast<uint16_t>((sign << 15) | (static_cast<uint32_t>(exp) << 10) | round);
        }
    }
    return out;
}

// Per-lane comparison that produces all-ones (true) or all-zeros (false) of
// width T, matching NEON CMEQ / FCMEQ semantics.
template <typename T, typename F>
static inline std::pair<uint64_t, uint64_t>
vec_cmp(uint64_t al, uint64_t ah, uint64_t bl, uint64_t bh, F pred) {
    constexpr size_t N = 16 / sizeof(T);
    using U = std::make_unsigned_t<T>;
    T a_arr[N]{};
    T b_arr[N]{};
    U r_arr[N]{};
    uint64_t a_buf[2] = { al, ah };
    uint64_t b_buf[2] = { bl, bh };
    std::memcpy(a_arr, a_buf, 16);
    std::memcpy(b_arr, b_buf, 16);
    for (size_t i = 0; i < N; ++i) r_arr[i] = pred(a_arr[i], b_arr[i]) ? static_cast<U>(~U{0}) : U{0};
    uint64_t r_buf[2];
    std::memcpy(r_buf, r_arr, 16);
    return { r_buf[0], r_buf[1] };
}

// Looks up the "producer" IRValue for a pseudo-op's parent (e.g. the Add32
// that feeds a GetCarryFromOp). Safe to call with an immediate; returns a
// default-constructed IRValue if the arg isn't a real inst reference.
static const IRValue &producer_of(const EvalCtx &ctx, const IR::Value &vin) {
    static const IRValue sentinel{};
    IR::Value v = unwrap_identity(vin);
    if (v.IsImmediate()) return sentinel;
    IR::Inst *p = v.GetInst();
    if (!p) return sentinel;
    const unsigned name = p->GetName();
    if (name >= ctx.values.size()) return sentinel;
    return ctx.values[name];
}

static bool evaluate_cond(IR::Cond cond, uint32_t cpsr_val) {
    const bool n = (cpsr_val & 0x80000000u) != 0;
    const bool z = (cpsr_val & 0x40000000u) != 0;
    const bool c = (cpsr_val & 0x20000000u) != 0;
    const bool v = (cpsr_val & 0x10000000u) != 0;
    switch (cond) {
    case IR::Cond::EQ: return z;
    case IR::Cond::NE: return !z;
    case IR::Cond::CS: return c;
    case IR::Cond::CC: return !c;
    case IR::Cond::MI: return n;
    case IR::Cond::PL: return !n;
    case IR::Cond::VS: return v;
    case IR::Cond::VC: return !v;
    case IR::Cond::HI: return c && !z;
    case IR::Cond::LS: return !c || z;
    case IR::Cond::GE: return n == v;
    case IR::Cond::LT: return n != v;
    case IR::Cond::GT: return !z && (n == v);
    case IR::Cond::LE: return z || (n != v);
    case IR::Cond::AL: return true;
    case IR::Cond::NV: return false;
    }
    return true;
}

}  // namespace

// --------------------------------------------------------------------------
// IRInterpreterCPU: construction / destruction / ArmInterpCallback friend access
// --------------------------------------------------------------------------

namespace {
// Friend-access shim: EvalCtx accessors reach through cpu -> CPU fields. We
// declare these here in one place and make them forwarders into the class.
uint32_t &EvalCtx_reg(IRInterpreterCPU *cpu, uint8_t i);
uint32_t &EvalCtx_ext(IRInterpreterCPU *cpu, uint8_t i);
uint32_t &EvalCtx_cpsr(IRInterpreterCPU *cpu);
uint32_t &EvalCtx_fpscr(IRInterpreterCPU *cpu);
}  // namespace

uint32_t &EvalCtx::reg(uint8_t i) { return EvalCtx_reg(cpu, i); }
uint32_t &EvalCtx::ext(uint8_t i) { return EvalCtx_ext(cpu, i); }
uint32_t &EvalCtx::cpsr() { return EvalCtx_cpsr(cpu); }
uint32_t &EvalCtx::fpscr() { return EvalCtx_fpscr(cpu); }

IRInterpreterCPU::IRInterpreterCPU(CPUState *state, std::size_t processor_id,
                                   Dynarmic::ExclusiveMonitor *monitor_)
    : parent(state)
    , cb(std::make_unique<ArmInterpCallback>(state, this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , monitor(monitor_)
    , core_id(processor_id) {
}

IRInterpreterCPU::~IRInterpreterCPU() = default;

namespace {
uint32_t &EvalCtx_reg(IRInterpreterCPU *cpu, uint8_t i) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::regs;
    };
    return static_cast<Friend *>(cpu)->regs[i];
}
uint32_t &EvalCtx_ext(IRInterpreterCPU *cpu, uint8_t i) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::ext_regs;
    };
    return static_cast<Friend *>(cpu)->ext_regs[i];
}
uint32_t &EvalCtx_cpsr(IRInterpreterCPU *cpu) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::cpsr;
    };
    return static_cast<Friend *>(cpu)->cpsr;
}
uint32_t &EvalCtx_fpscr(IRInterpreterCPU *cpu) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::fpscr;
    };
    return static_cast<Friend *>(cpu)->fpscr;
}
}  // namespace

// --------------------------------------------------------------------------
// Block cache + translation
//
// The block cache is structured as two tiers:
//   L0: direct-mapped array keyed by (key & (kL0Size-1)).  O(1) with no hash
//       function, one compare + branch.  Flushed on invalidate.
//   L1: std::unordered_map<uint64_t, unique_ptr<CachedBlock>>.  Authoritative.
//
// CachedBlock wraps the translated IR::Block with pre-analyzed terminal
// metadata so the dispatch loop can skip the boost::variant visitor in the
// common "CheckHalt -> LinkBlock(next)" case.
// --------------------------------------------------------------------------

// Extract a link target (pc + thumb + key + per-leaf halt check) from a
// terminal that is expected to be either Link{Block,BlockFast} or
// CheckHalt(else: Link{Block,BlockFast}).  Returns false if the shape
// doesn't match, leaving the outputs untouched.  Used by analyze_terminal
// below when recognising the FP_COND_LINK pattern: both arms of an If
// terminal may independently carry (or omit) a CheckHalt wrapper.
static bool extract_link_leaf(const IR::Term::Terminal &t,
                              uint32_t &out_pc, bool &out_thumb,
                              uint64_t &out_key, bool &out_halt_check) {
    const IR::Term::Terminal *inner = &t;
    bool halt = false;
    if (const auto *ch = boost::get<IR::Term::CheckHalt>(inner)) {
        halt = true;
        inner = &ch->else_;
    }
    if (const auto *link = boost::get<IR::Term::LinkBlock>(inner)) {
        A32::LocationDescriptor d(link->next);
        out_pc = d.PC();
        out_thumb = d.TFlag();
        out_key = d.UniqueHash();
        out_halt_check = halt;
        return true;
    }
    if (const auto *linkf = boost::get<IR::Term::LinkBlockFast>(inner)) {
        A32::LocationDescriptor d(linkf->next);
        out_pc = d.PC();
        out_thumb = d.TFlag();
        out_key = d.UniqueHash();
        out_halt_check = halt;
        return true;
    }
    return false;
}

// Recursive terminal analyzer: detects the common nested patterns emitted by
// the A32 frontend and populates the CachedBlock fast-path fields.  Anything
// we don't recognise leaves fast_path == FP_COMPLEX (visitor fallback).
static void analyze_terminal(CachedBlock &cb, const IR::Term::Terminal &t, bool halt_check) {
    if (const auto *ch = boost::get<IR::Term::CheckHalt>(&t)) {
        analyze_terminal(cb, ch->else_, /*halt_check=*/true);
        return;
    }
    auto set_link = [&](const A32::LocationDescriptor &next) {
        cb.fast_path = halt_check ? CachedBlock::FP_LINK_HALT : CachedBlock::FP_LINK;
        cb.link_pc = next.PC();
        cb.link_thumb = next.TFlag();
        cb.link_key = next.UniqueHash();
        cb.term_halt_check = halt_check;
    };
    if (const auto *link = boost::get<IR::Term::LinkBlock>(&t)) {
        set_link(A32::LocationDescriptor(link->next));
        return;
    }
    if (const auto *linkf = boost::get<IR::Term::LinkBlockFast>(&t)) {
        set_link(A32::LocationDescriptor(linkf->next));
        return;
    }
    if (boost::get<IR::Term::ReturnToDispatch>(&t)) {
        cb.fast_path = CachedBlock::FP_RETURN_DISPATCH;
        cb.term_halt_check = halt_check;
        return;
    }
    // M12.7: If(cond, [CheckHalt]Link(A), [CheckHalt]Link(B))
    // -- generated for conditional branches that have both arms resolvable
    // at translation time.  Both leaves must be Link-shaped (optionally
    // wrapped in a single CheckHalt) to qualify; anything deeper is a
    // visitor fallback.  We carry an outer halt_check bit through so the
    // outer CheckHalt semantics aren't lost when they wrap the whole If.
    if (const auto *if_ = boost::get<IR::Term::If>(&t)) {
        uint32_t a_pc = 0, b_pc = 0;
        bool a_thumb = false, b_thumb = false;
        uint64_t a_key = 0, b_key = 0;
        bool a_halt = false, b_halt = false;
        if (extract_link_leaf(if_->then_, a_pc, a_thumb, a_key, a_halt) &&
            extract_link_leaf(if_->else_, b_pc, b_thumb, b_key, b_halt)) {
            cb.fast_path = CachedBlock::FP_COND_LINK;
            cb.term_cond = if_->if_;
            cb.term_cond_al = (if_->if_ == IR::Cond::AL);
            cb.link_pc = a_pc;
            cb.link_thumb = a_thumb;
            cb.link_key = a_key;
            cb.term_halt_check = a_halt || halt_check;
            cb.link_pc_alt = b_pc;
            cb.link_thumb_alt = b_thumb;
            cb.link_key_alt = b_key;
            cb.term_halt_check_alt = b_halt || halt_check;
            return;
        }
        // Fall through to FP_COMPLEX for non-Link arms.
    }
    // Term::CheckBit / Term::Interpret / Term::Invalid / Term::PopRSBHint /
    // Term::FastDispatchHint / complex If -> visitor fallback.
    cb.fast_path = CachedBlock::FP_COMPLEX;
    cb.term_halt_check = halt_check;
}

CachedBlock *IRInterpreterCPU::lookup_cached_block(uint64_t key) {
    const std::size_t slot = static_cast<std::size_t>(key) & (kL0Size - 1);
    auto &entry = l0_cache[slot];
    if (entry.second && entry.first == key) {
        return entry.second;
    }
    auto it = block_cache.find(key);
    if (it == block_cache.end()) return nullptr;
    entry.first = key;
    entry.second = it->second.get();
    return entry.second;
}

CachedBlock *IRInterpreterCPU::get_or_translate_block_cached(uint32_t pc, bool thumb) {
    Dynarmic::A32::PSR psr;
    psr.T(thumb);
    Dynarmic::A32::FPSCR fpscr_r(fpscr);
    A32::LocationDescriptor loc(pc, psr, fpscr_r);

    const uint64_t key = loc.UniqueHash();

    // ---- L0 direct-mapped lookup ----
    const std::size_t slot = static_cast<std::size_t>(key) & (kL0Size - 1);
    auto &entry = l0_cache[slot];
    if (entry.second && entry.first == key) {
        return entry.second;
    }

    // ---- L1 hash map lookup ----
    auto it = block_cache.find(key);
    if (it != block_cache.end()) {
        entry.first = key;
        entry.second = it->second.get();
        return entry.second;
    }

    // ---- Miss: translate + analyze ----
    A32::TranslationOptions opts;
    opts.arch_version = A32::ArchVersion::v7;
    opts.define_unpredictable_behaviour = false;
    opts.hook_hint_instructions = true;

    IR::Block translated = A32::Translate(loc, cb.get(), opts);

    // Optimization sequence. We intentionally skip A32GetSetElimination and
    // ConstantPropagation: both are correctness-sensitive when the final
    // consumer is an evaluator rather than a JIT (they change the IR shape
    // in ways the JIT compensates for internally). NamingPass runs LAST so
    // the SSA name space is dense in [1..block.size()] after DCE.
    Optimization::PolyfillPass(translated, {});
    Optimization::DeadCodeElimination(translated);
    Optimization::IdentityRemovalPass(translated);
    Optimization::NamingPass(translated);
#ifdef DYNARMIC_IR_VERIFY
    Optimization::VerificationPass(translated);
#endif

    auto new_cached = std::unique_ptr<CachedBlock>(new CachedBlock{});
    new_cached->ir.reset(new IR::Block(std::move(translated)));

    // Pre-extract block-entry condition info.
    const IR::Cond block_cond = new_cached->ir->GetCondition();
    new_cached->cond = block_cond;
    new_cached->cond_al = (block_cond == IR::Cond::AL);
    if (new_cached->ir->HasConditionFailedLocation()) {
        A32::LocationDescriptor fail(new_cached->ir->ConditionFailedLocation());
        new_cached->cond_fail_valid = true;
        new_cached->cond_fail_pc = fail.PC();
        new_cached->cond_fail_thumb = fail.TFlag();
    }

    // Pre-analyze terminal tree.
    new_cached->terminal = new_cached->ir->GetTerminal();
    analyze_terminal(*new_cached, new_cached->terminal, /*halt_check=*/false);

    CachedBlock *raw = new_cached.get();
    block_cache.emplace(key, std::move(new_cached));
    entry.first = key;
    entry.second = raw;
    return raw;
}

// --------------------------------------------------------------------------
// Callback dispatch for exception / SVC -- needs IRInterpreterCPU to be
// complete, so it goes after the ctor.
// --------------------------------------------------------------------------

void ArmInterpCallback::ExceptionRaised(uint32_t pc, A32::Exception ex) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::halted;
        using IRInterpreterCPU::break_;
        using IRInterpreterCPU::halt_requested;
    };
    auto *self = static_cast<Friend *>(cpu);
    switch (ex) {
    case A32::Exception::Breakpoint:
        self->break_ = true;
        self->halt_requested.store(true, std::memory_order_release);
        cpu->set_pc(pc);
        break;
    case A32::Exception::WaitForInterrupt:
        self->halted = true;
        self->halt_requested.store(true, std::memory_order_release);
        break;
    case A32::Exception::PreloadData:
    case A32::Exception::PreloadDataWithIntentToWrite:
    case A32::Exception::PreloadInstruction:
    case A32::Exception::SendEvent:
    case A32::Exception::SendEventLocal:
    case A32::Exception::WaitForEvent:
    case A32::Exception::Yield:
        break;
    case A32::Exception::UndefinedInstruction:
        LOG_WARN("[IR-interp] Undefined instruction at 0x{:X}", pc);
        self->halt_requested.store(true, std::memory_order_release);
        break;
    case A32::Exception::UnpredictableInstruction:
        LOG_WARN("[IR-interp] Unpredictable instruction at 0x{:X}", pc);
        self->halt_requested.store(true, std::memory_order_release);
        break;
    case A32::Exception::DecodeError:
        LOG_WARN("[IR-interp] Decode error at 0x{:X}", pc);
        self->halt_requested.store(true, std::memory_order_release);
        break;
    default:
        LOG_WARN("[IR-interp] Unknown exception {} at pc=0x{:x}", static_cast<int>(ex), pc);
        self->halt_requested.store(true, std::memory_order_release);
        break;
    }
}

void ArmInterpCallback::CallSVC(uint32_t svc) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::halt_requested;
    };
    parent->svc_called = true;
    parent->svc = svc;
    static_cast<Friend *>(cpu)->halt_requested.store(true, std::memory_order_release);
}

// --------------------------------------------------------------------------
// Big opcode dispatcher. One case per implemented opcode; the default arm
// halts the CPU with a descriptive log line so unimplemented ops surface
// loudly during development rather than silently returning zero.
// --------------------------------------------------------------------------

namespace {

// Halt helper for the unimplemented default arm. Uses friend shim via the
// same pattern as EvalCtx accessors.
static void interp_halt_unimplemented(IRInterpreterCPU *cpu, IR::Opcode op, uint32_t pc) {
    struct Friend : public IRInterpreterCPU {
        using IRInterpreterCPU::halt_requested;
        using IRInterpreterCPU::halted;
    };
    auto *self = static_cast<Friend *>(cpu);
    LOG_CRITICAL("[IR-interp] Unimplemented opcode '{}' at block pc=0x{:x} -- halting "
                 "CPU. Implement this op in ir_interpreter_cpu.cpp.",
                 IR::GetNameOf(op), pc);
    self->halt_requested.store(true, std::memory_order_release);
    self->halted = true;
}

// Evaluate one IR::Inst and write its result + side-channel flags into
// ctx.values[inst.name]. Returns false if evaluation requested a halt
// (caller should stop iterating the block).
static bool evaluate_inst(EvalCtx &ctx, IR::Inst &inst, IRInterpreterCPU *cpu,
                          uint32_t block_pc,
                          ArmInterpCallback *cb, ArmDynarmicCP15 *cp15,
                          Dynarmic::ExclusiveMonitor *monitor, std::size_t core_id,
                          uint32_t &out_check_bit_storage) {
    const unsigned name = inst.GetName();
    if (name >= ctx.values.size()) {
        ctx.values.resize(name + 1);
    }
    IRValue &dst = ctx.values[name];
    dst.produced = true;

    using Op = IR::Opcode;
    const Op op = inst.GetOpcode();

    switch (op) {
    // ---------- Metadata / void ----------
    case Op::Void:
    case Op::Identity:
        // Identity args are resolved by unwrap_identity at read sites; we
        // still run the naming pass over them. No stored value needed.
        dst.lo = read_any(ctx, inst.GetArg(0));
        break;

    case Op::Breakpoint: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::break_; using IRInterpreterCPU::halt_requested; };
        static_cast<Friend *>(cpu)->break_ = true;
        static_cast<Friend *>(cpu)->halt_requested.store(true, std::memory_order_release);
        return false;
    }

    // ---------- A32 register state ----------
    case Op::A32GetRegister: {
        const auto reg = inst.GetArg(0).GetA32RegRef();
        dst.lo = ctx.reg(static_cast<uint8_t>(reg));
        break;
    }
    case Op::A32SetRegister: {
        const auto reg = inst.GetArg(0).GetA32RegRef();
        ctx.reg(static_cast<uint8_t>(reg)) = read_u32(ctx, inst.GetArg(1));
        break;
    }
    case Op::A32GetExtendedRegister32: {
        const auto reg = inst.GetArg(0).GetA32ExtRegRef();
        dst.lo = ctx.ext(static_cast<uint8_t>(reg) - static_cast<uint8_t>(A32::ExtReg::S0));
        break;
    }
    case Op::A32SetExtendedRegister32: {
        const auto reg = inst.GetArg(0).GetA32ExtRegRef();
        ctx.ext(static_cast<uint8_t>(reg) - static_cast<uint8_t>(A32::ExtReg::S0)) = read_u32(ctx, inst.GetArg(1));
        break;
    }
    case Op::A32GetExtendedRegister64: {
        // D0..D31 alias S{2n}:S{2n+1}
        const auto reg = inst.GetArg(0).GetA32ExtRegRef();
        const uint8_t d = static_cast<uint8_t>(reg) - static_cast<uint8_t>(A32::ExtReg::D0);
        const uint8_t s_lo = d * 2;
        const uint32_t lo = ctx.ext(s_lo);
        const uint32_t hi = ctx.ext(s_lo + 1);
        dst.lo = (static_cast<uint64_t>(hi) << 32) | lo;
        break;
    }
    case Op::A32SetExtendedRegister64: {
        const auto reg = inst.GetArg(0).GetA32ExtRegRef();
        const uint8_t d = static_cast<uint8_t>(reg) - static_cast<uint8_t>(A32::ExtReg::D0);
        const uint8_t s_lo = d * 2;
        const uint64_t value = read_u64(ctx, inst.GetArg(1));
        ctx.ext(s_lo) = static_cast<uint32_t>(value);
        ctx.ext(s_lo + 1) = static_cast<uint32_t>(value >> 32);
        break;
    }
    case Op::A32GetVector: {
        // Q(n) aliases S(4n..4n+3).  The frontend can also emit GetVector on
        // a D-register operand (upper 64 bits implicitly zeroed) or, rarely,
        // on an S-register (upper 96 bits zero).  Handle all three cases.
        const auto reg = inst.GetArg(0).GetA32ExtRegRef();
        const uint8_t r = static_cast<uint8_t>(reg);
        if (r >= static_cast<uint8_t>(A32::ExtReg::Q0)) {
            const uint8_t q = r - static_cast<uint8_t>(A32::ExtReg::Q0);
            const uint32_t s0 = ctx.ext(q * 4 + 0);
            const uint32_t s1 = ctx.ext(q * 4 + 1);
            const uint32_t s2 = ctx.ext(q * 4 + 2);
            const uint32_t s3 = ctx.ext(q * 4 + 3);
            dst.lo = (static_cast<uint64_t>(s1) << 32) | s0;
            dst.hi = (static_cast<uint64_t>(s3) << 32) | s2;
        } else if (r >= static_cast<uint8_t>(A32::ExtReg::D0)) {
            const uint8_t d = r - static_cast<uint8_t>(A32::ExtReg::D0);
            const uint32_t lo = ctx.ext(d * 2);
            const uint32_t hi = ctx.ext(d * 2 + 1);
            dst.lo = (static_cast<uint64_t>(hi) << 32) | lo;
            dst.hi = 0;
        } else {
            dst.lo = ctx.ext(r);
            dst.hi = 0;
        }
        break;
    }
    case Op::A32SetVector: {
        const auto reg = inst.GetArg(0).GetA32ExtRegRef();
        const uint8_t r = static_cast<uint8_t>(reg);
        const auto [vlo, vhi] = read_u128(ctx, inst.GetArg(1));
        if (r >= static_cast<uint8_t>(A32::ExtReg::Q0)) {
            const uint8_t q = r - static_cast<uint8_t>(A32::ExtReg::Q0);
            ctx.ext(q * 4 + 0) = static_cast<uint32_t>(vlo);
            ctx.ext(q * 4 + 1) = static_cast<uint32_t>(vlo >> 32);
            ctx.ext(q * 4 + 2) = static_cast<uint32_t>(vhi);
            ctx.ext(q * 4 + 3) = static_cast<uint32_t>(vhi >> 32);
        } else if (r >= static_cast<uint8_t>(A32::ExtReg::D0)) {
            const uint8_t d = r - static_cast<uint8_t>(A32::ExtReg::D0);
            ctx.ext(d * 2 + 0) = static_cast<uint32_t>(vlo);
            ctx.ext(d * 2 + 1) = static_cast<uint32_t>(vlo >> 32);
        } else {
            ctx.ext(r) = static_cast<uint32_t>(vlo);
        }
        break;
    }

    case Op::A32GetCpsr:
        dst.lo = ctx.cpsr();
        break;
    case Op::A32SetCpsr:
        ctx.cpsr() = read_u32(ctx, inst.GetArg(0));
        break;
    case Op::A32SetCpsrNZCV: {
        const uint32_t nzcv = read_u32(ctx, inst.GetArg(0)); // packed in 31..28
        ctx.cpsr() = (ctx.cpsr() & 0x0FFFFFFFu) | (nzcv & 0xF0000000u);
        break;
    }
    case Op::A32SetCpsrNZCVRaw: {
        const uint32_t nzcv = read_u32(ctx, inst.GetArg(0));
        ctx.cpsr() = (ctx.cpsr() & 0x0FFFFFFFu) | (nzcv & 0xF0000000u);
        break;
    }
    case Op::A32SetCpsrNZCVQ: {
        const uint32_t nzcvq = read_u32(ctx, inst.GetArg(0));
        ctx.cpsr() = (ctx.cpsr() & 0x07FFFFFFu) | (nzcvq & 0xF8000000u);
        break;
    }
    case Op::A32SetCpsrNZ: {
        const uint32_t nz = read_u32(ctx, inst.GetArg(0));
        ctx.cpsr() = (ctx.cpsr() & 0x3FFFFFFFu) | (nz & 0xC0000000u);
        break;
    }
    case Op::A32SetCpsrNZC: {
        const uint32_t nz = read_u32(ctx, inst.GetArg(0));
        const bool c = read_u1(ctx, inst.GetArg(1));
        uint32_t new_cpsr = (ctx.cpsr() & 0x1FFFFFFFu) | (nz & 0xC0000000u);
        new_cpsr = (new_cpsr & ~0x20000000u) | (c ? 0x20000000u : 0);
        ctx.cpsr() = new_cpsr;
        break;
    }
    case Op::A32GetCFlag:
        dst.lo = (ctx.cpsr() >> 29) & 1u;
        break;
    case Op::A32OrQFlag: {
        const bool q = read_u1(ctx, inst.GetArg(0));
        if (q) ctx.cpsr() |= 0x08000000u; // bit 27
        break;
    }
    case Op::A32GetGEFlags:
        dst.lo = (ctx.cpsr() >> 16) & 0xFu;
        break;
    case Op::A32SetGEFlags: {
        const uint32_t ge = read_u32(ctx, inst.GetArg(0)) & 0xFu;
        ctx.cpsr() = (ctx.cpsr() & ~0x000F0000u) | (ge << 16);
        break;
    }
    case Op::A32SetGEFlagsCompressed: {
        const uint32_t packed = read_u32(ctx, inst.GetArg(0));
        // 4 bytes, each non-zero -> corresponding GE bit set
        uint32_t ge = 0;
        if (packed & 0x000000FFu) ge |= 1;
        if (packed & 0x0000FF00u) ge |= 2;
        if (packed & 0x00FF0000u) ge |= 4;
        if (packed & 0xFF000000u) ge |= 8;
        ctx.cpsr() = (ctx.cpsr() & ~0x000F0000u) | (ge << 16);
        break;
    }

    case Op::A32BXWritePC: {
        const uint32_t val = read_u32(ctx, inst.GetArg(0));
        if (val & 1u) {
            ctx.cpsr() |= 0x20u;
            ctx.reg(15) = val & ~1u;
        } else {
            ctx.cpsr() &= ~0x20u;
            ctx.reg(15) = val & ~3u;
        }
        break;
    }
    case Op::A32UpdateUpperLocationDescriptor:
        // Triggers a recompile of the upper descriptor bits (IT state etc.) --
        // for us, the next Translate() call naturally sees current cpsr, so
        // there's nothing to record.
        break;

    case Op::A32CallSupervisor: {
        const uint32_t svc = read_u32(ctx, inst.GetArg(0));
        cb->CallSVC(svc);
        return false;
    }
    case Op::A32ExceptionRaised: {
        const uint32_t pc = read_u32(ctx, inst.GetArg(0));
        const uint64_t ex = read_u64(ctx, inst.GetArg(1));
        cb->ExceptionRaised(pc, static_cast<A32::Exception>(ex));
        return false;
    }
    case Op::A32DataSynchronizationBarrier:
    case Op::A32DataMemoryBarrier:
    case Op::A32InstructionSynchronizationBarrier:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        break;

    case Op::A32GetFpscr:
        dst.lo = ctx.fpscr();
        break;
    case Op::A32SetFpscr:
        ctx.fpscr() = read_u32(ctx, inst.GetArg(0));
        break;
    case Op::A32GetFpscrNZCV:
        dst.lo = ctx.fpscr() & 0xF0000000u;
        break;
    case Op::A32SetFpscrNZCV: {
        const uint32_t nzcv = read_u32(ctx, inst.GetArg(0));
        ctx.fpscr() = (ctx.fpscr() & 0x0FFFFFFFu) | (nzcv & 0xF0000000u);
        break;
    }

    case Op::A32SetCheckBit:
        out_check_bit_storage = read_u1(ctx, inst.GetArg(0)) ? 1u : 0u;
        break;

    // ---------- Pseudo-ops (read from producer side-channel) ----------
    case Op::GetCarryFromOp:
        dst.lo = producer_of(ctx, inst.GetArg(0)).carry_out ? 1u : 0u;
        break;
    case Op::GetOverflowFromOp:
        dst.lo = producer_of(ctx, inst.GetArg(0)).overflow_out ? 1u : 0u;
        break;
    case Op::GetGEFromOp:
        dst.lo = producer_of(ctx, inst.GetArg(0)).ge;
        break;
    case Op::GetNZCVFromOp:
        dst.lo = producer_of(ctx, inst.GetArg(0)).nzcv;
        break;
    case Op::GetNZFromOp:
        dst.lo = producer_of(ctx, inst.GetArg(0)).nzcv & 0xC0000000u;
        break;
    case Op::GetCFlagFromNZCV:
        dst.lo = (read_u32(ctx, inst.GetArg(0)) >> 29) & 1u;
        break;
    case Op::NZCVFromPackedFlags:
        dst.lo = read_u32(ctx, inst.GetArg(0)) & 0xF0000000u;
        break;
    case Op::PushRSB:
        // RSB hint: no-op for interpreter (no return stack optimization).
        break;

    // ---------- Packing / selection ----------
    case Op::Pack2x32To1x64: {
        const uint32_t low = read_u32(ctx, inst.GetArg(0));
        const uint32_t high = read_u32(ctx, inst.GetArg(1));
        dst.lo = (static_cast<uint64_t>(high) << 32) | low;
        break;
    }
    case Op::Pack2x64To1x128: {
        dst.lo = read_u64(ctx, inst.GetArg(0));
        dst.hi = read_u64(ctx, inst.GetArg(1));
        break;
    }
    case Op::ZeroExtendLongToQuad: {
        dst.lo = read_u64(ctx, inst.GetArg(0));
        dst.hi = 0;
        break;
    }
    case Op::LeastSignificantWord:
        dst.lo = static_cast<uint32_t>(read_u64(ctx, inst.GetArg(0)));
        break;
    case Op::LeastSignificantHalf:
        dst.lo = read_u32(ctx, inst.GetArg(0)) & 0xFFFFu;
        break;
    case Op::LeastSignificantByte:
        dst.lo = read_u32(ctx, inst.GetArg(0)) & 0xFFu;
        break;
    case Op::MostSignificantWord:
        dst.lo = static_cast<uint32_t>(read_u64(ctx, inst.GetArg(0)) >> 32);
        break;
    case Op::MostSignificantBit:
        dst.lo = (read_u32(ctx, inst.GetArg(0)) >> 31) & 1u;
        break;
    case Op::IsZero32:
        dst.lo = (read_u32(ctx, inst.GetArg(0)) == 0) ? 1u : 0u;
        break;
    case Op::IsZero64:
        dst.lo = (read_u64(ctx, inst.GetArg(0)) == 0) ? 1u : 0u;
        break;
    case Op::TestBit: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t b = read_u8(ctx, inst.GetArg(1));
        dst.lo = ((v >> b) & 1u);
        break;
    }
    case Op::ConditionalSelect32: {
        const auto cond = inst.GetArg(0).GetCond();
        const uint32_t t = read_u32(ctx, inst.GetArg(1));
        const uint32_t f = read_u32(ctx, inst.GetArg(2));
        dst.lo = evaluate_cond(cond, ctx.cpsr()) ? t : f;
        break;
    }
    case Op::ConditionalSelect64: {
        const auto cond = inst.GetArg(0).GetCond();
        const uint64_t t = read_u64(ctx, inst.GetArg(1));
        const uint64_t f = read_u64(ctx, inst.GetArg(2));
        dst.lo = evaluate_cond(cond, ctx.cpsr()) ? t : f;
        break;
    }
    case Op::ConditionalSelectNZCV: {
        const auto cond = inst.GetArg(0).GetCond();
        const uint32_t t = read_u32(ctx, inst.GetArg(1));
        const uint32_t f = read_u32(ctx, inst.GetArg(2));
        const uint32_t chosen = evaluate_cond(cond, ctx.cpsr()) ? t : f;
        dst.lo = chosen & 0xF0000000u;
        break;
    }

    // ---------- Shifts ----------
    case Op::LogicalShiftLeft32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint8_t s = read_u8(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        uint32_t result;
        bool carry_out;
        if (s == 0) { result = v; carry_out = carry_in; }
        else if (s < 32) { result = v << s; carry_out = (v >> (32 - s)) & 1u; }
        else if (s == 32) { result = 0; carry_out = v & 1u; }
        else { result = 0; carry_out = false; }
        dst.lo = result;
        dst.carry_out = carry_out;
        dst.nzcv = flags_nz_only(result) | (carry_out ? 0x20000000u : 0);
        break;
    }
    case Op::LogicalShiftRight32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint8_t s = read_u8(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        uint32_t result;
        bool carry_out;
        if (s == 0) { result = v; carry_out = carry_in; }
        else if (s < 32) { result = v >> s; carry_out = (v >> (s - 1)) & 1u; }
        else if (s == 32) { result = 0; carry_out = (v >> 31) & 1u; }
        else { result = 0; carry_out = false; }
        dst.lo = result;
        dst.carry_out = carry_out;
        dst.nzcv = flags_nz_only(result) | (carry_out ? 0x20000000u : 0);
        break;
    }
    case Op::ArithmeticShiftRight32: {
        const int32_t v = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const uint8_t s = read_u8(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        int32_t result;
        bool carry_out;
        if (s == 0) { result = v; carry_out = carry_in; }
        else if (s < 32) { result = v >> s; carry_out = (static_cast<uint32_t>(v) >> (s - 1)) & 1u; }
        else { result = v >> 31; carry_out = (static_cast<uint32_t>(v) >> 31) & 1u; }
        dst.lo = static_cast<uint32_t>(result);
        dst.carry_out = carry_out;
        dst.nzcv = flags_nz_only(static_cast<uint32_t>(result)) | (carry_out ? 0x20000000u : 0);
        break;
    }
    case Op::RotateRight32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint8_t s_raw = read_u8(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        const uint8_t s = s_raw & 31u;
        uint32_t result;
        bool carry_out;
        if (s_raw == 0) { result = v; carry_out = carry_in; }
        else if (s == 0) { result = v; carry_out = (v >> 31) & 1u; } // s_raw was multiple of 32
        else { result = (v >> s) | (v << (32 - s)); carry_out = (v >> (s - 1)) & 1u; }
        dst.lo = result;
        dst.carry_out = carry_out;
        dst.nzcv = flags_nz_only(result) | (carry_out ? 0x20000000u : 0);
        break;
    }
    case Op::RotateRightExtended: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const bool carry_in = read_u1(ctx, inst.GetArg(1));
        const uint32_t result = (v >> 1) | (carry_in ? 0x80000000u : 0);
        dst.lo = result;
        dst.carry_out = (v & 1u) != 0;
        dst.nzcv = flags_nz_only(result) | (dst.carry_out ? 0x20000000u : 0);
        break;
    }
    case Op::LogicalShiftLeftMasked32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint32_t s = read_u32(ctx, inst.GetArg(1)) & 31u;
        dst.lo = v << s;
        break;
    }
    case Op::LogicalShiftRightMasked32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint32_t s = read_u32(ctx, inst.GetArg(1)) & 31u;
        dst.lo = v >> s;
        break;
    }
    case Op::ArithmeticShiftRightMasked32: {
        const int32_t v = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const uint32_t s = read_u32(ctx, inst.GetArg(1)) & 31u;
        dst.lo = static_cast<uint32_t>(v >> s);
        break;
    }
    case Op::RotateRightMasked32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint32_t s = read_u32(ctx, inst.GetArg(1)) & 31u;
        dst.lo = (s == 0) ? v : ((v >> s) | (v << (32 - s)));
        break;
    }

    // ---------- 64-bit scalar shifts (no flag outputs; dynarmic IR spec) ----
    // Signatures (see external/dynarmic/src/dynarmic/ir/opcodes.inc:112-116):
    //   LogicalShiftLeft64   U64 <- (U64 value, U8 shift)
    //   LogicalShiftRight64  U64 <- (U64 value, U8 shift)
    //   ArithmeticShiftRight64 U64 <- (U64 value, U8 shift)
    // Per Dynarmic A32 frontend, these are emitted from long-multiply /
    // 64-bit helper paths in sce libc, libm and some vfp lowering.
    // Shift amount >= 64 is well-defined for LogicalShift* = 0, and for
    // ArithmeticShiftRight = sign-extended bit.
    case Op::LogicalShiftLeft64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t s = read_u8(ctx, inst.GetArg(1));
        dst.lo = (s >= 64) ? 0ull : (v << s);
        break;
    }
    case Op::LogicalShiftRight64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t s = read_u8(ctx, inst.GetArg(1));
        dst.lo = (s >= 64) ? 0ull : (v >> s);
        break;
    }
    case Op::ArithmeticShiftRight64: {
        const int64_t v = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const uint8_t s = read_u8(ctx, inst.GetArg(1));
        const int64_t r = (s >= 64) ? (v >> 63) : (v >> s);
        dst.lo = static_cast<uint64_t>(r);
        break;
    }
    case Op::RotateRight64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t s = read_u8(ctx, inst.GetArg(1)) & 63u;
        dst.lo = (s == 0) ? v : ((v >> s) | (v << (64 - s)));
        break;
    }
    case Op::LogicalShiftLeftMasked64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint64_t s = read_u64(ctx, inst.GetArg(1)) & 63ull;
        dst.lo = v << s;
        break;
    }
    case Op::LogicalShiftRightMasked64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint64_t s = read_u64(ctx, inst.GetArg(1)) & 63ull;
        dst.lo = v >> s;
        break;
    }
    case Op::ArithmeticShiftRightMasked64: {
        const int64_t v = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const uint64_t s = read_u64(ctx, inst.GetArg(1)) & 63ull;
        dst.lo = static_cast<uint64_t>(v >> s);
        break;
    }
    case Op::RotateRightMasked64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint64_t s = read_u64(ctx, inst.GetArg(1)) & 63ull;
        dst.lo = (s == 0) ? v : ((v >> s) | (v << (64 - s)));
        break;
    }

    // ---------- Arithmetic ----------
    case Op::Add32: {
        const uint32_t a = read_u32(ctx, inst.GetArg(0));
        const uint32_t b = read_u32(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        const uint64_t full = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + (carry_in ? 1ull : 0ull);
        const uint32_t r = static_cast<uint32_t>(full);
        dst.lo = r;
        dst.carry_out = (full >> 32) != 0;
        dst.overflow_out = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
        dst.nzcv = flags_nzcv(r, dst.carry_out, dst.overflow_out);
        // Packed GE-bits for use with PackedAdd*; not used by Add32 itself but
        // harmless to leave at 0.
        break;
    }
    case Op::Sub32: {
        const uint32_t a = read_u32(ctx, inst.GetArg(0));
        const uint32_t b = read_u32(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        const uint64_t full = static_cast<uint64_t>(a) + static_cast<uint64_t>(static_cast<uint32_t>(~b)) + (carry_in ? 1ull : 0ull);
        const uint32_t r = static_cast<uint32_t>(full);
        dst.lo = r;
        dst.carry_out = (full >> 32) != 0;
        dst.overflow_out = (((a ^ b) & (a ^ r)) >> 31) & 1u;
        dst.nzcv = flags_nzcv(r, dst.carry_out, dst.overflow_out);
        break;
    }
    case Op::Mul32: {
        const uint32_t a = read_u32(ctx, inst.GetArg(0));
        const uint32_t b = read_u32(ctx, inst.GetArg(1));
        const uint32_t r = a * b;
        dst.lo = r;
        dst.nzcv = flags_nz_only(r);
        break;
    }
    case Op::Mul64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        dst.lo = a * b;
        break;
    }
    case Op::Add64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        dst.lo = a + b + (carry_in ? 1ull : 0ull);
        break;
    }
    case Op::Sub64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        const bool carry_in = read_u1(ctx, inst.GetArg(2));
        // Dynarmic uses "NOT borrow" convention: carry_in==1 means no-borrow.
        dst.lo = a - b - (carry_in ? 0ull : 1ull);
        break;
    }
    case Op::SignedMultiplyHigh64: {
        const int64_t a = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const int64_t b = static_cast<int64_t>(read_u64(ctx, inst.GetArg(1)));
        const __int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);
        dst.lo = static_cast<uint64_t>(static_cast<unsigned __int128>(p) >> 64);
        break;
    }
    case Op::UnsignedMultiplyHigh64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        const unsigned __int128 p = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
        dst.lo = static_cast<uint64_t>(p >> 64);
        break;
    }
    case Op::UnsignedDiv64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        dst.lo = (b == 0) ? 0ull : (a / b);
        break;
    }
    case Op::SignedDiv64: {
        const int64_t a = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const int64_t b = static_cast<int64_t>(read_u64(ctx, inst.GetArg(1)));
        int64_t r = 0;
        if (b != 0 && !(a == INT64_MIN && b == -1)) r = a / b;
        dst.lo = static_cast<uint64_t>(r);
        break;
    }
    case Op::MaxSigned64: {
        const int64_t a = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const int64_t b = static_cast<int64_t>(read_u64(ctx, inst.GetArg(1)));
        dst.lo = static_cast<uint64_t>(a > b ? a : b);
        break;
    }
    case Op::MinSigned64: {
        const int64_t a = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const int64_t b = static_cast<int64_t>(read_u64(ctx, inst.GetArg(1)));
        dst.lo = static_cast<uint64_t>(a < b ? a : b);
        break;
    }
    case Op::MaxUnsigned64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        dst.lo = a > b ? a : b;
        break;
    }
    case Op::MinUnsigned64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        dst.lo = a < b ? a : b;
        break;
    }
    case Op::UnsignedDiv32: {
        const uint32_t a = read_u32(ctx, inst.GetArg(0));
        const uint32_t b = read_u32(ctx, inst.GetArg(1));
        dst.lo = (b == 0) ? 0u : (a / b);
        break;
    }
    case Op::SignedDiv32: {
        const int32_t a = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const int32_t b = static_cast<int32_t>(read_u32(ctx, inst.GetArg(1)));
        int32_t r = 0;
        if (b != 0 && !(a == INT32_MIN && b == -1)) r = a / b;
        dst.lo = static_cast<uint32_t>(r);
        break;
    }

    case Op::And32: {
        const uint32_t r = read_u32(ctx, inst.GetArg(0)) & read_u32(ctx, inst.GetArg(1));
        dst.lo = r;
        dst.nzcv = flags_nz_only(r);
        break;
    }
    case Op::AndNot32: {
        const uint32_t r = read_u32(ctx, inst.GetArg(0)) & ~read_u32(ctx, inst.GetArg(1));
        dst.lo = r;
        dst.nzcv = flags_nz_only(r);
        break;
    }
    case Op::Eor32: {
        const uint32_t r = read_u32(ctx, inst.GetArg(0)) ^ read_u32(ctx, inst.GetArg(1));
        dst.lo = r;
        dst.nzcv = flags_nz_only(r);
        break;
    }
    case Op::Or32: {
        const uint32_t r = read_u32(ctx, inst.GetArg(0)) | read_u32(ctx, inst.GetArg(1));
        dst.lo = r;
        dst.nzcv = flags_nz_only(r);
        break;
    }
    case Op::Not32: {
        const uint32_t r = ~read_u32(ctx, inst.GetArg(0));
        dst.lo = r;
        dst.nzcv = flags_nz_only(r);
        break;
    }
    case Op::And64:
        dst.lo = read_u64(ctx, inst.GetArg(0)) & read_u64(ctx, inst.GetArg(1));
        break;
    case Op::Or64:
        dst.lo = read_u64(ctx, inst.GetArg(0)) | read_u64(ctx, inst.GetArg(1));
        break;
    case Op::Eor64:
        dst.lo = read_u64(ctx, inst.GetArg(0)) ^ read_u64(ctx, inst.GetArg(1));
        break;
    case Op::Not64:
        dst.lo = ~read_u64(ctx, inst.GetArg(0));
        break;
    case Op::AndNot64:
        dst.lo = read_u64(ctx, inst.GetArg(0)) & ~read_u64(ctx, inst.GetArg(1));
        break;

    // ---------- Extend / reverse / CLZ / extract ----------
    case Op::SignExtendByteToWord:
        dst.lo = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(read_u8(ctx, inst.GetArg(0)))));
        break;
    case Op::SignExtendHalfToWord:
        dst.lo = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(read_u16(ctx, inst.GetArg(0)))));
        break;
    case Op::SignExtendByteToLong:
        dst.lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(read_u8(ctx, inst.GetArg(0)))));
        break;
    case Op::SignExtendHalfToLong:
        dst.lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(read_u16(ctx, inst.GetArg(0)))));
        break;
    case Op::SignExtendWordToLong:
        dst.lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)))));
        break;
    case Op::ZeroExtendByteToWord:
        dst.lo = static_cast<uint32_t>(read_u8(ctx, inst.GetArg(0)));
        break;
    case Op::ZeroExtendHalfToWord:
        dst.lo = static_cast<uint32_t>(read_u16(ctx, inst.GetArg(0)));
        break;
    case Op::ZeroExtendByteToLong:
        dst.lo = static_cast<uint64_t>(read_u8(ctx, inst.GetArg(0)));
        break;
    case Op::ZeroExtendHalfToLong:
        dst.lo = static_cast<uint64_t>(read_u16(ctx, inst.GetArg(0)));
        break;
    case Op::ZeroExtendWordToLong:
        dst.lo = static_cast<uint64_t>(read_u32(ctx, inst.GetArg(0)));
        break;
    case Op::ByteReverseWord: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        dst.lo = __builtin_bswap32(v);
        break;
    }
    case Op::ByteReverseHalf: {
        const uint16_t v = read_u16(ctx, inst.GetArg(0));
        dst.lo = static_cast<uint16_t>(__builtin_bswap16(v));
        break;
    }
    case Op::ByteReverseDual: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        dst.lo = __builtin_bswap64(v);
        break;
    }
    case Op::CountLeadingZeros32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        dst.lo = (v == 0) ? 32u : static_cast<uint32_t>(__builtin_clz(v));
        break;
    }
    case Op::CountLeadingZeros64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        dst.lo = (v == 0) ? 64ull : static_cast<uint64_t>(__builtin_clzll(v));
        break;
    }
    case Op::ExtractRegister32: {
        const uint32_t hi = read_u32(ctx, inst.GetArg(0));
        const uint32_t lo = read_u32(ctx, inst.GetArg(1));
        const uint8_t lsb = read_u8(ctx, inst.GetArg(2)) & 31u;
        if (lsb == 0) dst.lo = lo;
        else dst.lo = (lo >> lsb) | (hi << (32 - lsb));
        break;
    }
    case Op::ExtractRegister64: {
        const uint64_t hi = read_u64(ctx, inst.GetArg(0));
        const uint64_t lo = read_u64(ctx, inst.GetArg(1));
        const uint8_t lsb = read_u8(ctx, inst.GetArg(2)) & 63u;
        if (lsb == 0) dst.lo = lo;
        else dst.lo = (lo >> lsb) | (hi << (64 - lsb));
        break;
    }
    case Op::ReplicateBit32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint8_t bit = read_u8(ctx, inst.GetArg(1)) & 31u;
        const bool set = ((v >> bit) & 1u) != 0;
        dst.lo = set ? 0xFFFFFFFFu : 0u;
        break;
    }
    case Op::ReplicateBit64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t bit = read_u8(ctx, inst.GetArg(1)) & 63u;
        const bool set = ((v >> bit) & 1ull) != 0;
        dst.lo = set ? 0xFFFFFFFFFFFFFFFFull : 0ull;
        break;
    }
    case Op::MaxSigned32:
        dst.lo = static_cast<uint32_t>(std::max<int32_t>(
            static_cast<int32_t>(read_u32(ctx, inst.GetArg(0))),
            static_cast<int32_t>(read_u32(ctx, inst.GetArg(1)))));
        break;
    case Op::MaxUnsigned32:
        dst.lo = std::max<uint32_t>(read_u32(ctx, inst.GetArg(0)), read_u32(ctx, inst.GetArg(1)));
        break;
    case Op::MinSigned32:
        dst.lo = static_cast<uint32_t>(std::min<int32_t>(
            static_cast<int32_t>(read_u32(ctx, inst.GetArg(0))),
            static_cast<int32_t>(read_u32(ctx, inst.GetArg(1)))));
        break;
    case Op::MinUnsigned32:
        dst.lo = std::min<uint32_t>(read_u32(ctx, inst.GetArg(0)), read_u32(ctx, inst.GetArg(1)));
        break;

    // ---------- Memory access ----------
    case Op::A32ClearExclusive: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        static_cast<Friend *>(cpu)->exclusive_state = 0;
        if (monitor) monitor->ClearProcessor(core_id);
        break;
    }
    case Op::A32ReadMemory8:
        dst.lo = cb->MemoryRead8(read_u32(ctx, inst.GetArg(1)));
        break;
    case Op::A32ReadMemory16:
        dst.lo = cb->MemoryRead16(read_u32(ctx, inst.GetArg(1)));
        break;
    case Op::A32ReadMemory32:
        dst.lo = cb->MemoryRead32(read_u32(ctx, inst.GetArg(1)));
        break;
    case Op::A32ReadMemory64:
        dst.lo = cb->MemoryRead64(read_u32(ctx, inst.GetArg(1)));
        break;
    case Op::A32ExclusiveReadMemory8: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        static_cast<Friend *>(cpu)->exclusive_state = addr;
        dst.lo = cb->MemoryRead8(addr);
        break;
    }
    case Op::A32ExclusiveReadMemory16: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        static_cast<Friend *>(cpu)->exclusive_state = addr;
        dst.lo = cb->MemoryRead16(addr);
        break;
    }
    case Op::A32ExclusiveReadMemory32: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        static_cast<Friend *>(cpu)->exclusive_state = addr;
        dst.lo = cb->MemoryRead32(addr);
        break;
    }
    case Op::A32ExclusiveReadMemory64: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        static_cast<Friend *>(cpu)->exclusive_state = addr;
        dst.lo = cb->MemoryRead64(addr);
        break;
    }
    case Op::A32WriteMemory8:
        cb->MemoryWrite8(read_u32(ctx, inst.GetArg(1)), read_u8(ctx, inst.GetArg(2)));
        break;
    case Op::A32WriteMemory16:
        cb->MemoryWrite16(read_u32(ctx, inst.GetArg(1)), read_u16(ctx, inst.GetArg(2)));
        break;
    case Op::A32WriteMemory32:
        cb->MemoryWrite32(read_u32(ctx, inst.GetArg(1)), read_u32(ctx, inst.GetArg(2)));
        break;
    case Op::A32WriteMemory64:
        cb->MemoryWrite64(read_u32(ctx, inst.GetArg(1)), read_u64(ctx, inst.GetArg(2)));
        break;
    case Op::A32ExclusiveWriteMemory8: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        const uint8_t val = read_u8(ctx, inst.GetArg(2));
        const uint32_t tag = static_cast<Friend *>(cpu)->exclusive_state;
        if (tag != addr) { dst.lo = 1; break; }
        // Use current memory value as "expected" -- simplification: if another
        // thread wrote in between, the CAS fails and we report exclusive
        // failure. Acceptable for single-core-like IR-interp first cut.
        const uint8_t expected = cb->MemoryRead8(addr);
        dst.lo = cb->MemoryWriteExclusive8(addr, val, expected) ? 0u : 1u;
        static_cast<Friend *>(cpu)->exclusive_state = 0;
        break;
    }
    case Op::A32ExclusiveWriteMemory16: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        const uint16_t val = read_u16(ctx, inst.GetArg(2));
        const uint32_t tag = static_cast<Friend *>(cpu)->exclusive_state;
        if (tag != addr) { dst.lo = 1; break; }
        const uint16_t expected = cb->MemoryRead16(addr);
        dst.lo = cb->MemoryWriteExclusive16(addr, val, expected) ? 0u : 1u;
        static_cast<Friend *>(cpu)->exclusive_state = 0;
        break;
    }
    case Op::A32ExclusiveWriteMemory32: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        const uint32_t val = read_u32(ctx, inst.GetArg(2));
        const uint32_t tag = static_cast<Friend *>(cpu)->exclusive_state;
        if (tag != addr) { dst.lo = 1; break; }
        const uint32_t expected = cb->MemoryRead32(addr);
        dst.lo = cb->MemoryWriteExclusive32(addr, val, expected) ? 0u : 1u;
        static_cast<Friend *>(cpu)->exclusive_state = 0;
        break;
    }
    case Op::A32ExclusiveWriteMemory64: {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::exclusive_state; };
        const uint32_t addr = read_u32(ctx, inst.GetArg(1));
        const uint64_t val = read_u64(ctx, inst.GetArg(2));
        const uint32_t tag = static_cast<Friend *>(cpu)->exclusive_state;
        if (tag != addr) { dst.lo = 1; break; }
        const uint64_t expected = cb->MemoryRead64(addr);
        dst.lo = cb->MemoryWriteExclusive64(addr, val, expected) ? 0u : 1u;
        static_cast<Friend *>(cpu)->exclusive_state = 0;
        break;
    }

    // ---------- Coprocessor (cp15 only: TPIDRURO read) ----------
    case Op::A32CoprocGetOneWord: {
        const auto info = inst.GetArg(0).GetCoprocInfo();
        const uint8_t coproc_no = info[0];
        if (coproc_no != 15) { dst.lo = 0; break; }
        const bool two = info[1] != 0;
        const unsigned opc1 = info[2];
        const auto CRn = static_cast<A32::CoprocReg>(info[3]);
        const auto CRm = static_cast<A32::CoprocReg>(info[4]);
        const unsigned opc2 = info[5];
        auto result = cp15->CompileGetOneWord(two, opc1, CRn, CRm, opc2);
        if (auto pptr = std::get_if<uint32_t *>(&result)) {
            dst.lo = **pptr;
        } else {
            dst.lo = 0;
        }
        break;
    }
    case Op::A32CoprocSendOneWord:
    case Op::A32CoprocSendTwoWords:
    case Op::A32CoprocGetTwoWords:
    case Op::A32CoprocInternalOperation:
    case Op::A32CoprocLoadWords:
    case Op::A32CoprocStoreWords:
        // Vita3K only really needs TPIDRURO (handled above). Everything else
        // becomes a no-op with default-zero return.
        dst.lo = 0;
        break;

    // ======================================================================
    // M12.5 FP/VFP scalar ops
    //
    // Semantics: native host float/double arithmetic; IEEE 754 default
    // rounding (round-to-nearest) and NaN propagation matches FPSCR default.
    // The FZ / RM bits are intentionally NOT honoured in this first cut --
    // the vast majority of Vita guest code runs with defaults.  Refine here
    // if a title trips on sub-normal flushing.
    // ======================================================================
    case Op::FPAbs32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        dst.lo = v & 0x7FFFFFFFu;
        break;
    }
    case Op::FPAbs64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        dst.lo = v & 0x7FFFFFFFFFFFFFFFull;
        break;
    }
    case Op::FPNeg32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        dst.lo = v ^ 0x80000000u;
        break;
    }
    case Op::FPNeg64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        dst.lo = v ^ 0x8000000000000000ull;
        break;
    }
    case Op::FPAdd32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(a + b);
        break;
    }
    case Op::FPAdd64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(a + b);
        break;
    }
    case Op::FPSub32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(a - b);
        break;
    }
    case Op::FPSub64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(a - b);
        break;
    }
    case Op::FPMul32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(a * b);
        break;
    }
    case Op::FPMul64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(a * b);
        break;
    }
    case Op::FPDiv32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(a / b);
        break;
    }
    case Op::FPDiv64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(a / b);
        break;
    }
    case Op::FPSqrt32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        dst.lo = to_u32(std::sqrt(a));
        break;
    }
    case Op::FPSqrt64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        dst.lo = to_u64(std::sqrt(a));
        break;
    }
    case Op::FPMulAdd32: {
        // addend + op1 * op2
        const float add = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float a = as_f32(read_u32(ctx, inst.GetArg(1)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(2)));
        dst.lo = to_u32(std::fma(a, b, add));
        break;
    }
    case Op::FPMulAdd64: {
        const double add = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double a = as_f64(read_u64(ctx, inst.GetArg(1)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(2)));
        dst.lo = to_u64(std::fma(a, b, add));
        break;
    }
    case Op::FPMulSub32: {
        const float sub = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float a = as_f32(read_u32(ctx, inst.GetArg(1)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(2)));
        dst.lo = to_u32(std::fma(-a, b, sub));
        break;
    }
    case Op::FPMulSub64: {
        const double sub = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double a = as_f64(read_u64(ctx, inst.GetArg(1)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(2)));
        dst.lo = to_u64(std::fma(-a, b, sub));
        break;
    }
    case Op::FPMax32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(std::isnan(a) || std::isnan(b) ? std::numeric_limits<float>::quiet_NaN() : std::fmax(a, b));
        break;
    }
    case Op::FPMax64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::fmax(a, b));
        break;
    }
    case Op::FPMaxNumeric32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(std::fmax(a, b));
        break;
    }
    case Op::FPMaxNumeric64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(std::fmax(a, b));
        break;
    }
    case Op::FPMin32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(std::isnan(a) || std::isnan(b) ? std::numeric_limits<float>::quiet_NaN() : std::fmin(a, b));
        break;
    }
    case Op::FPMin64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::fmin(a, b));
        break;
    }
    case Op::FPMinNumeric32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = to_u32(std::fmin(a, b));
        break;
    }
    case Op::FPMinNumeric64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = to_u64(std::fmin(a, b));
        break;
    }
    case Op::FPMulX32: {
        // FPMulX: treat (+-0)*(+-inf) = +-2.0 as per ARM FPMulX definition.
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        float r = a * b;
        if ((std::fpclassify(a) == FP_ZERO && std::isinf(b)) ||
            (std::isinf(a) && std::fpclassify(b) == FP_ZERO)) {
            r = std::signbit(a) ^ std::signbit(b) ? -2.0f : 2.0f;
        }
        dst.lo = to_u32(r);
        break;
    }
    case Op::FPMulX64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        double r = a * b;
        if ((std::fpclassify(a) == FP_ZERO && std::isinf(b)) ||
            (std::isinf(a) && std::fpclassify(b) == FP_ZERO)) {
            r = std::signbit(a) ^ std::signbit(b) ? -2.0 : 2.0;
        }
        dst.lo = to_u64(r);
        break;
    }
    case Op::FPCompare32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const float b = as_f32(read_u32(ctx, inst.GetArg(1)));
        dst.lo = fp_compare_nzcv(a, b);
        break;
    }
    case Op::FPCompare64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const double b = as_f64(read_u64(ctx, inst.GetArg(1)));
        dst.lo = fp_compare_nzcv(a, b);
        break;
    }

    // ---- FP conversions ----
    case Op::FPSingleToDouble: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        dst.lo = to_u64(static_cast<double>(a));
        break;
    }
    case Op::FPDoubleToSingle: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        dst.lo = to_u32(static_cast<float>(a));
        break;
    }
    case Op::FPSingleToFixedS32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const float scaled = a * static_cast<float>(1ull << fbits);
        int32_t r;
        if (std::isnan(scaled)) r = 0;
        else if (scaled >= static_cast<float>(INT32_MAX)) r = INT32_MAX;
        else if (scaled <= static_cast<float>(INT32_MIN)) r = INT32_MIN;
        else r = static_cast<int32_t>(std::trunc(scaled));
        dst.lo = static_cast<uint32_t>(r);
        break;
    }
    case Op::FPSingleToFixedU32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const float scaled = a * static_cast<float>(1ull << fbits);
        uint32_t r;
        if (std::isnan(scaled) || scaled <= 0.0f) r = 0;
        else if (scaled >= 4294967296.0f) r = UINT32_MAX;
        else r = static_cast<uint32_t>(std::trunc(scaled));
        dst.lo = r;
        break;
    }
    case Op::FPDoubleToFixedS32: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = a * static_cast<double>(1ull << fbits);
        int32_t r;
        if (std::isnan(scaled)) r = 0;
        else if (scaled >= static_cast<double>(INT32_MAX)) r = INT32_MAX;
        else if (scaled <= static_cast<double>(INT32_MIN)) r = INT32_MIN;
        else r = static_cast<int32_t>(std::trunc(scaled));
        dst.lo = static_cast<uint32_t>(r);
        break;
    }
    case Op::FPDoubleToFixedU32: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = a * static_cast<double>(1ull << fbits);
        uint32_t r;
        if (std::isnan(scaled) || scaled <= 0.0) r = 0;
        else if (scaled >= 4294967296.0) r = UINT32_MAX;
        else r = static_cast<uint32_t>(std::trunc(scaled));
        dst.lo = r;
        break;
    }
    case Op::FPSingleToFixedS64: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = static_cast<double>(a) * static_cast<double>(1ull << fbits);
        int64_t r;
        if (std::isnan(scaled)) r = 0;
        else if (scaled >= 9223372036854775808.0) r = INT64_MAX;
        else if (scaled < -9223372036854775808.0) r = INT64_MIN;
        else r = static_cast<int64_t>(std::trunc(scaled));
        dst.lo = static_cast<uint64_t>(r);
        break;
    }
    case Op::FPSingleToFixedU64: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = static_cast<double>(a) * static_cast<double>(1ull << fbits);
        uint64_t r;
        if (std::isnan(scaled) || scaled <= 0.0) r = 0;
        else if (scaled >= 18446744073709551616.0) r = UINT64_MAX;
        else r = static_cast<uint64_t>(std::trunc(scaled));
        dst.lo = r;
        break;
    }
    case Op::FPDoubleToFixedS64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = a * static_cast<double>(1ull << fbits);
        int64_t r;
        if (std::isnan(scaled)) r = 0;
        else if (scaled >= 9223372036854775808.0) r = INT64_MAX;
        else if (scaled < -9223372036854775808.0) r = INT64_MIN;
        else r = static_cast<int64_t>(std::trunc(scaled));
        dst.lo = static_cast<uint64_t>(r);
        break;
    }
    case Op::FPDoubleToFixedU64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = a * static_cast<double>(1ull << fbits);
        uint64_t r;
        if (std::isnan(scaled) || scaled <= 0.0) r = 0;
        else if (scaled >= 18446744073709551616.0) r = UINT64_MAX;
        else r = static_cast<uint64_t>(std::trunc(scaled));
        dst.lo = r;
        break;
    }
    case Op::FPFixedS32ToSingle: {
        const int32_t v = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u32(static_cast<float>(v) / static_cast<float>(1ull << fbits));
        break;
    }
    case Op::FPFixedU32ToSingle: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u32(static_cast<float>(v) / static_cast<float>(1ull << fbits));
        break;
    }
    case Op::FPFixedS32ToDouble: {
        const int32_t v = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u64(static_cast<double>(v) / static_cast<double>(1ull << fbits));
        break;
    }
    case Op::FPFixedU32ToDouble: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u64(static_cast<double>(v) / static_cast<double>(1ull << fbits));
        break;
    }
    case Op::FPFixedS64ToSingle: {
        const int64_t v = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u32(static_cast<float>(static_cast<double>(v) / static_cast<double>(1ull << fbits)));
        break;
    }
    case Op::FPFixedU64ToSingle: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u32(static_cast<float>(static_cast<double>(v) / static_cast<double>(1ull << fbits)));
        break;
    }
    case Op::FPFixedS64ToDouble: {
        const int64_t v = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u64(static_cast<double>(v) / static_cast<double>(1ull << fbits));
        break;
    }
    case Op::FPFixedU64ToDouble: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        dst.lo = to_u64(static_cast<double>(v) / static_cast<double>(1ull << fbits));
        break;
    }
    case Op::FPRoundInt32: {
        const float a = as_f32(read_u32(ctx, inst.GetArg(0)));
        const uint8_t rmode = read_u8(ctx, inst.GetArg(1));
        float r;
        switch (rmode) {
        case 0: r = std::nearbyint(a); break;   // RNE
        case 1: r = std::ceil(a); break;        // RP
        case 2: r = std::floor(a); break;       // RM
        case 3: r = std::trunc(a); break;       // RZ
        default: r = std::nearbyint(a); break;
        }
        dst.lo = to_u32(r);
        break;
    }
    case Op::FPRoundInt64: {
        const double a = as_f64(read_u64(ctx, inst.GetArg(0)));
        const uint8_t rmode = read_u8(ctx, inst.GetArg(1));
        double r;
        switch (rmode) {
        case 0: r = std::nearbyint(a); break;
        case 1: r = std::ceil(a); break;
        case 2: r = std::floor(a); break;
        case 3: r = std::trunc(a); break;
        default: r = std::nearbyint(a); break;
        }
        dst.lo = to_u64(r);
        break;
    }

    // ======================================================================
    // M12.5 Vector (NEON) ops -- U128 stored as { lo, hi } little-endian.
    // ======================================================================
    case Op::ZeroVector:
        dst.lo = 0;
        dst.hi = 0;
        break;
    case Op::VectorZeroUpper: {
        const auto [a, b] = read_u128(ctx, inst.GetArg(0));
        (void)b;
        dst.lo = a;
        dst.hi = 0;
        break;
    }
    case Op::VectorZeroExtend8: {
        // Input is a U128 where only the low 64 bits hold the narrow lane
        // data; we zero-extend each byte/half/word to the next wider lane in
        // a full 128-bit result.
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        (void)hi;
        auto res = vec_unop<uint8_t>(lo, 0, [](uint8_t v) { return v; });
        // Zero-extend byte -> half: unpack low 8 bytes into 8 half-words.
        uint8_t src[16];
        std::memcpy(src, &res, 16);
        uint16_t out[8];
        for (int i = 0; i < 8; ++i) out[i] = src[i];
        std::memcpy(&dst.lo, out, 8);
        std::memcpy(&dst.hi, out + 4, 8);
        break;
    }
    case Op::VectorZeroExtend16: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        (void)hi;
        uint16_t src[8];
        std::memcpy(src, &lo, 8);
        std::memcpy(src + 4, &lo, 0); // fill upper 4 from src too; actually we only have 4 input halves from lo's low 64 bits
        // Correct sampling: low 64 bits of input hold 4 halves; zero-extend each to 32.
        uint16_t in4[4];
        std::memcpy(in4, &lo, 8);
        uint32_t out[4];
        for (int i = 0; i < 4; ++i) out[i] = in4[i];
        std::memcpy(&dst.lo, out, 8);
        std::memcpy(&dst.hi, out + 2, 8);
        (void)src;
        break;
    }
    case Op::VectorZeroExtend32: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        (void)hi;
        uint32_t in2[2];
        std::memcpy(in2, &lo, 8);
        uint64_t out[2] = { in2[0], in2[1] };
        dst.lo = out[0];
        dst.hi = out[1];
        break;
    }
    case Op::VectorZeroExtend64: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        (void)hi;
        dst.lo = lo;
        dst.hi = 0;
        break;
    }

    // ---- Element access ----
    case Op::VectorGetElement8: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        dst.lo = vec_get_element<uint8_t>(lo, hi, read_u8(ctx, inst.GetArg(1)));
        break;
    }
    case Op::VectorGetElement16: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        dst.lo = vec_get_element<uint16_t>(lo, hi, read_u8(ctx, inst.GetArg(1)));
        break;
    }
    case Op::VectorGetElement32: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        dst.lo = vec_get_element<uint32_t>(lo, hi, read_u8(ctx, inst.GetArg(1)));
        break;
    }
    case Op::VectorGetElement64: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        dst.lo = vec_get_element<uint64_t>(lo, hi, read_u8(ctx, inst.GetArg(1)));
        break;
    }
    case Op::VectorSetElement8: {
        auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        vec_set_element<uint8_t>(lo, hi, read_u8(ctx, inst.GetArg(1)), read_u8(ctx, inst.GetArg(2)));
        dst.lo = lo; dst.hi = hi;
        break;
    }
    case Op::VectorSetElement16: {
        auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        vec_set_element<uint16_t>(lo, hi, read_u8(ctx, inst.GetArg(1)), read_u16(ctx, inst.GetArg(2)));
        dst.lo = lo; dst.hi = hi;
        break;
    }
    case Op::VectorSetElement32: {
        auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        vec_set_element<uint32_t>(lo, hi, read_u8(ctx, inst.GetArg(1)), read_u32(ctx, inst.GetArg(2)));
        dst.lo = lo; dst.hi = hi;
        break;
    }
    case Op::VectorSetElement64: {
        auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        vec_set_element<uint64_t>(lo, hi, read_u8(ctx, inst.GetArg(1)), read_u64(ctx, inst.GetArg(2)));
        dst.lo = lo; dst.hi = hi;
        break;
    }

    // ---- Broadcasts ----
    case Op::VectorBroadcast8:        std::tie(dst.lo, dst.hi) = vec_broadcast<uint8_t>(read_u8(ctx, inst.GetArg(0))); break;
    case Op::VectorBroadcast16:       std::tie(dst.lo, dst.hi) = vec_broadcast<uint16_t>(read_u16(ctx, inst.GetArg(0))); break;
    case Op::VectorBroadcast32:       std::tie(dst.lo, dst.hi) = vec_broadcast<uint32_t>(read_u32(ctx, inst.GetArg(0))); break;
    case Op::VectorBroadcast64:       std::tie(dst.lo, dst.hi) = vec_broadcast<uint64_t>(read_u64(ctx, inst.GetArg(0))); break;
    case Op::VectorBroadcastLower8:   std::tie(dst.lo, dst.hi) = vec_broadcast<uint8_t>(read_u8(ctx, inst.GetArg(0)), /*lower_only=*/true); break;
    case Op::VectorBroadcastLower16:  std::tie(dst.lo, dst.hi) = vec_broadcast<uint16_t>(read_u16(ctx, inst.GetArg(0)), true); break;
    case Op::VectorBroadcastLower32:  std::tie(dst.lo, dst.hi) = vec_broadcast<uint32_t>(read_u32(ctx, inst.GetArg(0)), true); break;
    case Op::VectorBroadcastElement8: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint8_t>(vec_get_element<uint8_t>(lo, hi, read_u8(ctx, inst.GetArg(1))));
        break;
    }
    case Op::VectorBroadcastElement16: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint16_t>(vec_get_element<uint16_t>(lo, hi, read_u8(ctx, inst.GetArg(1))));
        break;
    }
    case Op::VectorBroadcastElement32: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint32_t>(vec_get_element<uint32_t>(lo, hi, read_u8(ctx, inst.GetArg(1))));
        break;
    }
    case Op::VectorBroadcastElement64: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint64_t>(vec_get_element<uint64_t>(lo, hi, read_u8(ctx, inst.GetArg(1))));
        break;
    }
    case Op::VectorBroadcastElementLower8: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint8_t>(vec_get_element<uint8_t>(lo, hi, read_u8(ctx, inst.GetArg(1))), true);
        break;
    }
    case Op::VectorBroadcastElementLower16: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint16_t>(vec_get_element<uint16_t>(lo, hi, read_u8(ctx, inst.GetArg(1))), true);
        break;
    }
    case Op::VectorBroadcastElementLower32: {
        const auto [lo, hi] = read_u128(ctx, inst.GetArg(0));
        std::tie(dst.lo, dst.hi) = vec_broadcast<uint32_t>(vec_get_element<uint32_t>(lo, hi, read_u8(ctx, inst.GetArg(1))), true);
        break;
    }

    // ---- Bit-wise ----
    case Op::VectorAnd: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        dst.lo = al & bl; dst.hi = ah & bh;
        break;
    }
    case Op::VectorAndNot: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        dst.lo = al & ~bl; dst.hi = ah & ~bh;
        break;
    }
    case Op::VectorOr: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        dst.lo = al | bl; dst.hi = ah | bh;
        break;
    }
    case Op::VectorEor: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        dst.lo = al ^ bl; dst.hi = ah ^ bh;
        break;
    }
    case Op::VectorNot: {
        const auto [a, b] = read_u128(ctx, inst.GetArg(0));
        dst.lo = ~a; dst.hi = ~b;
        break;
    }

    // ---- Arithmetic (add/sub/neg/abs) ----
#define IR_VEC_BIN(OP, T, EXPR) case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        std::tie(dst.lo, dst.hi) = vec_binop<T>(al, ah, bl, bh, [](T a, T b) { return static_cast<T>(EXPR); }); \
        break; }
#define IR_VEC_UN(OP, T, EXPR) case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        std::tie(dst.lo, dst.hi) = vec_unop<T>(al, ah, [](T v) { return static_cast<T>(EXPR); }); \
        break; }

    IR_VEC_BIN(VectorAdd8,  uint8_t,  (a + b))
    IR_VEC_BIN(VectorAdd16, uint16_t, (a + b))
    IR_VEC_BIN(VectorAdd32, uint32_t, (a + b))
    IR_VEC_BIN(VectorAdd64, uint64_t, (a + b))
    IR_VEC_BIN(VectorSub8,  uint8_t,  (a - b))
    IR_VEC_BIN(VectorSub16, uint16_t, (a - b))
    IR_VEC_BIN(VectorSub32, uint32_t, (a - b))
    IR_VEC_BIN(VectorSub64, uint64_t, (a - b))
    IR_VEC_BIN(VectorMultiply8,  uint8_t,  (a * b))
    IR_VEC_BIN(VectorMultiply16, uint16_t, (a * b))
    IR_VEC_BIN(VectorMultiply32, uint32_t, (a * b))
    IR_VEC_BIN(VectorMultiply64, uint64_t, (a * b))
    IR_VEC_UN(VectorAbs8,  int8_t,  (v < 0 ? -v : v))
    IR_VEC_UN(VectorAbs16, int16_t, (v < 0 ? -v : v))
    IR_VEC_UN(VectorAbs32, int32_t, (v < 0 ? -v : v))
    IR_VEC_UN(VectorAbs64, int64_t, (v < 0 ? -v : v))

    // ---- Shifts (immediate count in U8 arg1) ----
    case Op::VectorLogicalShiftLeft8:  { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint8_t>(al, ah, [s](uint8_t v) { return static_cast<uint8_t>(s < 8 ? (v << s) : 0); }); break; }
    case Op::VectorLogicalShiftLeft16: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint16_t>(al, ah, [s](uint16_t v) { return static_cast<uint16_t>(s < 16 ? (v << s) : 0); }); break; }
    case Op::VectorLogicalShiftLeft32: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint32_t>(al, ah, [s](uint32_t v) { return s < 32 ? (v << s) : 0u; }); break; }
    case Op::VectorLogicalShiftLeft64: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint64_t>(al, ah, [s](uint64_t v) { return s < 64 ? (v << s) : 0ull; }); break; }
    case Op::VectorLogicalShiftRight8:  { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint8_t>(al, ah, [s](uint8_t v) { return static_cast<uint8_t>(s < 8 ? (v >> s) : 0); }); break; }
    case Op::VectorLogicalShiftRight16: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint16_t>(al, ah, [s](uint16_t v) { return static_cast<uint16_t>(s < 16 ? (v >> s) : 0); }); break; }
    case Op::VectorLogicalShiftRight32: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint32_t>(al, ah, [s](uint32_t v) { return s < 32 ? (v >> s) : 0u; }); break; }
    case Op::VectorLogicalShiftRight64: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<uint64_t>(al, ah, [s](uint64_t v) { return s < 64 ? (v >> s) : 0ull; }); break; }
    case Op::VectorArithmeticShiftRight8:  { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<int8_t>(al, ah, [s](int8_t v) { const uint8_t sc = s < 8 ? s : 7; return static_cast<int8_t>(v >> sc); }); break; }
    case Op::VectorArithmeticShiftRight16: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<int16_t>(al, ah, [s](int16_t v) { const uint8_t sc = s < 16 ? s : 15; return static_cast<int16_t>(v >> sc); }); break; }
    case Op::VectorArithmeticShiftRight32: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<int32_t>(al, ah, [s](int32_t v) { const uint8_t sc = s < 32 ? s : 31; return v >> sc; }); break; }
    case Op::VectorArithmeticShiftRight64: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const uint8_t s = read_u8(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_unop<int64_t>(al, ah, [s](int64_t v) { const uint8_t sc = s < 64 ? s : 63; return v >> sc; }); break; }

    // ---- Compares (per-lane) ----
    case Op::VectorEqual8:  { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_cmp<uint8_t>(al, ah, bl, bh, [](uint8_t a, uint8_t b) { return a == b; }); break; }
    case Op::VectorEqual16: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_cmp<uint16_t>(al, ah, bl, bh, [](uint16_t a, uint16_t b) { return a == b; }); break; }
    case Op::VectorEqual32: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_cmp<uint32_t>(al, ah, bl, bh, [](uint32_t a, uint32_t b) { return a == b; }); break; }
    case Op::VectorEqual64: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); std::tie(dst.lo, dst.hi) = vec_cmp<uint64_t>(al, ah, bl, bh, [](uint64_t a, uint64_t b) { return a == b; }); break; }
    case Op::VectorEqual128: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); const bool eq = (al == bl) && (ah == bh); dst.lo = eq ? ~0ull : 0ull; dst.hi = eq ? ~0ull : 0ull; break; }

    // ---- Min/Max signed+unsigned ----
    IR_VEC_BIN(VectorMaxS8,  int8_t,  (a > b ? a : b))
    IR_VEC_BIN(VectorMaxS16, int16_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMaxS32, int32_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMaxS64, int64_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMaxU8,  uint8_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMaxU16, uint16_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMaxU32, uint32_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMaxU64, uint64_t, (a > b ? a : b))
    IR_VEC_BIN(VectorMinS8,  int8_t,  (a < b ? a : b))
    IR_VEC_BIN(VectorMinS16, int16_t, (a < b ? a : b))
    IR_VEC_BIN(VectorMinS32, int32_t, (a < b ? a : b))
    IR_VEC_BIN(VectorMinS64, int64_t, (a < b ? a : b))
    IR_VEC_BIN(VectorMinU8,  uint8_t, (a < b ? a : b))
    IR_VEC_BIN(VectorMinU16, uint16_t, (a < b ? a : b))
    IR_VEC_BIN(VectorMinU32, uint32_t, (a < b ? a : b))
    IR_VEC_BIN(VectorMinU64, uint64_t, (a < b ? a : b))

#undef IR_VEC_BIN
#undef IR_VEC_UN

    // ---- Extract / interleave (lower halves) ----
    case Op::VectorExtract: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        const uint8_t pos_bits = read_u8(ctx, inst.GetArg(2));
        // pos_bits is in bits, 0..127, multiple of 8. Produce the 128-bit
        // window starting `pos_bits` bits into the concatenation (a:b).
        uint8_t src[32];
        std::memcpy(src,      &al, 8);
        std::memcpy(src + 8,  &ah, 8);
        std::memcpy(src + 16, &bl, 8);
        std::memcpy(src + 24, &bh, 8);
        const uint8_t off = (pos_bits / 8) & 0x1F;
        uint8_t out[16];
        for (int i = 0; i < 16; ++i) out[i] = src[off + i];
        std::memcpy(&dst.lo, out, 8);
        std::memcpy(&dst.hi, out + 8, 8);
        break;
    }
    case Op::VectorExtractLower: {
        // Dynarmic A32 ASIMD: 64-bit vectors only — concatenate Vn[63:0] : Vm[63:0] (16 bytes),
        // then take 8 consecutive bytes starting at (position_bits / 8). Matches A64 EXT Dd,Dn,Dm,#imm.
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        (void)ah;
        (void)bh;
        const uint8_t pos_bits = read_u8(ctx, inst.GetArg(2));
        uint8_t src[16];
        std::memcpy(src, &al, 8);
        std::memcpy(src + 8, &bl, 8);
        // 16-byte window, 8-byte result → start index 0..8 (inclusive)
        const unsigned off = std::min(8u, static_cast<unsigned>(pos_bits / 8u));
        uint64_t out64 = 0;
        std::memcpy(&out64, src + off, 8);
        dst.lo = out64;
        dst.hi = 0;
        break;
    }
    case Op::VectorInterleaveLower8:  { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)ah; (void)bh; uint8_t A[8], B[8], R[16]; std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8); for (int i = 0; i < 8; ++i) { R[2*i] = A[i]; R[2*i+1] = B[i]; } std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 8, 8); break; }
    case Op::VectorInterleaveLower16: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)ah; (void)bh; uint16_t A[4], B[4], R[8]; std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8); for (int i = 0; i < 4; ++i) { R[2*i] = A[i]; R[2*i+1] = B[i]; } std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 4, 8); break; }
    case Op::VectorInterleaveLower32: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)ah; (void)bh; uint32_t A[2], B[2], R[4]; std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8); R[0] = A[0]; R[1] = B[0]; R[2] = A[1]; R[3] = B[1]; std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 2, 8); break; }
    case Op::VectorInterleaveLower64: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)ah; (void)bh; dst.lo = al; dst.hi = bl; break; }

    // ======================================================================
    // M12.5 gap-fill: remaining FPFixed{S,U}16 conversions.
    // ======================================================================
    case Op::FPFixedS16ToSingle: { const int16_t v = static_cast<int16_t>(read_u16(ctx, inst.GetArg(0))); const uint8_t fbits = read_u8(ctx, inst.GetArg(1)); dst.lo = to_u32(static_cast<float>(v) / static_cast<float>(1ull << fbits)); break; }
    case Op::FPFixedU16ToSingle: { const uint16_t v = read_u16(ctx, inst.GetArg(0)); const uint8_t fbits = read_u8(ctx, inst.GetArg(1)); dst.lo = to_u32(static_cast<float>(v) / static_cast<float>(1ull << fbits)); break; }
    case Op::FPFixedS16ToDouble: { const int16_t v = static_cast<int16_t>(read_u16(ctx, inst.GetArg(0))); const uint8_t fbits = read_u8(ctx, inst.GetArg(1)); dst.lo = to_u64(static_cast<double>(v) / static_cast<double>(1ull << fbits)); break; }
    case Op::FPFixedU16ToDouble: { const uint16_t v = read_u16(ctx, inst.GetArg(0)); const uint8_t fbits = read_u8(ctx, inst.GetArg(1)); dst.lo = to_u64(static_cast<double>(v) / static_cast<double>(1ull << fbits)); break; }

    // ======================================================================
    // M12.5 gap-fill: scalar saturation ops
    // ======================================================================
    case Op::SignedSaturation: {
        const int32_t v = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const uint8_t n = read_u8(ctx, inst.GetArg(1));
        // Clamp v into the N-bit signed range [-(1<<(N-1)), (1<<(N-1))-1].
        if (n == 0 || n > 32) { dst.lo = static_cast<uint32_t>(v); break; }
        const int32_t lo = -(1 << (n - 1));
        const int32_t hi = (1 << (n - 1)) - 1;
        int32_t r = v;
        bool sat = false;
        if (v < lo) { r = lo; sat = true; }
        else if (v > hi) { r = hi; sat = true; }
        dst.lo = static_cast<uint32_t>(r);
        dst.ge = sat ? 1u : 0u;   // re-purposed side-channel for GetGEFromOp
        break;
    }
    case Op::UnsignedSaturation: {
        const int32_t v = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const uint8_t n = read_u8(ctx, inst.GetArg(1));
        if (n == 0 || n > 32) { dst.lo = static_cast<uint32_t>(v); break; }
        const int64_t hi = (1ll << n) - 1;
        int64_t r = v;
        bool sat = false;
        if (r < 0) { r = 0; sat = true; }
        else if (r > hi) { r = hi; sat = true; }
        dst.lo = static_cast<uint32_t>(r);
        dst.ge = sat ? 1u : 0u;
        break;
    }
    case Op::SignedSaturatedAddWithFlag32: {
        const int32_t a = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const int32_t b = static_cast<int32_t>(read_u32(ctx, inst.GetArg(1)));
        const int64_t sum = static_cast<int64_t>(a) + b;
        const int32_t r = sat_clamp_s<int32_t>(sum);
        dst.lo = static_cast<uint32_t>(r);
        dst.ge = (sum != r) ? 1u : 0u;  // Q-flag side channel
        break;
    }
    case Op::SignedSaturatedSubWithFlag32: {
        const int32_t a = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const int32_t b = static_cast<int32_t>(read_u32(ctx, inst.GetArg(1)));
        const int64_t diff = static_cast<int64_t>(a) - b;
        const int32_t r = sat_clamp_s<int32_t>(diff);
        dst.lo = static_cast<uint32_t>(r);
        dst.ge = (diff != r) ? 1u : 0u;
        break;
    }
#define IR_SAT_BIN(OP, T, TI, EXPR_W, CLAMP) \
    case Op::OP: { \
        const TI a = static_cast<TI>(static_cast<T>(read_any(ctx, inst.GetArg(0)))); \
        const TI b = static_cast<TI>(static_cast<T>(read_any(ctx, inst.GetArg(1)))); \
        const int64_t full = static_cast<int64_t>(EXPR_W); \
        dst.lo = static_cast<uint64_t>(static_cast<T>(CLAMP<T>(full))); \
        break; }
    IR_SAT_BIN(SignedSaturatedAdd8,  int8_t,  int16_t, static_cast<int32_t>(a) + b, sat_clamp_s)
    IR_SAT_BIN(SignedSaturatedAdd16, int16_t, int32_t, static_cast<int64_t>(a) + b, sat_clamp_s)
    IR_SAT_BIN(SignedSaturatedAdd32, int32_t, int64_t, static_cast<int64_t>(a) + b, sat_clamp_s)
    IR_SAT_BIN(SignedSaturatedSub8,  int8_t,  int16_t, static_cast<int32_t>(a) - b, sat_clamp_s)
    IR_SAT_BIN(SignedSaturatedSub16, int16_t, int32_t, static_cast<int64_t>(a) - b, sat_clamp_s)
    IR_SAT_BIN(SignedSaturatedSub32, int32_t, int64_t, static_cast<int64_t>(a) - b, sat_clamp_s)
    IR_SAT_BIN(UnsignedSaturatedAdd8,  uint8_t,  uint16_t, static_cast<int64_t>(a) + b, sat_clamp_u)
    IR_SAT_BIN(UnsignedSaturatedAdd16, uint16_t, uint32_t, static_cast<int64_t>(a) + b, sat_clamp_u)
    IR_SAT_BIN(UnsignedSaturatedAdd32, uint32_t, uint64_t, static_cast<int64_t>(a) + b, sat_clamp_u)
    IR_SAT_BIN(UnsignedSaturatedSub8,  uint8_t,  uint16_t, static_cast<int64_t>(a) - b, sat_clamp_u)
    IR_SAT_BIN(UnsignedSaturatedSub16, uint16_t, uint32_t, static_cast<int64_t>(a) - b, sat_clamp_u)
    IR_SAT_BIN(UnsignedSaturatedSub32, uint32_t, uint64_t, static_cast<int64_t>(a) - b, sat_clamp_u)
#undef IR_SAT_BIN
    case Op::SignedSaturatedAdd64: {
        const int64_t a = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const int64_t b = static_cast<int64_t>(read_u64(ctx, inst.GetArg(1)));
        // Checked add via unsigned wrap-around + overflow detection.
        const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
        const uint64_t r = ua + ub;
        const bool overflow = ((~(a ^ b) & (a ^ static_cast<int64_t>(r))) >> 63) & 1;
        if (overflow) {
            dst.lo = (a >= 0) ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(INT64_MIN);
        } else {
            dst.lo = r;
        }
        break;
    }
    case Op::SignedSaturatedSub64: {
        const int64_t a = static_cast<int64_t>(read_u64(ctx, inst.GetArg(0)));
        const int64_t b = static_cast<int64_t>(read_u64(ctx, inst.GetArg(1)));
        const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
        const uint64_t r = ua - ub;
        const bool overflow = (((a ^ b) & (a ^ static_cast<int64_t>(r))) >> 63) & 1;
        if (overflow) {
            dst.lo = (a >= 0) ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(INT64_MIN);
        } else {
            dst.lo = r;
        }
        break;
    }
    case Op::UnsignedSaturatedAdd64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        const uint64_t r = a + b;
        dst.lo = (r < a) ? UINT64_MAX : r;
        break;
    }
    case Op::UnsignedSaturatedSub64: {
        const uint64_t a = read_u64(ctx, inst.GetArg(0));
        const uint64_t b = read_u64(ctx, inst.GetArg(1));
        dst.lo = (a < b) ? 0 : (a - b);
        break;
    }
    case Op::SignedSaturatedDoublingMultiplyReturnHigh16: {
        const int16_t a = static_cast<int16_t>(read_u16(ctx, inst.GetArg(0)));
        const int16_t b = static_cast<int16_t>(read_u16(ctx, inst.GetArg(1)));
        int32_t product = static_cast<int32_t>(a) * b * 2;
        if (a == INT16_MIN && b == INT16_MIN) product = INT32_MAX;  // saturating doubling
        dst.lo = static_cast<uint16_t>(product >> 16);
        break;
    }
    case Op::SignedSaturatedDoublingMultiplyReturnHigh32: {
        const int32_t a = static_cast<int32_t>(read_u32(ctx, inst.GetArg(0)));
        const int32_t b = static_cast<int32_t>(read_u32(ctx, inst.GetArg(1)));
        int64_t product = static_cast<int64_t>(a) * b * 2;
        if (a == INT32_MIN && b == INT32_MIN) product = INT64_MAX;
        dst.lo = static_cast<uint32_t>(product >> 32);
        break;
    }

    // ======================================================================
    // M12.5 gap-fill: packed 8/16-bit SIMD in 32-bit register (ARMv6 DSPe)
    //
    // Lane layout: for {8}, regs split into 4 lanes at bits [7:0], [15:8],
    // [23:16], [31:24].  For {16}, 2 lanes at [15:0] and [31:16].  Signed
    // variants reinterpret each lane as int8_t/int16_t.
    // ======================================================================
    {
        // Block-local helpers for packed ops.
        auto pack4x8  = [](uint8_t  b0, uint8_t  b1, uint8_t  b2, uint8_t  b3) -> uint32_t {
            return (uint32_t{b3} << 24) | (uint32_t{b2} << 16) | (uint32_t{b1} << 8) | b0;
        };
        auto pack2x16 = [](uint16_t h0, uint16_t h1) -> uint32_t {
            return (uint32_t{h1} << 16) | h0;
        };
        (void)pack4x8; (void)pack2x16;
    }
#define IR_PKD8(OP, T, EXPR)  \
    case Op::OP: { \
        const uint32_t a = read_u32(ctx, inst.GetArg(0)); \
        const uint32_t b = read_u32(ctx, inst.GetArg(1)); \
        T a0 = static_cast<T>(a & 0xFF), a1 = static_cast<T>((a >> 8) & 0xFF), a2 = static_cast<T>((a >> 16) & 0xFF), a3 = static_cast<T>((a >> 24) & 0xFF); \
        T b0 = static_cast<T>(b & 0xFF), b1 = static_cast<T>((b >> 8) & 0xFF), b2 = static_cast<T>((b >> 16) & 0xFF), b3 = static_cast<T>((b >> 24) & 0xFF); \
        uint32_t r = (uint32_t)((uint8_t)(EXPR(a0, b0)))        | ((uint32_t)(uint8_t)(EXPR(a1, b1)) << 8) | ((uint32_t)(uint8_t)(EXPR(a2, b2)) << 16) | ((uint32_t)(uint8_t)(EXPR(a3, b3)) << 24); \
        dst.lo = r; break; }
#define IR_PKD16(OP, T, EXPR) \
    case Op::OP: { \
        const uint32_t a = read_u32(ctx, inst.GetArg(0)); \
        const uint32_t b = read_u32(ctx, inst.GetArg(1)); \
        T a0 = static_cast<T>(a & 0xFFFF), a1 = static_cast<T>((a >> 16) & 0xFFFF); \
        T b0 = static_cast<T>(b & 0xFFFF), b1 = static_cast<T>((b >> 16) & 0xFFFF); \
        uint32_t r = (uint32_t)((uint16_t)(EXPR(a0, b0))) | ((uint32_t)(uint16_t)(EXPR(a1, b1)) << 16); \
        dst.lo = r; break; }

    IR_PKD8 (PackedAddU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x + y); })
    IR_PKD8 (PackedAddS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t>(x + y); })
    IR_PKD8 (PackedSubU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x - y); })
    IR_PKD8 (PackedSubS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t>(x - y); })
    IR_PKD16(PackedAddU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x + y); })
    IR_PKD16(PackedAddS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>(x + y); })
    IR_PKD16(PackedSubU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x - y); })
    IR_PKD16(PackedSubS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>(x - y); })

    // Halving = average = (a+b) >> 1; handles overflow by keeping the carry.
    IR_PKD8 (PackedHalvingAddU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>((static_cast<uint16_t>(x) + y) >> 1); })
    IR_PKD8 (PackedHalvingAddS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t>((static_cast<int16_t>(x) + y) >> 1); })
    IR_PKD8 (PackedHalvingSubU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>((static_cast<int16_t>(x) - y) >> 1); })
    IR_PKD8 (PackedHalvingSubS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t>((static_cast<int16_t>(x) - y) >> 1); })
    IR_PKD16(PackedHalvingAddU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>((static_cast<uint32_t>(x) + y) >> 1); })
    IR_PKD16(PackedHalvingAddS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>((static_cast<int32_t>(x) + y) >> 1); })
    IR_PKD16(PackedHalvingSubU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>((static_cast<int32_t>(x) - y) >> 1); })
    IR_PKD16(PackedHalvingSubS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>((static_cast<int32_t>(x) - y) >> 1); })

    // Saturated: clamp each lane to T's range.
    IR_PKD8 (PackedSaturatedAddU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return sat_clamp_u<uint8_t>(static_cast<int64_t>(x) + y); })
    IR_PKD8 (PackedSaturatedAddS8,  int8_t,   [](int8_t   x, int8_t   y) { return sat_clamp_s<int8_t>(static_cast<int64_t>(x) + y); })
    IR_PKD8 (PackedSaturatedSubU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return sat_clamp_u<uint8_t>(static_cast<int64_t>(x) - y); })
    IR_PKD8 (PackedSaturatedSubS8,  int8_t,   [](int8_t   x, int8_t   y) { return sat_clamp_s<int8_t>(static_cast<int64_t>(x) - y); })
    IR_PKD16(PackedSaturatedAddU16, uint16_t, [](uint16_t x, uint16_t y) { return sat_clamp_u<uint16_t>(static_cast<int64_t>(x) + y); })
    IR_PKD16(PackedSaturatedAddS16, int16_t,  [](int16_t  x, int16_t  y) { return sat_clamp_s<int16_t>(static_cast<int64_t>(x) + y); })
    IR_PKD16(PackedSaturatedSubU16, uint16_t, [](uint16_t x, uint16_t y) { return sat_clamp_u<uint16_t>(static_cast<int64_t>(x) - y); })
    IR_PKD16(PackedSaturatedSubS16, int16_t,  [](int16_t  x, int16_t  y) { return sat_clamp_s<int16_t>(static_cast<int64_t>(x) - y); })

#undef IR_PKD8
#undef IR_PKD16

    // Mixed halving add/sub (2-lane halves only).  For UADDSUBX / ASX / SAX /
    // UHASX variants: r[lane0] = a[lane0] op b[lane1], r[lane1] = a[lane1] op b[lane0].
    case Op::PackedAddSubU16:
    case Op::PackedAddSubS16:
    case Op::PackedSubAddU16:
    case Op::PackedSubAddS16:
    case Op::PackedHalvingAddSubU16:
    case Op::PackedHalvingAddSubS16:
    case Op::PackedHalvingSubAddU16:
    case Op::PackedHalvingSubAddS16: {
        const uint32_t a = read_u32(ctx, inst.GetArg(0));
        const uint32_t b = read_u32(ctx, inst.GetArg(1));
        auto lane_op = [op](uint16_t ax, uint16_t bx, bool do_sub, bool halve) -> uint16_t {
            if (op == Op::PackedAddSubS16 || op == Op::PackedSubAddS16 || op == Op::PackedHalvingAddSubS16 || op == Op::PackedHalvingSubAddS16) {
                const int32_t x = static_cast<int16_t>(ax);
                const int32_t y = static_cast<int16_t>(bx);
                int32_t r = do_sub ? (x - y) : (x + y);
                if (halve) r >>= 1;
                return static_cast<uint16_t>(static_cast<int16_t>(r));
            } else {
                const int32_t x = ax;
                const int32_t y = bx;
                int32_t r = do_sub ? (x - y) : (x + y);
                if (halve) r = static_cast<uint32_t>(r) >> 1;
                return static_cast<uint16_t>(r);
            }
        };
        const bool halve = (op == Op::PackedHalvingAddSubU16 || op == Op::PackedHalvingAddSubS16 || op == Op::PackedHalvingSubAddU16 || op == Op::PackedHalvingSubAddS16);
        const bool addsub = (op == Op::PackedAddSubU16 || op == Op::PackedAddSubS16 || op == Op::PackedHalvingAddSubU16 || op == Op::PackedHalvingAddSubS16);
        const uint16_t a0 = static_cast<uint16_t>(a), a1 = static_cast<uint16_t>(a >> 16);
        const uint16_t b0 = static_cast<uint16_t>(b), b1 = static_cast<uint16_t>(b >> 16);
        // ADDSUB (ASX): lane0 = a0 - b1, lane1 = a1 + b0   (per ARM manual)
        // SUBADD (SAX): lane0 = a0 + b1, lane1 = a1 - b0
        const uint16_t r0 = addsub ? lane_op(a0, b1, /*sub=*/true,  halve) : lane_op(a0, b1, /*sub=*/false, halve);
        const uint16_t r1 = addsub ? lane_op(a1, b0, /*sub=*/false, halve) : lane_op(a1, b0, /*sub=*/true,  halve);
        dst.lo = (static_cast<uint32_t>(r1) << 16) | r0;
        break;
    }

    case Op::PackedAbsDiffSumU8: {
        const uint32_t a = read_u32(ctx, inst.GetArg(0));
        const uint32_t b = read_u32(ctx, inst.GetArg(1));
        uint32_t sum = 0;
        for (int i = 0; i < 4; ++i) {
            const int16_t diff = static_cast<int16_t>((a >> (i * 8)) & 0xFF) -
                                 static_cast<int16_t>((b >> (i * 8)) & 0xFF);
            sum += (diff < 0 ? -diff : diff);
        }
        dst.lo = sum;
        break;
    }
    case Op::PackedSelect: {
        const uint32_t ge = read_u32(ctx, inst.GetArg(0)) & 0xFu;
        const uint32_t a  = read_u32(ctx, inst.GetArg(1));
        const uint32_t b  = read_u32(ctx, inst.GetArg(2));
        uint32_t r = 0;
        for (int i = 0; i < 4; ++i) {
            const uint32_t m = (ge >> i) & 1u ? 0xFFu : 0x00u;
            const uint32_t byte = ((a >> (i * 8)) & m) | ((b >> (i * 8)) & ~m & 0xFFu);
            r |= byte << (i * 8);
        }
        dst.lo = r;
        break;
    }

    // ======================================================================
    // M12.5 gap-fill: Vector saturated / accumulate / widen-multiply / shifts
    // ======================================================================
#define IR_VEC_BIN(OP, T, EXPR) case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        std::tie(dst.lo, dst.hi) = vec_binop<T>(al, ah, bl, bh, [](T a, T b) { return static_cast<T>(EXPR); }); \
        break; }
#define IR_VEC_UN(OP, T, EXPR) case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        std::tie(dst.lo, dst.hi) = vec_unop<T>(al, ah, [](T v) { return static_cast<T>(EXPR); }); \
        break; }

    IR_VEC_BIN(VectorSignedSaturatedAdd8,  int8_t,  (sat_clamp_s<int8_t> (static_cast<int64_t>(a) + b)))
    IR_VEC_BIN(VectorSignedSaturatedAdd16, int16_t, (sat_clamp_s<int16_t>(static_cast<int64_t>(a) + b)))
    IR_VEC_BIN(VectorSignedSaturatedAdd32, int32_t, (sat_clamp_s<int32_t>(static_cast<int64_t>(a) + b)))
    IR_VEC_BIN(VectorSignedSaturatedSub8,  int8_t,  (sat_clamp_s<int8_t> (static_cast<int64_t>(a) - b)))
    IR_VEC_BIN(VectorSignedSaturatedSub16, int16_t, (sat_clamp_s<int16_t>(static_cast<int64_t>(a) - b)))
    IR_VEC_BIN(VectorSignedSaturatedSub32, int32_t, (sat_clamp_s<int32_t>(static_cast<int64_t>(a) - b)))
    IR_VEC_BIN(VectorUnsignedSaturatedAdd8,  uint8_t,  (sat_clamp_u<uint8_t> (static_cast<int64_t>(a) + b)))
    IR_VEC_BIN(VectorUnsignedSaturatedAdd16, uint16_t, (sat_clamp_u<uint16_t>(static_cast<int64_t>(a) + b)))
    IR_VEC_BIN(VectorUnsignedSaturatedAdd32, uint32_t, (sat_clamp_u<uint32_t>(static_cast<int64_t>(a) + b)))
    IR_VEC_BIN(VectorUnsignedSaturatedSub8,  uint8_t,  (sat_clamp_u<uint8_t> (static_cast<int64_t>(a) - b)))
    IR_VEC_BIN(VectorUnsignedSaturatedSub16, uint16_t, (sat_clamp_u<uint16_t>(static_cast<int64_t>(a) - b)))
    IR_VEC_BIN(VectorUnsignedSaturatedSub32, uint32_t, (sat_clamp_u<uint32_t>(static_cast<int64_t>(a) - b)))

    // 64-bit signed saturated add/sub with explicit overflow detect.
    case Op::VectorSignedSaturatedAdd64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        auto sat64 = [](int64_t a, int64_t b) -> int64_t {
            const uint64_t ua = a, ub = b, r = ua + ub;
            if (((~(a ^ b) & (a ^ static_cast<int64_t>(r))) >> 63) & 1)
                return a >= 0 ? INT64_MAX : INT64_MIN;
            return static_cast<int64_t>(r);
        };
        const int64_t r0 = sat64(static_cast<int64_t>(al), static_cast<int64_t>(bl));
        const int64_t r1 = sat64(static_cast<int64_t>(ah), static_cast<int64_t>(bh));
        dst.lo = static_cast<uint64_t>(r0); dst.hi = static_cast<uint64_t>(r1);
        break;
    }
    case Op::VectorSignedSaturatedSub64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        auto sat64 = [](int64_t a, int64_t b) -> int64_t {
            const uint64_t ua = a, ub = b, r = ua - ub;
            if ((((a ^ b) & (a ^ static_cast<int64_t>(r))) >> 63) & 1)
                return a >= 0 ? INT64_MAX : INT64_MIN;
            return static_cast<int64_t>(r);
        };
        const int64_t r0 = sat64(static_cast<int64_t>(al), static_cast<int64_t>(bl));
        const int64_t r1 = sat64(static_cast<int64_t>(ah), static_cast<int64_t>(bh));
        dst.lo = static_cast<uint64_t>(r0); dst.hi = static_cast<uint64_t>(r1);
        break;
    }
    case Op::VectorUnsignedSaturatedAdd64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        dst.lo = (al + bl) < al ? UINT64_MAX : (al + bl);
        dst.hi = (ah + bh) < ah ? UINT64_MAX : (ah + bh);
        break;
    }
    case Op::VectorUnsignedSaturatedSub64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        dst.lo = (al < bl) ? 0ull : (al - bl);
        dst.hi = (ah < bh) ? 0ull : (ah - bh);
        break;
    }

    // Signed saturated abs: if v == MIN, saturate to MAX.
    IR_VEC_UN(VectorSignedSaturatedAbs8,  int8_t,  (v == INT8_MIN  ? INT8_MAX  : (v < 0 ? -v : v)))
    IR_VEC_UN(VectorSignedSaturatedAbs16, int16_t, (v == INT16_MIN ? INT16_MAX : (v < 0 ? -v : v)))
    IR_VEC_UN(VectorSignedSaturatedAbs32, int32_t, (v == INT32_MIN ? INT32_MAX : (v < 0 ? -v : v)))
    IR_VEC_UN(VectorSignedSaturatedAbs64, int64_t, (v == INT64_MIN ? INT64_MAX : (v < 0 ? -v : v)))
    IR_VEC_UN(VectorSignedSaturatedNeg8,  int8_t,  (v == INT8_MIN  ? INT8_MAX  : -v))
    IR_VEC_UN(VectorSignedSaturatedNeg16, int16_t, (v == INT16_MIN ? INT16_MAX : -v))
    IR_VEC_UN(VectorSignedSaturatedNeg32, int32_t, (v == INT32_MIN ? INT32_MAX : -v))
    IR_VEC_UN(VectorSignedSaturatedNeg64, int64_t, (v == INT64_MIN ? INT64_MAX : -v))

    // Signed saturated accumulate unsigned: signed lane += unsigned lane, clamp to signed range.
    IR_VEC_BIN(VectorSignedSaturatedAccumulateUnsigned8,  int8_t,  (sat_clamp_s<int8_t> (static_cast<int64_t>(a) + static_cast<uint8_t>(b))))
    IR_VEC_BIN(VectorSignedSaturatedAccumulateUnsigned16, int16_t, (sat_clamp_s<int16_t>(static_cast<int64_t>(a) + static_cast<uint16_t>(b))))
    IR_VEC_BIN(VectorSignedSaturatedAccumulateUnsigned32, int32_t, (sat_clamp_s<int32_t>(static_cast<int64_t>(a) + static_cast<uint32_t>(b))))
    // 64-bit variant needs explicit overflow detection.
    case Op::VectorSignedSaturatedAccumulateUnsigned64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        auto lane = [](int64_t a, uint64_t b) -> int64_t {
            const uint64_t sum = static_cast<uint64_t>(a) + b;
            if (a >= 0 && sum < static_cast<uint64_t>(a)) return INT64_MAX;
            if (a < 0  && sum >= static_cast<uint64_t>(INT64_MAX)) {
                // Overflow into sign bit: always saturates to MAX since b >= 0
                // only when sum can't fit in signed 64.
                if (static_cast<int64_t>(sum) < 0 && a >= 0) return INT64_MAX;
            }
            if (static_cast<int64_t>(sum) < a && b != 0) return INT64_MAX;
            return static_cast<int64_t>(sum);
        };
        dst.lo = static_cast<uint64_t>(lane(static_cast<int64_t>(al), bl));
        dst.hi = static_cast<uint64_t>(lane(static_cast<int64_t>(ah), bh));
        break;
    }

    // Doubling multiply return high (SQDMULH / SQRDMULH).
    IR_VEC_BIN(VectorSignedSaturatedDoublingMultiplyHigh16, int16_t, (([&]() { int32_t p = static_cast<int32_t>(a) * b * 2; if (a == INT16_MIN && b == INT16_MIN) return static_cast<int16_t>(INT16_MAX); return static_cast<int16_t>(p >> 16); }())))
    IR_VEC_BIN(VectorSignedSaturatedDoublingMultiplyHigh32, int32_t, (([&]() { int64_t p = static_cast<int64_t>(a) * b * 2; if (a == INT32_MIN && b == INT32_MIN) return static_cast<int32_t>(INT32_MAX); return static_cast<int32_t>(p >> 32); }())))
    IR_VEC_BIN(VectorSignedSaturatedDoublingMultiplyHighRounding16, int16_t, (([&]() { int32_t p = static_cast<int32_t>(a) * b * 2 + 0x8000; if (a == INT16_MIN && b == INT16_MIN) return static_cast<int16_t>(INT16_MAX); return static_cast<int16_t>(p >> 16); }())))
    IR_VEC_BIN(VectorSignedSaturatedDoublingMultiplyHighRounding32, int32_t, (([&]() { int64_t p = static_cast<int64_t>(a) * b * 2 + 0x80000000ll; if (a == INT32_MIN && b == INT32_MIN) return static_cast<int32_t>(INT32_MAX); return static_cast<int32_t>(p >> 32); }())))

    // Signed saturated shift left by register count.
    case Op::VectorSignedSaturatedShiftLeft8:
    case Op::VectorSignedSaturatedShiftLeft16:
    case Op::VectorSignedSaturatedShiftLeft32:
    case Op::VectorSignedSaturatedShiftLeft64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        // The shift-count register holds per-lane signed shift amounts in the
        // low byte of each lane.  Positive = shift left, negative = shift right.
        switch (op) {
        case Op::VectorSignedSaturatedShiftLeft8:
            std::tie(dst.lo, dst.hi) = vec_binop<int8_t>(al, ah, bl, bh, [](int8_t a, int8_t b) { if (b >= 0) return sat_clamp_s<int8_t>(static_cast<int64_t>(a) << (b & 7)); const uint8_t sc = (-b) & 7; return static_cast<int8_t>(a >> sc); });
            break;
        case Op::VectorSignedSaturatedShiftLeft16:
            std::tie(dst.lo, dst.hi) = vec_binop<int16_t>(al, ah, bl, bh, [](int16_t a, int16_t b) { const int8_t s = static_cast<int8_t>(b & 0xFF); if (s >= 0) return sat_clamp_s<int16_t>(static_cast<int64_t>(a) << (s & 15)); const uint8_t sc = (-s) & 15; return static_cast<int16_t>(a >> sc); });
            break;
        case Op::VectorSignedSaturatedShiftLeft32:
            std::tie(dst.lo, dst.hi) = vec_binop<int32_t>(al, ah, bl, bh, [](int32_t a, int32_t b) { const int8_t s = static_cast<int8_t>(b & 0xFF); if (s >= 0) return sat_clamp_s<int32_t>(static_cast<int64_t>(a) << (s & 31)); const uint8_t sc = (-s) & 31; return a >> sc; });
            break;
        case Op::VectorSignedSaturatedShiftLeft64:
            std::tie(dst.lo, dst.hi) = vec_binop<int64_t>(al, ah, bl, bh, [](int64_t a, int64_t b) { const int8_t s = static_cast<int8_t>(b & 0xFF); if (s >= 0) { const uint8_t sc = s & 63; const int64_t r = a << sc; if (sc != 0 && (r >> sc) != a) return a >= 0 ? INT64_MAX : INT64_MIN; return r; } const uint8_t sc = (-s) & 63; return a >> sc; });
            break;
        default: break;
        }
        break;
    }

    // Signed saturated shift left by immediate, with signed->unsigned conversion.
#define IR_VEC_SSLU(OP, T, UT) case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const uint8_t s = read_u8(ctx, inst.GetArg(1)); \
        std::tie(dst.lo, dst.hi) = vec_unop<T>(al, ah, [s](T v) -> T { const int64_t full = static_cast<int64_t>(v) << s; return static_cast<T>(static_cast<UT>(sat_clamp_u<UT>(full))); }); \
        break; }
    IR_VEC_SSLU(VectorSignedSaturatedShiftLeftUnsigned8,  int8_t,  uint8_t)
    IR_VEC_SSLU(VectorSignedSaturatedShiftLeftUnsigned16, int16_t, uint16_t)
    IR_VEC_SSLU(VectorSignedSaturatedShiftLeftUnsigned32, int32_t, uint32_t)
    IR_VEC_SSLU(VectorSignedSaturatedShiftLeftUnsigned64, int64_t, uint64_t)
#undef IR_VEC_SSLU

    // Doubling multiply long: 16/32 -> 32/64 widened.
    case Op::VectorSignedSaturatedDoublingMultiplyLong16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        (void)ah; (void)bh;
        int16_t A[4], B[4]; int32_t R[4];
        std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8);
        for (int i = 0; i < 4; ++i) {
            int32_t p = static_cast<int32_t>(A[i]) * B[i] * 2;
            if (A[i] == INT16_MIN && B[i] == INT16_MIN) p = INT32_MAX;
            R[i] = p;
        }
        std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 2, 8);
        break;
    }
    case Op::VectorSignedSaturatedDoublingMultiplyLong32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        (void)ah; (void)bh;
        int32_t A[2], B[2]; int64_t R[2];
        std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8);
        for (int i = 0; i < 2; ++i) {
            int64_t p = static_cast<int64_t>(A[i]) * B[i] * 2;
            if (A[i] == INT32_MIN && B[i] == INT32_MIN) p = INT64_MAX;
            R[i] = p;
        }
        dst.lo = static_cast<uint64_t>(R[0]); dst.hi = static_cast<uint64_t>(R[1]);
        break;
    }

    // Narrowing saturating conversions (signed -> signed/unsigned).
#define IR_VEC_NARROW(OP, SRC_T, DST_T, CLAMP) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        SRC_T A[2][16 / sizeof(SRC_T)]; \
        uint64_t buf[2] = { al, ah }; \
        std::memcpy(A, buf, 16); \
        DST_T R[16 / sizeof(SRC_T) * 2] = {}; \
        constexpr size_t half = 16 / sizeof(SRC_T); \
        for (size_t i = 0; i < half; ++i) R[i] = static_cast<DST_T>(CLAMP<DST_T>(static_cast<int64_t>(A[0][i]))); \
        uint64_t out[2] = {}; \
        std::memcpy(out, R, half * sizeof(DST_T)); \
        dst.lo = out[0]; dst.hi = out[1]; \
        break; }
    IR_VEC_NARROW(VectorSignedSaturatedNarrowToSigned16,   int16_t, int8_t,  sat_clamp_s)
    IR_VEC_NARROW(VectorSignedSaturatedNarrowToSigned32,   int32_t, int16_t, sat_clamp_s)
    IR_VEC_NARROW(VectorSignedSaturatedNarrowToSigned64,   int64_t, int32_t, sat_clamp_s)
    IR_VEC_NARROW(VectorSignedSaturatedNarrowToUnsigned16, int16_t, uint8_t,  sat_clamp_u)
    IR_VEC_NARROW(VectorSignedSaturatedNarrowToUnsigned32, int32_t, uint16_t, sat_clamp_u)
    IR_VEC_NARROW(VectorSignedSaturatedNarrowToUnsigned64, int64_t, uint32_t, sat_clamp_u)
#undef IR_VEC_NARROW

    // Sign extend: take low 64 bits, widen each lane.
#define IR_VEC_SEXT(OP, SRC_T, DST_T) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        (void)ah; \
        SRC_T A[8 / sizeof(SRC_T)]; \
        std::memcpy(A, &al, 8); \
        DST_T R[16 / sizeof(DST_T)]; \
        for (size_t i = 0; i < 16 / sizeof(DST_T); ++i) R[i] = static_cast<DST_T>(A[i]); \
        std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + (16 / sizeof(DST_T)) / 2, 8); \
        break; }
    IR_VEC_SEXT(VectorSignExtend8,  int8_t,  int16_t)
    IR_VEC_SEXT(VectorSignExtend16, int16_t, int32_t)
    IR_VEC_SEXT(VectorSignExtend32, int32_t, int64_t)
    case Op::VectorSignExtend64: {
        // Sign-extend a 64-bit lane into a 128-bit register.
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        (void)ah;
        dst.lo = al;
        dst.hi = (al & 0x8000000000000000ull) ? UINT64_MAX : 0ull;
        break;
    }
#undef IR_VEC_SEXT

    // Widening multiply: low halves of a × b, sign/unsign-extended into wider lanes.
#define IR_VEC_WMUL(OP, SRC_T, DST_T) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        (void)ah; (void)bh; \
        SRC_T A[8 / sizeof(SRC_T)], B[8 / sizeof(SRC_T)]; \
        std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8); \
        DST_T R[16 / sizeof(DST_T)]; \
        for (size_t i = 0; i < 16 / sizeof(DST_T); ++i) R[i] = static_cast<DST_T>(A[i]) * static_cast<DST_T>(B[i]); \
        std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + (16 / sizeof(DST_T)) / 2, 8); \
        break; }
    IR_VEC_WMUL(VectorMultiplySignedWiden8,    int8_t,  int16_t)
    IR_VEC_WMUL(VectorMultiplySignedWiden16,   int16_t, int32_t)
    IR_VEC_WMUL(VectorMultiplySignedWiden32,   int32_t, int64_t)
    IR_VEC_WMUL(VectorMultiplyUnsignedWiden8,  uint8_t, uint16_t)
    IR_VEC_WMUL(VectorMultiplyUnsignedWiden16, uint16_t, uint32_t)
    IR_VEC_WMUL(VectorMultiplyUnsignedWiden32, uint32_t, uint64_t)
#undef IR_VEC_WMUL

    // ---- Paired add/min/max (combine lanes from a:b into single result) ----
#define IR_VEC_PAIRED(OP, T, COMBINE) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        constexpr size_t N = 16 / sizeof(T); \
        T A[N], B[N], R[N]; \
        uint64_t abuf[2] = { al, ah }, bbuf[2] = { bl, bh }; \
        std::memcpy(A, abuf, 16); std::memcpy(B, bbuf, 16); \
        for (size_t i = 0; i < N / 2; ++i) R[i]         = static_cast<T>(COMBINE(A[2*i], A[2*i+1])); \
        for (size_t i = 0; i < N / 2; ++i) R[N/2 + i]   = static_cast<T>(COMBINE(B[2*i], B[2*i+1])); \
        uint64_t rbuf[2]; std::memcpy(rbuf, R, 16); dst.lo = rbuf[0]; dst.hi = rbuf[1]; \
        break; }
    IR_VEC_PAIRED(VectorPairedAdd8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x + y); })
    IR_VEC_PAIRED(VectorPairedAdd16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x + y); })
    IR_VEC_PAIRED(VectorPairedAdd32, uint32_t, [](uint32_t x, uint32_t y) { return x + y; })
    IR_VEC_PAIRED(VectorPairedAdd64, uint64_t, [](uint64_t x, uint64_t y) { return x + y; })
    IR_VEC_PAIRED(VectorPairedMaxS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t> (x > y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMaxS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>(x > y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMaxS32, int32_t,  [](int32_t  x, int32_t  y) { return x > y ? x : y; })
    IR_VEC_PAIRED(VectorPairedMaxU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x > y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMaxU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x > y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMaxU32, uint32_t, [](uint32_t x, uint32_t y) { return x > y ? x : y; })
    IR_VEC_PAIRED(VectorPairedMinS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t> (x < y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMinS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>(x < y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMinS32, int32_t,  [](int32_t  x, int32_t  y) { return x < y ? x : y; })
    IR_VEC_PAIRED(VectorPairedMinU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x < y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMinU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x < y ? x : y); })
    IR_VEC_PAIRED(VectorPairedMinU32, uint32_t, [](uint32_t x, uint32_t y) { return x < y ? x : y; })

    // ---- Paired 64-bit (lower-only: only half the lanes relevant).
#define IR_VEC_PAIRED_LOWER(OP, T, COMBINE) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        (void)ah; (void)bh; \
        constexpr size_t N = 8 / sizeof(T); \
        T A[N], B[N], R[N * 2] = {}; \
        std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8); \
        for (size_t i = 0; i < N / 2; ++i) R[i]         = static_cast<T>(COMBINE(A[2*i], A[2*i+1])); \
        for (size_t i = 0; i < N / 2; ++i) R[N/2 + i]   = static_cast<T>(COMBINE(B[2*i], B[2*i+1])); \
        uint64_t outlo = 0, outhi = 0; \
        std::memcpy(&outlo, R, N * sizeof(T)); \
        dst.lo = outlo; dst.hi = outhi; \
        break; }
    IR_VEC_PAIRED_LOWER(VectorPairedAddLower8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x + y); })
    IR_VEC_PAIRED_LOWER(VectorPairedAddLower16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x + y); })
    IR_VEC_PAIRED_LOWER(VectorPairedAddLower32, uint32_t, [](uint32_t x, uint32_t y) { return x + y; })
    IR_VEC_PAIRED_LOWER(VectorPairedMaxLowerS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t>(x > y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMaxLowerS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>(x > y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMaxLowerS32, int32_t,  [](int32_t  x, int32_t  y) { return x > y ? x : y; })
    IR_VEC_PAIRED_LOWER(VectorPairedMaxLowerU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x > y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMaxLowerU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x > y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMaxLowerU32, uint32_t, [](uint32_t x, uint32_t y) { return x > y ? x : y; })
    IR_VEC_PAIRED_LOWER(VectorPairedMinLowerS8,  int8_t,   [](int8_t   x, int8_t   y) { return static_cast<int8_t>(x < y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMinLowerS16, int16_t,  [](int16_t  x, int16_t  y) { return static_cast<int16_t>(x < y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMinLowerS32, int32_t,  [](int32_t  x, int32_t  y) { return x < y ? x : y; })
    IR_VEC_PAIRED_LOWER(VectorPairedMinLowerU8,  uint8_t,  [](uint8_t  x, uint8_t  y) { return static_cast<uint8_t>(x < y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMinLowerU16, uint16_t, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x < y ? x : y); })
    IR_VEC_PAIRED_LOWER(VectorPairedMinLowerU32, uint32_t, [](uint32_t x, uint32_t y) { return x < y ? x : y; })
#undef IR_VEC_PAIRED_LOWER

    // Paired add widening (single-operand): sum adjacent pairs, widen result lane.
#define IR_VEC_PAW(OP, SRC_T, DST_T) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        constexpr size_t N = 16 / sizeof(SRC_T); \
        SRC_T A[N]; DST_T R[N / 2]; \
        uint64_t abuf[2] = { al, ah }; \
        std::memcpy(A, abuf, 16); \
        for (size_t i = 0; i < N / 2; ++i) R[i] = static_cast<DST_T>(A[2*i]) + static_cast<DST_T>(A[2*i + 1]); \
        uint64_t rbuf[2]; std::memcpy(rbuf, R, 16); dst.lo = rbuf[0]; dst.hi = rbuf[1]; \
        break; }
    IR_VEC_PAW(VectorPairedAddSignedWiden8,   int8_t,  int16_t)
    IR_VEC_PAW(VectorPairedAddSignedWiden16,  int16_t, int32_t)
    IR_VEC_PAW(VectorPairedAddSignedWiden32,  int32_t, int64_t)
    IR_VEC_PAW(VectorPairedAddUnsignedWiden8,  uint8_t,  uint16_t)
    IR_VEC_PAW(VectorPairedAddUnsignedWiden16, uint16_t, uint32_t)
    IR_VEC_PAW(VectorPairedAddUnsignedWiden32, uint32_t, uint64_t)
#undef IR_VEC_PAW

    // ---- Count leading zeros (per-lane) ----
    IR_VEC_UN(VectorCountLeadingZeros8,  uint8_t,  (v == 0 ? 8u : static_cast<uint8_t>(__builtin_clz(static_cast<uint32_t>(v)) - 24)))
    IR_VEC_UN(VectorCountLeadingZeros16, uint16_t, (v == 0 ? 16u : static_cast<uint16_t>(__builtin_clz(static_cast<uint32_t>(v)) - 16)))
    IR_VEC_UN(VectorCountLeadingZeros32, uint32_t, (v == 0 ? 32u : static_cast<uint32_t>(__builtin_clz(v))))

    // ---- Bit/element reverse ----
    case Op::VectorReverseBits: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        auto bitrev8 = [](uint8_t v) -> uint8_t {
            v = static_cast<uint8_t>(((v >> 1) & 0x55) | ((v & 0x55) << 1));
            v = static_cast<uint8_t>(((v >> 2) & 0x33) | ((v & 0x33) << 2));
            v = static_cast<uint8_t>(((v >> 4) & 0x0F) | ((v & 0x0F) << 4));
            return v;
        };
        std::tie(dst.lo, dst.hi) = vec_unop<uint8_t>(al, ah, bitrev8);
        break;
    }
    // REV16: reverse byte order within each 16-bit lane.
    IR_VEC_UN(VectorReverseElementsInHalfGroups8,  uint16_t, (static_cast<uint16_t>(((v & 0xFF) << 8) | ((v >> 8) & 0xFF))))
    // REV32: reverse elements within each 32-bit group.
    case Op::VectorReverseElementsInWordGroups8: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        auto rev = [](uint32_t v) { return __builtin_bswap32(v); };
        std::tie(dst.lo, dst.hi) = vec_unop<uint32_t>(al, ah, rev);
        break;
    }
    case Op::VectorReverseElementsInWordGroups16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        auto rev = [](uint32_t v) { return static_cast<uint32_t>(((v & 0xFFFFu) << 16) | ((v >> 16) & 0xFFFFu)); };
        std::tie(dst.lo, dst.hi) = vec_unop<uint32_t>(al, ah, rev);
        break;
    }
    // REV64: reverse elements within each 64-bit group.
    case Op::VectorReverseElementsInLongGroups8: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        auto rev = [](uint64_t v) { return __builtin_bswap64(v); };
        std::tie(dst.lo, dst.hi) = vec_unop<uint64_t>(al, ah, rev);
        break;
    }
    case Op::VectorReverseElementsInLongGroups16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        auto rev = [](uint64_t v) {
            return ((v & 0x000000000000FFFFull) << 48) |
                   ((v & 0x00000000FFFF0000ull) << 16) |
                   ((v & 0x0000FFFF00000000ull) >> 16) |
                   ((v & 0xFFFF000000000000ull) >> 48);
        };
        std::tie(dst.lo, dst.hi) = vec_unop<uint64_t>(al, ah, rev);
        break;
    }
    case Op::VectorReverseElementsInLongGroups32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        auto rev = [](uint64_t v) { return (v << 32) | (v >> 32); };
        std::tie(dst.lo, dst.hi) = vec_unop<uint64_t>(al, ah, rev);
        break;
    }

    // ---- Interleave upper / Deinterleave ----
    case Op::VectorInterleaveUpper8:  { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)al; (void)bl; uint8_t A[8], B[8], R[16]; std::memcpy(A, &ah, 8); std::memcpy(B, &bh, 8); for (int i = 0; i < 8; ++i) { R[2*i] = A[i]; R[2*i+1] = B[i]; } std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 8, 8); break; }
    case Op::VectorInterleaveUpper16: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)al; (void)bl; uint16_t A[4], B[4], R[8]; std::memcpy(A, &ah, 8); std::memcpy(B, &bh, 8); for (int i = 0; i < 4; ++i) { R[2*i] = A[i]; R[2*i+1] = B[i]; } std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 4, 8); break; }
    case Op::VectorInterleaveUpper32: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)al; (void)bl; uint32_t A[2], B[2], R[4]; std::memcpy(A, &ah, 8); std::memcpy(B, &bh, 8); R[0] = A[0]; R[1] = B[0]; R[2] = A[1]; R[3] = B[1]; std::memcpy(&dst.lo, R, 8); std::memcpy(&dst.hi, R + 2, 8); break; }
    case Op::VectorInterleaveUpper64: { const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); (void)al; (void)bl; dst.lo = ah; dst.hi = bh; break; }

    // Deinterleave: separate even or odd lanes of (a:b) into one register.
#define IR_VEC_DEINT(OP, T, PICK_ODD) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        constexpr size_t N = 16 / sizeof(T); \
        T A[N], B[N], R[N]; \
        uint64_t abuf[2] = { al, ah }, bbuf[2] = { bl, bh }; \
        std::memcpy(A, abuf, 16); std::memcpy(B, bbuf, 16); \
        const size_t start = (PICK_ODD) ? 1 : 0; \
        for (size_t i = 0; i < N / 2; ++i) R[i]         = A[2*i + start]; \
        for (size_t i = 0; i < N / 2; ++i) R[N / 2 + i] = B[2*i + start]; \
        uint64_t rbuf[2]; std::memcpy(rbuf, R, 16); dst.lo = rbuf[0]; dst.hi = rbuf[1]; \
        break; }
    IR_VEC_DEINT(VectorDeinterleaveEven8,  uint8_t,  false)
    IR_VEC_DEINT(VectorDeinterleaveEven16, uint16_t, false)
    IR_VEC_DEINT(VectorDeinterleaveEven32, uint32_t, false)
    IR_VEC_DEINT(VectorDeinterleaveEven64, uint64_t, false)
    IR_VEC_DEINT(VectorDeinterleaveOdd8,  uint8_t,  true)
    IR_VEC_DEINT(VectorDeinterleaveOdd16, uint16_t, true)
    IR_VEC_DEINT(VectorDeinterleaveOdd32, uint32_t, true)
    IR_VEC_DEINT(VectorDeinterleaveOdd64, uint64_t, true)
#undef IR_VEC_DEINT

#define IR_VEC_DEINT_LO(OP, T, PICK_ODD) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        (void)ah; (void)bh; \
        constexpr size_t N = 8 / sizeof(T); \
        T A[N], B[N], R[N * 2] = {}; \
        std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8); \
        const size_t start = (PICK_ODD) ? 1 : 0; \
        for (size_t i = 0; i < N / 2; ++i) R[i]         = A[2*i + start]; \
        for (size_t i = 0; i < N / 2; ++i) R[N / 2 + i] = B[2*i + start]; \
        uint64_t outlo = 0; std::memcpy(&outlo, R, N * sizeof(T)); \
        dst.lo = outlo; dst.hi = 0; \
        break; }
    IR_VEC_DEINT_LO(VectorDeinterleaveEvenLower8,  uint8_t,  false)
    IR_VEC_DEINT_LO(VectorDeinterleaveEvenLower16, uint16_t, false)
    IR_VEC_DEINT_LO(VectorDeinterleaveEvenLower32, uint32_t, false)
    IR_VEC_DEINT_LO(VectorDeinterleaveOddLower8,  uint8_t,  true)
    IR_VEC_DEINT_LO(VectorDeinterleaveOddLower16, uint16_t, true)
    IR_VEC_DEINT_LO(VectorDeinterleaveOddLower32, uint32_t, true)
#undef IR_VEC_DEINT_LO

    // ======================================================================
    // M12.5 gap-fill: FPVector (per-lane NEON FP)
    //
    // All FPVector ops accept a trailing U1 "fpcr_controlled" arg we ignore.
    // We do everything with native float/double -- accepts default FPSCR
    // semantics (RNE, no FZ); refine if a title trips.
    // ======================================================================
#define IR_FPVEC_BIN32(OP, EXPR) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        float A[4], B[4], R[4]; uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh }; \
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16); \
        for (int i = 0; i < 4; ++i) { const float a = A[i], b = B[i]; R[i] = (EXPR); } \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        break; }
#define IR_FPVEC_BIN64(OP, EXPR) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1)); \
        double A[2], B[2], R[2]; uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh }; \
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16); \
        for (int i = 0; i < 2; ++i) { const double a = A[i], b = B[i]; R[i] = (EXPR); } \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        break; }
#define IR_FPVEC_UN32(OP, EXPR) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        float A[4], R[4]; uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16); \
        for (int i = 0; i < 4; ++i) { const float v = A[i]; R[i] = (EXPR); } \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        break; }
#define IR_FPVEC_UN64(OP, EXPR) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        double A[2], R[2]; uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16); \
        for (int i = 0; i < 2; ++i) { const double v = A[i]; R[i] = (EXPR); } \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        break; }

    IR_FPVEC_BIN32(FPVectorAdd32, a + b)
    IR_FPVEC_BIN64(FPVectorAdd64, a + b)
    IR_FPVEC_BIN32(FPVectorSub32, a - b)
    IR_FPVEC_BIN64(FPVectorSub64, a - b)
    IR_FPVEC_BIN32(FPVectorMul32, a * b)
    IR_FPVEC_BIN64(FPVectorMul64, a * b)
    IR_FPVEC_BIN32(FPVectorDiv32, a / b)
    IR_FPVEC_BIN64(FPVectorDiv64, a / b)
    IR_FPVEC_BIN32(FPVectorMax32, (std::isnan(a) || std::isnan(b) ? std::numeric_limits<float>::quiet_NaN() : std::fmax(a, b)))
    IR_FPVEC_BIN64(FPVectorMax64, (std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::fmax(a, b)))
    IR_FPVEC_BIN32(FPVectorMaxNumeric32, std::fmax(a, b))
    IR_FPVEC_BIN64(FPVectorMaxNumeric64, std::fmax(a, b))
    IR_FPVEC_BIN32(FPVectorMin32, (std::isnan(a) || std::isnan(b) ? std::numeric_limits<float>::quiet_NaN() : std::fmin(a, b)))
    IR_FPVEC_BIN64(FPVectorMin64, (std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : std::fmin(a, b)))
    IR_FPVEC_BIN32(FPVectorMinNumeric32, std::fmin(a, b))
    IR_FPVEC_BIN64(FPVectorMinNumeric64, std::fmin(a, b))
    IR_FPVEC_UN32(FPVectorAbs32, std::fabs(v))
    IR_FPVEC_UN64(FPVectorAbs64, std::fabs(v))
    IR_FPVEC_UN32(FPVectorNeg32, -v)
    IR_FPVEC_UN64(FPVectorNeg64, -v)
    case Op::FPVectorSqrt32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        float A[4], R[4]; uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16);
        for (int i = 0; i < 4; ++i) R[i] = std::sqrt(A[i]);
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorSqrt64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        double A[2], R[2]; uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16);
        for (int i = 0; i < 2; ++i) R[i] = std::sqrt(A[i]);
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorAbs16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        uint16_t A[8]; uint64_t abuf[2] = { al, ah }; std::memcpy(A, abuf, 16);
        for (int i = 0; i < 8; ++i) A[i] &= 0x7FFF;
        uint64_t out[2]; std::memcpy(out, A, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorNeg16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        uint16_t A[8]; uint64_t abuf[2] = { al, ah }; std::memcpy(A, abuf, 16);
        for (int i = 0; i < 8; ++i) A[i] ^= 0x8000;
        uint64_t out[2]; std::memcpy(out, A, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }

    // FP vector compares (per-lane all-ones / all-zeros).
    IR_FPVEC_BIN32(FPVectorEqual32, (a == b) ? std::bit_cast<float>(uint32_t{0xFFFFFFFFu}) : 0.0f)
    IR_FPVEC_BIN64(FPVectorEqual64, (a == b) ? std::bit_cast<double>(uint64_t{~0ull}) : 0.0)
    IR_FPVEC_BIN32(FPVectorGreater32, (a > b) ? std::bit_cast<float>(uint32_t{0xFFFFFFFFu}) : 0.0f)
    IR_FPVEC_BIN64(FPVectorGreater64, (a > b) ? std::bit_cast<double>(uint64_t{~0ull}) : 0.0)
    IR_FPVEC_BIN32(FPVectorGreaterEqual32, (a >= b) ? std::bit_cast<float>(uint32_t{0xFFFFFFFFu}) : 0.0f)
    IR_FPVEC_BIN64(FPVectorGreaterEqual64, (a >= b) ? std::bit_cast<double>(uint64_t{~0ull}) : 0.0)
    case Op::FPVectorEqual16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        uint16_t A[8], B[8], R[8]; uint64_t abuf[2] = { al, ah }, bbuf[2] = { bl, bh };
        std::memcpy(A, abuf, 16); std::memcpy(B, bbuf, 16);
        for (int i = 0; i < 8; ++i) R[i] = (fp16_to_fp32(A[i]) == fp16_to_fp32(B[i])) ? 0xFFFFu : 0;
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    IR_FPVEC_BIN32(FPVectorMulX32,
        (((std::fpclassify(a) == FP_ZERO && std::isinf(b)) || (std::isinf(a) && std::fpclassify(b) == FP_ZERO))
            ? (std::signbit(a) ^ std::signbit(b) ? -2.0f : 2.0f)
            : a * b))
    IR_FPVEC_BIN64(FPVectorMulX64,
        (((std::fpclassify(a) == FP_ZERO && std::isinf(b)) || (std::isinf(a) && std::fpclassify(b) == FP_ZERO))
            ? (std::signbit(a) ^ std::signbit(b) ? -2.0 : 2.0)
            : a * b))

    // FPVector MulAdd (4 U128 args: addend, op1, op2, fpcr): result = addend + op1*op2.
    case Op::FPVectorMulAdd32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        const auto [cl, ch] = read_u128(ctx, inst.GetArg(2));
        float A[4], B[4], C[4], R[4];
        uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh }, cBuf[2] = { cl, ch };
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16); std::memcpy(C, cBuf, 16);
        for (int i = 0; i < 4; ++i) R[i] = std::fma(B[i], C[i], A[i]);
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorMulAdd64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        const auto [cl, ch] = read_u128(ctx, inst.GetArg(2));
        double A[2], B[2], C[2], R[2];
        uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh }, cBuf[2] = { cl, ch };
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16); std::memcpy(C, cBuf, 16);
        for (int i = 0; i < 2; ++i) R[i] = std::fma(B[i], C[i], A[i]);
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorMulAdd16: {
        // 8-lane FP16 fused multiply-add: addend + op1*op2.
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        const auto [cl, ch] = read_u128(ctx, inst.GetArg(2));
        uint16_t A[8], B[8], C[8], R[8];
        uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh }, cBuf[2] = { cl, ch };
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16); std::memcpy(C, cBuf, 16);
        for (int i = 0; i < 8; ++i) R[i] = fp32_to_fp16(std::fma(fp16_to_fp32(B[i]), fp16_to_fp32(C[i]), fp16_to_fp32(A[i])));
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }

    // ---- FPVector paired add ----
    case Op::FPVectorPairedAdd32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        float A[4], B[4], R[4]; uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh };
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16);
        R[0] = A[0] + A[1]; R[1] = A[2] + A[3];
        R[2] = B[0] + B[1]; R[3] = B[2] + B[3];
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorPairedAdd64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        double A[2], B[2], R[2]; uint64_t aBuf[2] = { al, ah }, bBuf[2] = { bl, bh };
        std::memcpy(A, aBuf, 16); std::memcpy(B, bBuf, 16);
        R[0] = A[0] + A[1]; R[1] = B[0] + B[1];
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorPairedAddLower32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        (void)ah; (void)bh;
        float A[2], B[2], R[4] = {};
        std::memcpy(A, &al, 8); std::memcpy(B, &bl, 8);
        R[0] = A[0] + A[1]; R[1] = B[0] + B[1];
        uint64_t out[2] = {}; std::memcpy(out, R, 8);
        dst.lo = out[0]; dst.hi = 0;
        break;
    }
    case Op::FPVectorPairedAddLower64: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const auto [bl, bh] = read_u128(ctx, inst.GetArg(1));
        (void)ah; (void)bh;
        // Only 1 pair in each 64-bit half -> lower 64 bits of result.
        dst.lo = to_u64(as_f64(al) + as_f64(bl));
        dst.hi = 0;
        break;
    }

    // ---- FPVector conversions (half <-> float, fixed <-> float) ----
    case Op::FPVectorFromHalf32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        (void)ah;
        uint16_t A[4]; std::memcpy(A, &al, 8);
        float R[4]; for (int i = 0; i < 4; ++i) R[i] = fp16_to_fp32(A[i]);
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorToHalf32: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        float A[4]; uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16);
        uint16_t R[4]; for (int i = 0; i < 4; ++i) R[i] = fp32_to_fp16(A[i]);
        uint64_t out = 0; std::memcpy(&out, R, 8); dst.lo = out; dst.hi = 0;
        break;
    }
#define IR_FPVEC_FROM_FIXED(OP, FP_T, FX_T, BIT_T) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1)); \
        constexpr size_t N = 16 / sizeof(FX_T); \
        FX_T A[N]; FP_T R[N]; \
        uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16); \
        for (size_t i = 0; i < N; ++i) R[i] = static_cast<FP_T>(A[i]) / static_cast<FP_T>(1ull << fbits); \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        (void)(BIT_T{}); break; }
    IR_FPVEC_FROM_FIXED(FPVectorFromSignedFixed32,   float,  int32_t,  uint32_t)
    IR_FPVEC_FROM_FIXED(FPVectorFromSignedFixed64,   double, int64_t,  uint64_t)
    IR_FPVEC_FROM_FIXED(FPVectorFromUnsignedFixed32, float,  uint32_t, uint32_t)
    IR_FPVEC_FROM_FIXED(FPVectorFromUnsignedFixed64, double, uint64_t, uint64_t)
#undef IR_FPVEC_FROM_FIXED

#define IR_FPVEC_TO_FIXED(OP, FP_T, OUT_T, CLAMP) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1)); \
        constexpr size_t N = 16 / sizeof(OUT_T); \
        FP_T A[N]; OUT_T R[N]; \
        uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16); \
        for (size_t i = 0; i < N; ++i) { \
            const FP_T scaled = A[i] * static_cast<FP_T>(1ull << fbits); \
            if (std::isnan(scaled)) R[i] = 0; \
            else R[i] = static_cast<OUT_T>(CLAMP<OUT_T>(static_cast<int64_t>(std::trunc(scaled)))); \
        } \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        break; }
    IR_FPVEC_TO_FIXED(FPVectorToSignedFixed32,   float,  int32_t,  sat_clamp_s)
    IR_FPVEC_TO_FIXED(FPVectorToSignedFixed64,   double, int64_t,  sat_clamp_s)
    IR_FPVEC_TO_FIXED(FPVectorToUnsignedFixed32, float,  uint32_t, sat_clamp_u)
    IR_FPVEC_TO_FIXED(FPVectorToUnsignedFixed64, double, uint64_t, sat_clamp_u)
    // Halved 16-bit variants: input is U128 of 8 half-floats, output is U128
    // of 8 half-width fixed.
    case Op::FPVectorToSignedFixed16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        uint16_t A[8]; int16_t R[8];
        uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16);
        for (int i = 0; i < 8; ++i) {
            const float scaled = fp16_to_fp32(A[i]) * static_cast<float>(1ull << fbits);
            R[i] = std::isnan(scaled) ? 0 : sat_clamp_s<int16_t>(static_cast<int64_t>(std::trunc(scaled)));
        }
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
    case Op::FPVectorToUnsignedFixed16: {
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        uint16_t A[8], R[8];
        uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16);
        for (int i = 0; i < 8; ++i) {
            const float scaled = fp16_to_fp32(A[i]) * static_cast<float>(1ull << fbits);
            R[i] = std::isnan(scaled) ? 0 : sat_clamp_u<uint16_t>(static_cast<int64_t>(std::trunc(scaled)));
        }
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1];
        break;
    }
#undef IR_FPVEC_TO_FIXED

    // ---- FPVector RoundInt ----
#define IR_FPVEC_ROUND(OP, FP_T) \
    case Op::OP: { \
        const auto [al, ah] = read_u128(ctx, inst.GetArg(0)); \
        const uint8_t rmode = read_u8(ctx, inst.GetArg(1)); \
        constexpr size_t N = 16 / sizeof(FP_T); \
        FP_T A[N], R[N]; uint64_t aBuf[2] = { al, ah }; std::memcpy(A, aBuf, 16); \
        for (size_t i = 0; i < N; ++i) { \
            switch (rmode) { \
            case 0: R[i] = std::nearbyint(A[i]); break; \
            case 1: R[i] = std::ceil(A[i]); break; \
            case 2: R[i] = std::floor(A[i]); break; \
            case 3: R[i] = std::trunc(A[i]); break; \
            default: R[i] = std::nearbyint(A[i]); break; \
            } \
        } \
        uint64_t out[2]; std::memcpy(out, R, 16); dst.lo = out[0]; dst.hi = out[1]; \
        break; }
    IR_FPVEC_ROUND(FPVectorRoundInt32, float)
    IR_FPVEC_ROUND(FPVectorRoundInt64, double)
#undef IR_FPVEC_ROUND

    // ---- FPVector Recip/RSqrt Estimate + StepFused ----
    IR_FPVEC_UN32(FPVectorRecipEstimate32, (v == 0.0f ? std::numeric_limits<float>::infinity() : 1.0f / v))
    IR_FPVEC_UN64(FPVectorRecipEstimate64, (v == 0.0  ? std::numeric_limits<double>::infinity() : 1.0 / v))
    IR_FPVEC_UN32(FPVectorRSqrtEstimate32, (v <= 0.0f ? std::numeric_limits<float>::quiet_NaN() : 1.0f / std::sqrt(v)))
    IR_FPVEC_UN64(FPVectorRSqrtEstimate64, (v <= 0.0  ? std::numeric_limits<double>::quiet_NaN() : 1.0 / std::sqrt(v)))
    IR_FPVEC_BIN32(FPVectorRecipStepFused32, 2.0f - a * b)
    IR_FPVEC_BIN64(FPVectorRecipStepFused64, 2.0 - a * b)
    IR_FPVEC_BIN32(FPVectorRSqrtStepFused32, (3.0f - a * b) * 0.5f)
    IR_FPVEC_BIN64(FPVectorRSqrtStepFused64, (3.0 - a * b) * 0.5)

    // ---- FP half-precision + remaining recip/rsqrt stubs (scalar) ----
    case Op::FPHalfToSingle: {
        const uint16_t v = read_u16(ctx, inst.GetArg(0));
        dst.lo = to_u32(fp16_to_fp32(v));
        break;
    }
    case Op::FPHalfToDouble: {
        const uint16_t v = read_u16(ctx, inst.GetArg(0));
        dst.lo = to_u64(static_cast<double>(fp16_to_fp32(v)));
        break;
    }
    case Op::FPSingleToHalf: {
        const float f = as_f32(read_u32(ctx, inst.GetArg(0)));
        dst.lo = fp32_to_fp16(f);
        break;
    }
    case Op::FPDoubleToHalf: {
        const double f = as_f64(read_u64(ctx, inst.GetArg(0)));
        dst.lo = fp32_to_fp16(static_cast<float>(f));
        break;
    }
    case Op::FPHalfToFixedS32: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const float scaled = v * static_cast<float>(1ull << fbits);
        dst.lo = std::isnan(scaled) ? 0u : static_cast<uint32_t>(sat_clamp_s<int32_t>(static_cast<int64_t>(std::trunc(scaled))));
        break;
    }
    case Op::FPHalfToFixedU32: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const float scaled = v * static_cast<float>(1ull << fbits);
        dst.lo = std::isnan(scaled) ? 0u : sat_clamp_u<uint32_t>(static_cast<int64_t>(std::trunc(scaled)));
        break;
    }
    case Op::FPHalfToFixedS16: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const float scaled = v * static_cast<float>(1ull << fbits);
        dst.lo = std::isnan(scaled) ? 0u : static_cast<uint16_t>(sat_clamp_s<int16_t>(static_cast<int64_t>(std::trunc(scaled))));
        break;
    }
    case Op::FPHalfToFixedU16: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const float scaled = v * static_cast<float>(1ull << fbits);
        dst.lo = std::isnan(scaled) ? 0u : sat_clamp_u<uint16_t>(static_cast<int64_t>(std::trunc(scaled)));
        break;
    }
    case Op::FPHalfToFixedS64: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = static_cast<double>(v) * static_cast<double>(1ull << fbits);
        if (std::isnan(scaled)) { dst.lo = 0; break; }
        if (scaled >= 9223372036854775808.0) { dst.lo = static_cast<uint64_t>(INT64_MAX); break; }
        if (scaled < -9223372036854775808.0) { dst.lo = static_cast<uint64_t>(INT64_MIN); break; }
        dst.lo = static_cast<uint64_t>(static_cast<int64_t>(std::trunc(scaled)));
        break;
    }
    case Op::FPHalfToFixedU64: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t fbits = read_u8(ctx, inst.GetArg(1));
        const double scaled = static_cast<double>(v) * static_cast<double>(1ull << fbits);
        if (std::isnan(scaled) || scaled <= 0.0) { dst.lo = 0; break; }
        if (scaled >= 18446744073709551616.0) { dst.lo = UINT64_MAX; break; }
        dst.lo = static_cast<uint64_t>(std::trunc(scaled));
        break;
    }

    // Scalar FP Recip/RSqrt Estimate + StepFused + RecipExponent + RoundInt16.
    case Op::FPRoundInt16: {
        const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0)));
        const uint8_t rmode = read_u8(ctx, inst.GetArg(1));
        float r;
        switch (rmode) {
        case 0: r = std::nearbyint(v); break;
        case 1: r = std::ceil(v); break;
        case 2: r = std::floor(v); break;
        case 3: r = std::trunc(v); break;
        default: r = std::nearbyint(v); break;
        }
        dst.lo = fp32_to_fp16(r);
        break;
    }
    case Op::FPRecipEstimate16: { const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0))); dst.lo = fp32_to_fp16(v == 0.0f ? std::numeric_limits<float>::infinity() : 1.0f / v); break; }
    case Op::FPRecipEstimate32: { const float v = as_f32(read_u32(ctx, inst.GetArg(0))); dst.lo = to_u32(v == 0.0f ? std::numeric_limits<float>::infinity() : 1.0f / v); break; }
    case Op::FPRecipEstimate64: { const double v = as_f64(read_u64(ctx, inst.GetArg(0))); dst.lo = to_u64(v == 0.0  ? std::numeric_limits<double>::infinity() : 1.0 / v); break; }
    case Op::FPRSqrtEstimate16: { const float v = fp16_to_fp32(read_u16(ctx, inst.GetArg(0))); dst.lo = fp32_to_fp16(v <= 0.0f ? std::numeric_limits<float>::quiet_NaN() : 1.0f / std::sqrt(v)); break; }
    case Op::FPRSqrtEstimate32: { const float v = as_f32(read_u32(ctx, inst.GetArg(0))); dst.lo = to_u32(v <= 0.0f ? std::numeric_limits<float>::quiet_NaN() : 1.0f / std::sqrt(v)); break; }
    case Op::FPRSqrtEstimate64: { const double v = as_f64(read_u64(ctx, inst.GetArg(0))); dst.lo = to_u64(v <= 0.0  ? std::numeric_limits<double>::quiet_NaN() : 1.0 / std::sqrt(v)); break; }
    case Op::FPRecipStepFused16: { const float a = fp16_to_fp32(read_u16(ctx, inst.GetArg(0))); const float b = fp16_to_fp32(read_u16(ctx, inst.GetArg(1))); dst.lo = fp32_to_fp16(2.0f - a * b); break; }
    case Op::FPRecipStepFused32: { const float a = as_f32(read_u32(ctx, inst.GetArg(0))); const float b = as_f32(read_u32(ctx, inst.GetArg(1))); dst.lo = to_u32(2.0f - a * b); break; }
    case Op::FPRecipStepFused64: { const double a = as_f64(read_u64(ctx, inst.GetArg(0))); const double b = as_f64(read_u64(ctx, inst.GetArg(1))); dst.lo = to_u64(2.0 - a * b); break; }
    case Op::FPRSqrtStepFused16: { const float a = fp16_to_fp32(read_u16(ctx, inst.GetArg(0))); const float b = fp16_to_fp32(read_u16(ctx, inst.GetArg(1))); dst.lo = fp32_to_fp16((3.0f - a * b) * 0.5f); break; }
    case Op::FPRSqrtStepFused32: { const float a = as_f32(read_u32(ctx, inst.GetArg(0))); const float b = as_f32(read_u32(ctx, inst.GetArg(1))); dst.lo = to_u32((3.0f - a * b) * 0.5f); break; }
    case Op::FPRSqrtStepFused64: { const double a = as_f64(read_u64(ctx, inst.GetArg(0))); const double b = as_f64(read_u64(ctx, inst.GetArg(1))); dst.lo = to_u64((3.0 - a * b) * 0.5); break; }
    case Op::FPRecipExponent16: {
        // Returns 2^(-floor(log2(|v|))). Implement by flipping the exponent field.
        const uint16_t v = read_u16(ctx, inst.GetArg(0));
        const uint16_t exp = (v >> 10) & 0x1F;
        const uint16_t sign = v & 0x8000;
        // Map finite: newexp = 30 - exp (for normals); subnormals/nan pass through.
        uint16_t out;
        if (exp == 0 || exp == 0x1F) out = v;
        else out = static_cast<uint16_t>(sign | ((30u - exp) << 10));
        dst.lo = out;
        break;
    }
    case Op::FPRecipExponent32: {
        const uint32_t v = read_u32(ctx, inst.GetArg(0));
        const uint32_t exp = (v >> 23) & 0xFF;
        const uint32_t sign = v & 0x80000000u;
        uint32_t out;
        if (exp == 0 || exp == 0xFF) out = v;
        else out = sign | ((253u - exp) << 23);
        dst.lo = out;
        break;
    }
    case Op::FPRecipExponent64: {
        const uint64_t v = read_u64(ctx, inst.GetArg(0));
        const uint64_t exp = (v >> 52) & 0x7FF;
        const uint64_t sign = v & 0x8000000000000000ull;
        uint64_t out;
        if (exp == 0 || exp == 0x7FF) out = v;
        else out = sign | ((2045ull - exp) << 52);
        dst.lo = out;
        break;
    }

#undef IR_FPVEC_BIN32
#undef IR_FPVEC_BIN64
#undef IR_FPVEC_UN32
#undef IR_FPVEC_UN64
#undef IR_VEC_BIN
#undef IR_VEC_UN
#undef IR_VEC_PAIRED

    default:
        interp_halt_unimplemented(cpu, op, block_pc);
        return false;
    }

    return true;
}

}  // namespace

// Terminal visitor for the FP_COMPLEX fallback path.  Declared here at file
// scope (instead of nested inside run_block) so multiple call sites can share
// the implementation.
namespace {
struct TerminalVisitor : public boost::static_visitor<void> {
    IRInterpreterCPU *self;
    explicit TerminalVisitor(IRInterpreterCPU *s) : self(s) {}

    void operator()(const Term::Invalid &) const {
        LOG_ERROR("[IR-interp] Block produced Terminal::Invalid -- halting");
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::halt_requested; };
        static_cast<Friend *>(self)->halt_requested.store(true, std::memory_order_release);
    }
    void operator()(const Term::Interpret &term) const {
        A32::LocationDescriptor next(term.next);
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::regs; using IRInterpreterCPU::cpsr; using IRInterpreterCPU::halt_requested; };
        auto *f = static_cast<Friend *>(self);
        f->regs[15] = next.PC();
        if (next.TFlag()) f->cpsr |= 0x20u; else f->cpsr &= ~0x20u;
        LOG_WARN("[IR-interp] Term::Interpret reached (hook_hint_instructions should suppress these); "
                 "halting at pc=0x{:x}", f->regs[15]);
        f->halt_requested.store(true, std::memory_order_release);
    }
    void operator()(const Term::ReturnToDispatch &) const {
        // regs[15] already carries the next-PC.
    }
    void operator()(const Term::LinkBlock &term) const {
        A32::LocationDescriptor next(term.next);
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::regs; using IRInterpreterCPU::cpsr; };
        auto *f = static_cast<Friend *>(self);
        f->regs[15] = next.PC();
        if (next.TFlag()) f->cpsr |= 0x20u; else f->cpsr &= ~0x20u;
    }
    void operator()(const Term::LinkBlockFast &term) const {
        A32::LocationDescriptor next(term.next);
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::regs; using IRInterpreterCPU::cpsr; };
        auto *f = static_cast<Friend *>(self);
        f->regs[15] = next.PC();
        if (next.TFlag()) f->cpsr |= 0x20u; else f->cpsr &= ~0x20u;
    }
    void operator()(const Term::PopRSBHint &) const {}
    void operator()(const Term::FastDispatchHint &) const {}
    void operator()(const Term::If &term) const {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::cpsr; };
        const uint32_t cpsr_v = static_cast<Friend *>(self)->cpsr;
        const bool pass = evaluate_cond(term.if_, cpsr_v);
        boost::apply_visitor(TerminalVisitor{self}, pass ? term.then_ : term.else_);
    }
    void operator()(const Term::CheckBit &term) const {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::check_bit; };
        const bool bit = static_cast<Friend *>(self)->check_bit;
        boost::apply_visitor(TerminalVisitor{self}, bit ? term.then_ : term.else_);
    }
    void operator()(const Term::CheckHalt &term) const {
        struct Friend : public IRInterpreterCPU { using IRInterpreterCPU::halt_requested; };
        if (static_cast<Friend *>(self)->halt_requested.load(std::memory_order_acquire)) return;
        boost::apply_visitor(TerminalVisitor{self}, term.else_);
    }
};
} // namespace

// Execute the body of a block (condition check + every IR::Inst) but do NOT
// dispatch the terminal.  Returns 0 on normal completion, 1 if execution was
// halted mid-block (so the outer loop can bail without running the terminal).
int IRInterpreterCPU::run_block_body(CachedBlock &cblk) {
    IR::Block &block = *cblk.ir;

    // Block-entry condition: fast path for AL (~95% of blocks).
    if (!cblk.cond_al) {
        if (!evaluate_cond(cblk.cond, cpsr)) {
            if (cblk.cond_fail_valid) {
                regs[15] = cblk.cond_fail_pc;
                if (cblk.cond_fail_thumb) cpsr |= 0x20u; else cpsr &= ~0x20u;
            }
            return 1;  // Tells the outer loop to skip terminal dispatch.
        }
    }

    // Reuse the CPU-owned SSA value vector -- avoids per-block allocation +
    // zero-init.  resize() grows only when needed; shrinks are rare (the
    // vector settles at max block size seen).  Each inst writes its dst
    // before any later inst reads it, so we don't need to pre-zero.
    EvalCtx ctx;
    ctx.cpu = this;
    std::swap(ctx.values, reusable_values);
    const std::size_t needed = block.size() + 8;
    if (ctx.values.size() < needed) ctx.values.resize(needed);

    uint32_t check_bit_local = check_bit ? 1u : 0u;
    bool halted_here = false;
    const uint32_t block_pc = A32::LocationDescriptor(block.Location()).PC();
    for (auto &inst : block) {
        if (!evaluate_inst(ctx, inst, this, block_pc,
                           cb.get(), cp15.get(), monitor, core_id,
                           check_bit_local)) {
            halted_here = true;
            break;
        }
    }
    check_bit = check_bit_local != 0;

    std::swap(ctx.values, reusable_values);
    return halted_here ? 1 : 0;
}

// --------------------------------------------------------------------------
// CPUInterface surface
// --------------------------------------------------------------------------

int IRInterpreterCPU::run() {
    halted = false;
    break_ = false;
    halt_requested.store(false, std::memory_order_release);
    parent->svc_called = false;
    // M12.7.3: reset the per-run log budget but *keep* the consecutive-fault
    // streak.  If guest code is in a runaway loop the streak will already be
    // approaching kRunawayMemFaultLimit from the previous run() slice, and we
    // want the next invalid access to finish tripping the halt.  Resetting
    // the log budget gives the developer fresh context on each frame.
    mem_fault_log_budget = kMemFaultLogCap;

    // `next_hint` carries a direct CachedBlock* from the previous iteration's
    // terminal resolution, letting us skip both L0 and the hash map when we
    // follow a statically-known block chain (the common case in tight inner
    // loops).  Stale-safe because invalidate_jit_cache() sets halt_requested
    // before clearing the cache, which forces us to exit this loop.
    CachedBlock *next_hint = nullptr;
    while (!halt_requested.load(std::memory_order_acquire)) {
        CachedBlock *cblk;
        if (next_hint) {
            cblk = next_hint;
            next_hint = nullptr;
        } else {
            const uint32_t pc = regs[15];
            const bool thumb = (cpsr & 0x20u) != 0;
            cblk = get_or_translate_block_cached(pc, thumb);
            if (!cblk) {
                LOG_ERROR("[IR-interp] translate failed at pc=0x{:x}", pc);
                halted = true;
                break;
            }
        }

        if (run_block_body(*cblk) != 0) continue; // halted or cond-failed

        // Pre-analyzed terminal dispatch.  In decreasing order of frequency
        // on Vita workloads:
        //   1. FP_LINK_HALT (typical sequential block):
        //      Load cached next pointer, check halt flag, set regs[15]/T.
        //   2. FP_LINK: same, skip halt check.
        //   3. FP_COND_LINK (M12.7): conditional branch with both arms
        //      resolvable -- evaluate_cond once, dispatch directly.
        //   4. FP_RETURN_DISPATCH: block already wrote regs[15].
        //   5. FP_COMPLEX: fall back to visitor.
        switch (cblk->fast_path) {
        case CachedBlock::FP_LINK_HALT:
            if (halt_requested.load(std::memory_order_acquire)) goto done;
            [[fallthrough]];
        case CachedBlock::FP_LINK: {
            regs[15] = cblk->link_pc;
            if (cblk->link_thumb) cpsr |= 0x20u; else cpsr &= ~0x20u;
            if (!cblk->link_cached) {
                cblk->link_cached = lookup_cached_block(cblk->link_key);
            }
            next_hint = cblk->link_cached;  // may be nullptr -> next iter misses & translates
            break;
        }
        case CachedBlock::FP_COND_LINK: {
            const bool pass = cblk->term_cond_al
                              || evaluate_cond(cblk->term_cond, cpsr);
            if (pass) {
                if (cblk->term_halt_check
                    && halt_requested.load(std::memory_order_acquire)) goto done;
                regs[15] = cblk->link_pc;
                if (cblk->link_thumb) cpsr |= 0x20u; else cpsr &= ~0x20u;
                if (!cblk->link_cached) {
                    cblk->link_cached = lookup_cached_block(cblk->link_key);
                }
                next_hint = cblk->link_cached;
            } else {
                if (cblk->term_halt_check_alt
                    && halt_requested.load(std::memory_order_acquire)) goto done;
                regs[15] = cblk->link_pc_alt;
                if (cblk->link_thumb_alt) cpsr |= 0x20u; else cpsr &= ~0x20u;
                if (!cblk->link_cached_alt) {
                    cblk->link_cached_alt = lookup_cached_block(cblk->link_key_alt);
                }
                next_hint = cblk->link_cached_alt;
            }
            break;
        }
        case CachedBlock::FP_RETURN_DISPATCH:
            // regs[15] / T already set by SetRegister(15,..) / BXWritePC
            // during the block body.  Cold L0 lookup next iter.
            break;
        case CachedBlock::FP_COMPLEX:
            boost::apply_visitor(TerminalVisitor{this}, cblk->terminal);
            break;
        }
    }
done:
    return halted ? 1 : 0;
}

int IRInterpreterCPU::step() {
    halt_requested.store(false, std::memory_order_release);
    parent->svc_called = false;
    mem_fault_log_budget = kMemFaultLogCap;
    const uint32_t pc = regs[15];
    const bool thumb = (cpsr & 0x20u) != 0;

    // Single-stepping path: translate with the single-stepping bit in the
    // location descriptor so that the IR is limited to one guest instruction.
    Dynarmic::A32::PSR psr;
    psr.T(thumb);
    Dynarmic::A32::FPSCR fpr(fpscr);
    A32::LocationDescriptor loc(pc, psr, fpr, /*single_stepping=*/true);
    A32::TranslationOptions opts;
    opts.arch_version = A32::ArchVersion::v7;
    opts.define_unpredictable_behaviour = false;
    opts.hook_hint_instructions = true;

    IR::Block translated = A32::Translate(loc, cb.get(), opts);
    Optimization::PolyfillPass(translated, {});
    Optimization::DeadCodeElimination(translated);
    Optimization::IdentityRemovalPass(translated);
    Optimization::NamingPass(translated);

    // Stepping doesn't cache -- wrap a throw-away CachedBlock so the shared
    // run_block_body + terminal dispatch can run.
    CachedBlock scratch;
    scratch.ir.reset(new IR::Block(std::move(translated)));
    const IR::Cond block_cond = scratch.ir->GetCondition();
    scratch.cond = block_cond;
    scratch.cond_al = (block_cond == IR::Cond::AL);
    if (scratch.ir->HasConditionFailedLocation()) {
        A32::LocationDescriptor fail(scratch.ir->ConditionFailedLocation());
        scratch.cond_fail_valid = true;
        scratch.cond_fail_pc = fail.PC();
        scratch.cond_fail_thumb = fail.TFlag();
    }
    scratch.terminal = scratch.ir->GetTerminal();
    analyze_terminal(scratch, scratch.terminal, false);

    if (run_block_body(scratch) != 0) return 0;

    switch (scratch.fast_path) {
    case CachedBlock::FP_LINK_HALT:
        if (halt_requested.load(std::memory_order_acquire)) break;
        [[fallthrough]];
    case CachedBlock::FP_LINK:
        regs[15] = scratch.link_pc;
        if (scratch.link_thumb) cpsr |= 0x20u; else cpsr &= ~0x20u;
        break;
    case CachedBlock::FP_COND_LINK: {
        const bool pass = scratch.term_cond_al
                          || evaluate_cond(scratch.term_cond, cpsr);
        if (pass) {
            if (scratch.term_halt_check
                && halt_requested.load(std::memory_order_acquire)) break;
            regs[15] = scratch.link_pc;
            if (scratch.link_thumb) cpsr |= 0x20u; else cpsr &= ~0x20u;
        } else {
            if (scratch.term_halt_check_alt
                && halt_requested.load(std::memory_order_acquire)) break;
            regs[15] = scratch.link_pc_alt;
            if (scratch.link_thumb_alt) cpsr |= 0x20u; else cpsr &= ~0x20u;
        }
        break;
    }
    case CachedBlock::FP_RETURN_DISPATCH:
        break;
    case CachedBlock::FP_COMPLEX:
        boost::apply_visitor(TerminalVisitor{this}, scratch.terminal);
        break;
    }
    return 0;
}

void IRInterpreterCPU::stop() {
    halt_requested.store(true, std::memory_order_release);
}

uint32_t IRInterpreterCPU::get_reg(uint8_t idx) { return regs[idx]; }
void IRInterpreterCPU::set_reg(uint8_t idx, uint32_t v) { regs[idx] = v; }
uint32_t IRInterpreterCPU::get_sp() { return regs[13]; }
void IRInterpreterCPU::set_sp(uint32_t v) { regs[13] = v; }
uint32_t IRInterpreterCPU::get_pc() { return regs[15]; }
uint32_t IRInterpreterCPU::get_lr() { return regs[14]; }
void IRInterpreterCPU::set_lr(uint32_t v) { regs[14] = v; }
uint32_t IRInterpreterCPU::get_cpsr() { return cpsr; }
void IRInterpreterCPU::set_cpsr(uint32_t v) { cpsr = v; }
uint32_t IRInterpreterCPU::get_tpidruro() { return cp15->get_tpidruro(); }
void IRInterpreterCPU::set_tpidruro(uint32_t v) { cp15->set_tpidruro(v); }

void IRInterpreterCPU::set_pc(uint32_t v) {
    if (v & 1) {
        cpsr |= 0x20u;
        regs[15] = v & ~1u;
    } else {
        cpsr &= ~0x20u;
        regs[15] = v & ~3u;
    }
}

float IRInterpreterCPU::get_float_reg(uint8_t idx) {
    return std::bit_cast<float>(ext_regs[idx]);
}
void IRInterpreterCPU::set_float_reg(uint8_t idx, float v) {
    ext_regs[idx] = std::bit_cast<uint32_t>(v);
}
uint32_t IRInterpreterCPU::get_fpscr() { return fpscr; }
void IRInterpreterCPU::set_fpscr(uint32_t v) { fpscr = v; }

CPUContext IRInterpreterCPU::save_context() {
    CPUContext ctx;
    ctx.cpu_registers = regs;
    static_assert(sizeof(ctx.fpu_registers) == sizeof(ext_regs));
    std::memcpy(ctx.fpu_registers.data(), ext_regs.data(), sizeof(ext_regs));
    ctx.cpsr = cpsr;
    ctx.fpscr = fpscr;
    return ctx;
}
void IRInterpreterCPU::load_context(const CPUContext &ctx) {
    regs = ctx.cpu_registers;
    std::memcpy(ext_regs.data(), ctx.fpu_registers.data(), sizeof(ext_regs));
    cpsr = ctx.cpsr;
    fpscr = ctx.fpscr;
}

void IRInterpreterCPU::invalidate_jit_cache(Address /*start*/, size_t /*length*/) {
    // Simplest correct implementation: flush the entire block cache. A more
    // targeted version would intersect [start, start+length) with cached
    // block ranges, but correctness first -- M12.5 can tighten this.
    //
    // Must also clear L0 (otherwise we'd return dangling pointers) and zero
    // out any direct link pointers in *remaining* CachedBlocks -- but since
    // we clear the map entirely in a single shot, link_cached dangling is a
    // non-issue: no CachedBlock survives the clear.  L0 must still be reset
    // so the next lookup doesn't hit a stale slot pointing into freed memory.
    l0_cache.fill({ 0, nullptr });
    block_cache.clear();
}

bool IRInterpreterCPU::is_thumb_mode() { return (cpsr & 0x20u) != 0; }

bool IRInterpreterCPU::hit_breakpoint() { return break_; }
void IRInterpreterCPU::trigger_breakpoint() { break_ = true; stop(); }
void IRInterpreterCPU::set_log_code(bool v) { log_code = v; }
void IRInterpreterCPU::set_log_mem(bool v) { log_mem = v; }
bool IRInterpreterCPU::get_log_code() { return log_code; }
bool IRInterpreterCPU::get_log_mem() { return log_mem; }

std::size_t IRInterpreterCPU::processor_id() const { return core_id; }
