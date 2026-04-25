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

#include <mem/functions.h>
#include <mem/state.h>

#include <util/align.h>
#include <util/log.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(LIBRETRO) && defined(__APPLE__)
#include <TargetConditionals.h>
#endif

constexpr uint32_t STANDARD_PAGE_SIZE = KiB(4);
constexpr size_t TOTAL_MEM_SIZE = GiB(4);
constexpr bool LOG_PROTECT = false;
#ifdef NDEBUG
constexpr bool PAGE_NAME_TRACKING = false;
#else
constexpr bool PAGE_NAME_TRACKING = true;
#endif

// TODO: support multiple handlers
static AccessViolationHandler access_violation_handler;
static void register_access_violation_handler(const AccessViolationHandler &handler);

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force);
static void delete_memory(uint8_t *memory);

#ifdef _WIN32
static std::string get_error_msg() {
    return std::system_category().message(GetLastError());
}
#else
static std::string get_error_msg() {
    return strerror(errno);
}
#endif

#ifdef LIBRETRO
static void commit_guest_rw(MemState &state, Address guest_start, uint32_t guest_bytes) {
    if (guest_bytes == 0)
        return;
#ifdef _WIN32
    uint8_t *const p = &state.memory[guest_start];
    const void *const ret = VirtualAlloc(p, guest_bytes, MEM_COMMIT, PAGE_READWRITE);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
    if (state.host_page_size > state.page_size) {
        const Address host_start = align_down(guest_start, static_cast<uint64_t>(state.host_page_size));
        const Address host_end = align(guest_start + guest_bytes, static_cast<uint64_t>(state.host_page_size));
        uint8_t *const p = &state.memory[host_start];
        const size_t len = host_end - host_start;
        const int ret = mprotect(p, len, PROT_READ | PROT_WRITE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
        return;
    }
    uint8_t *const p = &state.memory[guest_start];
    const int ret = mprotect(p, guest_bytes, PROT_READ | PROT_WRITE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

static void decommit_guest_none(MemState &state, Address guest_start, uint32_t guest_bytes) {
    if (guest_bytes == 0)
        return;
#ifdef _WIN32
    uint8_t *const p = &state.memory[guest_start];
    const BOOL ret = VirtualFree(p, guest_bytes, MEM_DECOMMIT);
    LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#else
    if (state.host_page_size > state.page_size) {
        // Several guest 4 KiB pages can map into one host page (e.g. 16 KiB on iOS). Applying
        // PROT_NONE to the host-aligned range would remove RW from *all* guest pages in that
        // host page — including allocations that are still live (e.g. another module segment),
        // which then crashes later (EXC_BAD_ACCESS in relocate_entry / memcpy).
        // Physical backing may linger until process exit; the allocator bitmap is authoritative.
        return;
    }
    uint8_t *const p = &state.memory[guest_start];
    int ret = mprotect(p, guest_bytes, PROT_NONE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
    ret = madvise(p, guest_bytes, MADV_DONTNEED);
    LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
}

void ensure_guest_page_mapped_rw(MemState &state, Address vaddr) noexcept {
    if (state.host_page_size <= state.page_size)
        return;
    const Address guest_page = align_down(vaddr, static_cast<uint64_t>(state.page_size));
#ifdef _WIN32
    thread_local Address cache_guest_page = UINT32_MAX;
    if (cache_guest_page == guest_page)
        return;
    commit_guest_rw(state, guest_page, state.page_size);
    cache_guest_page = guest_page;
#else
    const Address host_lo = align_down(guest_page, static_cast<uint64_t>(state.host_page_size));
    const Address host_hi = align(guest_page + state.page_size, static_cast<uint64_t>(state.host_page_size));
    thread_local Address cache_lo = UINT32_MAX;
    thread_local Address cache_hi = 0;
    if (cache_lo != UINT32_MAX && host_lo == cache_lo && host_hi == cache_hi)
        return;
    commit_guest_rw(state, guest_page, state.page_size);
    cache_lo = host_lo;
    cache_hi = host_hi;
#endif
}
#endif // LIBRETRO

#ifndef LIBRETRO
void ensure_guest_page_mapped_rw(MemState &state, Address vaddr) noexcept {
    (void)state;
    (void)vaddr;
}
#endif

bool init(MemState &state, const bool use_page_table) {
#ifdef _WIN32
    SYSTEM_INFO system_info = {};
    GetSystemInfo(&system_info);
    const uint32_t system_page = system_info.dwPageSize;
#else
    const uint32_t system_page = static_cast<uint32_t>(sysconf(_SC_PAGESIZE));
#endif
#ifdef LIBRETRO
    state.host_page_size = std::max(STANDARD_PAGE_SIZE, system_page);
    // Guest (PS Vita) uses 4 KiB pages; the allocator bitmap must match ELF segment boundaries.
    // Using the OS page size (e.g. 16 KiB on recent iOS) would merge adjacent SELF segments and break allocate_at.
    state.page_size = STANDARD_PAGE_SIZE;
    if (state.host_page_size > state.page_size) {
        LOG_INFO("Guest page size {} B, host page size {} B (mprotect rounded to host pages).",
            state.page_size, state.host_page_size);
    }
#else
    state.page_size = std::max(STANDARD_PAGE_SIZE, system_page);
#endif

    assert(state.page_size >= 4096); // Limit imposed by Unicorn.
    assert(!use_page_table || state.page_size == KiB(4));

    void *preferred_address = reinterpret_cast<void *>(1ULL << 34);

#ifdef _WIN32
    state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(preferred_address, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);
    if (!state.memory) {
        // fallback
        state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(nullptr, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);

        if (!state.memory) {
            LOG_CRITICAL("VirtualAlloc failed: {}", get_error_msg());
            return false;
        }
    }
#else
    // http://man7.org/linux/man-pages/man2/mmap.2.html
    const int prot = PROT_NONE;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    const int fd = 0;
    const off_t offset = 0;
    // preferred_address is only a hint for mmap, if it can't use it, the kernel will choose itself the address
    state.memory = Memory(static_cast<uint8_t *>(mmap(preferred_address, TOTAL_MEM_SIZE, prot, flags, fd, offset)), delete_memory);
    if (state.memory.get() == MAP_FAILED) {
        LOG_CRITICAL("mmap failed {}", get_error_msg());
        return false;
    }
#endif

    const size_t table_length = TOTAL_MEM_SIZE / state.page_size;
    state.alloc_table = AllocPageTable(new AllocMemPage[table_length]);
    memset(state.alloc_table.get(), 0, sizeof(AllocMemPage) * table_length);

    state.allocator.set_maximum(table_length);

    const auto handler = [&state](uint8_t *addr, bool write) noexcept {
        return handle_access_violation(state, addr, write);
    };
    register_access_violation_handler(handler);

    const Address null_address = alloc_inner(state, 0, 1, "null", true);
    if (state.allocator.free_slot_count(0, 1) != 0) {
        LOG_CRITICAL("Failed to allocate null page (allocator bitmap not reserved)");
        return false;
    }
    assert(null_address == 0);
#ifdef _WIN32
    DWORD old_protect = 0;
#ifdef LIBRETRO
    const BOOL ret = VirtualProtect(state.memory.get(), state.host_page_size, PAGE_NOACCESS, &old_protect);
#else
    const BOOL ret = VirtualProtect(state.memory.get(), state.page_size, PAGE_NOACCESS, &old_protect);
#endif
    LOG_CRITICAL_IF(!ret, "VirtualProtect failed: {}", get_error_msg());
#elif !defined(__ANDROID__)
#ifdef LIBRETRO
    const int ret = mprotect(state.memory.get(), state.host_page_size, PROT_NONE);
#else
    const int ret = mprotect(state.memory.get(), state.page_size, PROT_NONE);
#endif
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif

    state.use_page_table = use_page_table;
    if (use_page_table) {
        state.page_table = PageTable(new PagePtr[TOTAL_MEM_SIZE / KiB(4)]);
        // we use an absolute offset (it is faster), so each entry is the same
        std::fill_n(state.page_table.get(), TOTAL_MEM_SIZE / KiB(4), state.memory.get());
    }

    return true;
}

static void delete_memory(uint8_t *memory) {
    if (memory != nullptr) {
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, 0, MEM_RELEASE);
        assert(ret);
#else
        munmap(memory, TOTAL_MEM_SIZE);
#endif
    }
}

bool is_valid_addr(const MemState &state, Address addr) {
    const uint32_t page_num = addr / state.page_size;
    return addr && state.allocator.free_slot_count(page_num, page_num + 1) == 0;
}

bool is_valid_addr_range(const MemState &state, Address start, Address end) {
    const uint32_t start_page = start / state.page_size;
    const uint32_t end_page = (end + state.page_size - 1) / state.page_size;
    return state.allocator.free_slot_count(start_page, end_page) == 0;
}

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force) {
    int page_num;
    if (force) {
        if (state.allocator.allocate_at(start_page, page_count) < 0) {
            LOG_CRITICAL("Failed to allocate at specific page");
            return 0;
        }
        page_num = start_page;
    } else {
        page_num = state.allocator.allocate_from(start_page, page_count, false);
        if (page_num < 0)
            return 0;
    }

    const uint32_t size = page_count * state.page_size;
    const Address addr = page_num * state.page_size;

#ifdef LIBRETRO
    commit_guest_rw(state, addr, size);
#else
    uint8_t *const memory = &state.memory[addr];
#ifdef _WIN32
    const void *const ret = VirtualAlloc(memory, size, MEM_COMMIT, PAGE_READWRITE);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
    const int ret = mprotect(memory, size, PROT_READ | PROT_WRITE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
#endif
    std::memset(&state.memory[addr], 0, size);

    AllocMemPage &page = state.alloc_table[page_num];
    assert(!page.allocated);
    page.allocated = 1;
    page.size = page_count;

    if (PAGE_NAME_TRACKING) {
        state.page_name_map.emplace(page_num, name);
    }

    return addr;
}

Address alloc_aligned(MemState &state, uint32_t size, const char *name, unsigned int alignment, Address start_addr) {
    if (alignment == 0)
        return alloc(state, size, name, start_addr);
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    size += alignment;
    const uint32_t page_count = align(size, state.page_size) / state.page_size;
    const Address addr = alloc_inner(state, start_addr / state.page_size, page_count, name, false);
    const Address align_addr = align(addr, alignment);
    const uint32_t page_num = addr / state.page_size;
    const uint32_t align_page_num = align_addr / state.page_size;

    if (page_num != align_page_num) {
        AllocMemPage &page = state.alloc_table[page_num];
        AllocMemPage &align_page = state.alloc_table[align_page_num];
        const uint32_t remnant_front = align_page_num - page_num;
        state.allocator.free(page_num, remnant_front);
        page.allocated = 0;
        align_page.allocated = 1;
        align_page.size = page.size - remnant_front;
    }

    return align_addr;
}

static void align_to_page(MemState &state, Address &addr, Address &size) {
    const Address end = align(addr + size, state.page_size);
    addr = align_down(addr, state.page_size);
    size = end - addr;
}

void unprotect_inner(MemState &state, Address addr, uint32_t size) {
    if (LOG_PROTECT) {
        fmt::print("Unprotect: {} {}\n", log_hex(addr), size);
    }
    uint8_t *addr_ptr = state.use_page_table ? state.page_table[addr / KiB(4)] : state.memory.get();

#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(&addr_ptr[addr], size - 1, PAGE_READWRITE, &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualProtect failed: {}", get_error_msg());
#else
#ifdef LIBRETRO
    if (state.host_page_size > state.page_size) {
        const Address host_start = align_down(addr, static_cast<uint64_t>(state.host_page_size));
        const Address host_end = align(addr + size, static_cast<uint64_t>(state.host_page_size));
        const int ret = mprotect(&addr_ptr[host_start], host_end - host_start, PROT_READ | PROT_WRITE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
        return;
    }
#endif
    const int ret = mprotect(&addr_ptr[addr], size, PROT_READ | PROT_WRITE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

void protect_inner(MemState &state, Address addr, uint32_t size, const MemPerm perm) {
    uint8_t *addr_ptr = state.use_page_table ? state.page_table[addr / KiB(4)] : state.memory.get();

#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(&addr_ptr[addr], size - 1, (perm == MemPerm::None) ? PAGE_NOACCESS : ((perm == MemPerm::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE), &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualProtect failed: {}", get_error_msg());
#else
    const int prot = (perm == MemPerm::None) ? PROT_NONE : ((perm == MemPerm::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE));
#ifdef LIBRETRO
    if (state.host_page_size > state.page_size) {
        const Address host_start = align_down(addr, static_cast<uint64_t>(state.host_page_size));
        const Address host_end = align(addr + size, static_cast<uint64_t>(state.host_page_size));
        const int ret = mprotect(&addr_ptr[host_start], host_end - host_start, prot);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
        return;
    }
#endif
    const int ret = mprotect(&addr_ptr[addr], size, prot);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept {
    const uintptr_t memory_addr = reinterpret_cast<uintptr_t>(state.memory.get());
    const uintptr_t fault_addr = reinterpret_cast<uintptr_t>(addr);

    Address vaddr = 0;
    const std::unique_lock<std::mutex> lock(state.protect_mutex);
    if (fault_addr < memory_addr || fault_addr >= memory_addr + TOTAL_MEM_SIZE) {
        if (state.use_page_table) {
            // this may come from an external mapping
            uint64_t addr_val = std::bit_cast<uint64_t>(addr);
            auto it = state.external_mapping.lower_bound(addr_val);
            if (it != state.external_mapping.end() && addr_val < it->first + it->second.size) {
                vaddr = static_cast<Address>(addr_val - it->first + it->second.address);
            } else {
                return false;
            }
        } else {
            return false;
        }
    } else {
        vaddr = static_cast<Address>(fault_addr - memory_addr);
    }

    if (!is_valid_addr(state, vaddr)) {
        return false;
    }
    if (LOG_PROTECT) {
        fmt::print("Access: {}\n", log_hex(vaddr));
    }

    auto it = state.protect_tree.lower_bound(vaddr);
    if (it == state.protect_tree.end()) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.page_size), state.page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    ProtectSegmentInfo &info = it->second;
    if (vaddr < it->first || vaddr >= it->first + info.size) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.page_size), state.page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    Address previous_beg = it->first;
    for (auto &[block_addr, block] : info.blocks) {
        block.callback(vaddr, write);
    }

    unprotect_inner(state, it->first, info.size);
    state.protect_tree.erase(it);

    return true;
}

bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    ProtectSegmentInfo protect(size, perm);
    align_to_page(state, addr, protect.size);

    ProtectBlockInfo block;
    block.size = size;
    block.callback = callback;

    protect.blocks.emplace(addr, std::move(block));

    auto it = state.protect_tree.lower_bound(addr);
    if (it == state.protect_tree.end() || it->first + it->second.size <= addr) {
        if (it == state.protect_tree.begin())
            it = state.protect_tree.end();
        else
            --it;
    }

    while (it != state.protect_tree.end() && it->first < addr + size) {
        const Address start = std::min(it->first, addr);
        protect.size = std::max(it->first + it->second.size, addr + protect.size) - start;
        addr = start;
        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect
        protect.perm = most_restrictive_perm(protect.perm, it->second.perm);

        if (it == state.protect_tree.begin()) {
            state.protect_tree.erase(it);
            break;
        }

        // protect tree is in reverse order, so decrease it
        state.protect_tree.erase(it--);
    }

    protect_inner(state, addr, protect.size, protect.perm);

    state.protect_tree.emplace(addr, std::move(protect));
    return true;
}

bool is_protecting(MemState &state, Address addr, MemPerm *perm) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    auto ite = state.protect_tree.lower_bound(addr);

    if (ite != state.protect_tree.end() && addr >= ite->first && addr < ite->first + ite->second.size) {
        if (perm)
            *perm = ite->second.perm;

        return true;
    }

    return false;
}

void sync_guest_write_if_protected(MemState &state, Address vaddr) noexcept {
#if defined(LIBRETRO) && defined(__APPLE__) && TARGET_OS_IOS
    const std::unique_lock<std::mutex> lock(state.protect_mutex);
    auto ite = state.protect_tree.lower_bound(vaddr);
    if (ite == state.protect_tree.end()) {
        return;
    }
    if (vaddr < ite->first || vaddr >= ite->first + ite->second.size) {
        return;
    }
    const MemPerm perm = ite->second.perm;
    const bool write_blocked = (perm == MemPerm::None) || (perm == MemPerm::ReadOnly);
    if (!write_blocked) {
        return;
    }
    ProtectSegmentInfo &info = ite->second;
    const Address seg_begin = ite->first;
    for (auto &[block_addr, block] : info.blocks) {
        block.callback(vaddr, true);
    }
    unprotect_inner(state, seg_begin, info.size);
    state.protect_tree.erase(ite);
#else
    (void)state;
    (void)vaddr;
#endif
}

void add_external_mapping(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr) {
    assert((size & 4095) == 0);
    if (!mem.use_page_table)
        return;

    uint64_t addr_value = std::bit_cast<uint64_t>(addr_ptr);
    uint8_t *page_table_entry = addr_ptr - addr;
    uint8_t *original_address = &mem.memory[addr];
    for (int block = 0; block < size / KiB(4); block++) {
        // this is not thread write safe, but hopefully not other thread is busy copying while this happens
        memcpy(addr_ptr + block * KiB(4), original_address + block * KiB(4), KiB(4));
        mem.page_table[addr / KiB(4) + block] = page_table_entry;
    }

    // set the first page table entry to the original value to be able to call protect_inner
    mem.page_table[addr / KiB(4)] = mem.memory.get();
    protect_inner(mem, addr, size, MemPerm::None);
    mem.page_table[addr / KiB(4)] = page_table_entry;

    const std::unique_lock<std::mutex> lock(mem.protect_mutex);
    mem.external_mapping[addr_value] = { addr, size };
}

void remove_external_mapping(MemState &mem, uint8_t *addr_ptr, uint32_t size) {
    uint64_t addr_value = std::bit_cast<uint64_t>(addr_ptr);
    MemExternalMapping mapping;
    if (mem.use_page_table) {
        const std::unique_lock<std::mutex> lock(mem.protect_mutex);
        auto it = mem.external_mapping.find(addr_value);
        assert(it != mem.external_mapping.end());

        mapping = it->second;
        mem.external_mapping.erase(it);
    } else {
        mapping.address = static_cast<Address>(addr_ptr - mem.memory.get());
        mapping.size = size;
    }

    // remove all protections on this range
    unprotect_inner(mem, mapping.address, mapping.size);
    {
        const std::unique_lock<std::mutex> lock(mem.protect_mutex);
        auto prot_it = mem.protect_tree.lower_bound(mapping.address);
        if (prot_it->first + prot_it->second.size <= mapping.address) {
            if (prot_it == mem.protect_tree.begin())
                prot_it = mem.protect_tree.end();
            else
                --prot_it;
        }

        while (prot_it != mem.protect_tree.end() && prot_it->first < mapping.address + mapping.size) {
            if (prot_it == mem.protect_tree.begin()) {
                mem.protect_tree.erase(prot_it);
                break;
            }

            mem.protect_tree.erase(prot_it--);
        }
    }

    if (mem.use_page_table) {
        // unprotect the original memory range
        mem.page_table[mapping.address / KiB(4)] = mem.memory.get();
        unprotect_inner(mem, mapping.address, mapping.size);
        // copy back and reset the page table
        for (int block = 0; block < mapping.size / KiB(4); block++) {
            // this is not thread write safe, but hopefully not other thread is busy copying while this happens
            memcpy(&mem.memory[mapping.address] + block * KiB(4), addr_ptr + block * KiB(4), KiB(4));
            mem.page_table[mapping.address / KiB(4) + block] = mem.memory.get();
        }
    }
}

Address alloc(MemState &state, uint32_t size, const char *name, Address start_addr) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_count = align(size, state.page_size) / state.page_size;
    const Address addr = alloc_inner(state, start_addr / state.page_size, page_count, name, false);
    return addr;
}

Address alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t wanted_page = address / state.page_size;
    size += address % state.page_size;
    const uint32_t page_count = align(size, state.page_size) / state.page_size;
    return alloc_inner(state, wanted_page, page_count, name, true);
}

Address try_alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    const uint32_t wanted_page = address / state.page_size;
    size += address % state.page_size;
    const uint32_t page_count = align(size, state.page_size) / state.page_size;
    if (state.allocator.free_slot_count(wanted_page, wanted_page + page_count) != page_count) {
        return 0;
    }
    return alloc_inner(state, wanted_page, page_count, name, true);
}

Block alloc_block(MemState &mem, uint32_t size, const char *name, Address start_addr) {
    const Address address = alloc(mem, size, name, start_addr);
    return Block(address, [&mem](Address stack) {
        free(mem, stack);
    });
}

void free(MemState &state, Address address) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_num = address / state.page_size;
    assert(page_num >= 0);

    AllocMemPage &page = state.alloc_table[page_num];
    if (!page.allocated) {
        LOG_CRITICAL("Freeing unallocated page");
    }
    page.allocated = 0;

    state.allocator.free(page_num, page.size);
    if (PAGE_NAME_TRACKING) {
        state.page_name_map.erase(page_num);
    }

    assert(!state.use_page_table || state.page_table[address / KiB(4)] == state.memory.get());
#ifdef LIBRETRO
    const Address guest_start = page_num * state.page_size;
    const uint32_t guest_bytes = page.size * state.page_size;
    decommit_guest_none(state, guest_start, guest_bytes);
#else
    uint8_t *const memory = &state.memory[page_num * state.page_size];

#ifdef _WIN32
    const BOOL ret = VirtualFree(memory, page.size * state.page_size, MEM_DECOMMIT);
    LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#else
    int ret = mprotect(memory, page.size * state.page_size, PROT_NONE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
    ret = madvise(memory, page.size * state.page_size, MADV_DONTNEED);
    LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
#endif
}

uint32_t mem_available(MemState &state) {
    return state.allocator.free_slot_count(0, state.allocator.max_offset) * state.page_size;
}

const char *mem_name(Address address, MemState &state) {
    if (PAGE_NAME_TRACKING) {
        return state.page_name_map.find(address / state.page_size)->second.c_str();
    }
    return "";
}

#ifdef _WIN32

static LONG WINAPI exception_handler(PEXCEPTION_POINTERS pExp) noexcept {
    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto ptr = reinterpret_cast<uint8_t *>(pExp->ExceptionRecord->ExceptionInformation[1]);
    const bool is_writing = pExp->ExceptionRecord->ExceptionInformation[0] == 1;
    const bool is_executing = pExp->ExceptionRecord->ExceptionInformation[0] == 8;

    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !is_executing) {
        if (access_violation_handler(ptr, is_writing)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    if (!AddVectoredExceptionHandler(1, exception_handler)) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
}

#else

static void signal_handler(int sig, siginfo_t *info, void *uct) noexcept {
    auto context = static_cast<ucontext_t *>(uct);

#ifdef __aarch64__
#ifdef __APPLE__
    const uint32_t esr = context->uc_mcontext->__es.__esr;
#else
    _aarch64_ctx *ctx = reinterpret_cast<_aarch64_ctx *>(context->uc_mcontext.__reserved);
    // get the ESR register
    while (ctx->magic != ESR_MAGIC) {
        if (ctx->magic == 0)
            [[unlikely]]
            raise(SIGTRAP);
        else
            [[likely]]
            ctx = reinterpret_cast<_aarch64_ctx *>(reinterpret_cast<uint8_t *>(ctx) + ctx->size);
    }

    const uint64_t esr = reinterpret_cast<esr_context *>(ctx)->esr;
#endif
    // https://developer.arm.com/documentation/ddi0595/2021-03/AArch64-Registers/ESR-EL1--Exception-Syndrome-Register--EL1-
    const uint32_t exception_class = static_cast<uint32_t>(esr) >> 26;
    const bool is_executing = (exception_class == 0b100000) || (exception_class == 0b100001);
    const bool is_data_abort = (exception_class == 0b100100) || (exception_class == 0b100101);
    const bool is_writing = is_data_abort && (esr & (1 << 6));
#else
#ifdef __APPLE__
    const uint64_t err = context->uc_mcontext->__es.__err;
#else
    const uint64_t err = context->uc_mcontext.gregs[REG_ERR];
#endif
    const bool is_executing = err & 0x10;
    const bool is_writing = err & 0x2;
#endif

    if (!is_executing) {
        if (access_violation_handler(reinterpret_cast<uint8_t *>(info->si_addr), is_writing)) {
            return;
        }
    }

    LOG_CRITICAL("Unhandled access to 0x{:X}", reinterpret_cast<uintptr_t>(info->si_addr));
    raise(SIGTRAP);
    return;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = signal_handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
#ifdef __APPLE__
    // When accessing memory region which is PROT_NONE on macOS, it is raising SIGBUS not SIGSEGV.
    // So apply same signal handler to SIGBUS
    if (sigaction(SIGBUS, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler to SIGBUS");
    }
#endif
}

#endif
