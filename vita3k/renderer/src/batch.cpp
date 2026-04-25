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

#include <renderer/commands.h>
#include <renderer/driver_functions.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/types.h>

#include <renderer/vulkan/types.h>

#include <config/state.h>
#include <functional>
#include <util/log.h>

struct FeatureState;

namespace renderer {
Command *generic_command_allocate() {
    return new Command;
}

void generic_command_free(Command *cmd) {
    delete cmd;
}

void complete_command(State &state, CommandHelper &helper, const int code) {
    auto lock = std::unique_lock(state.command_finish_one_mutex);
    helper.complete(code);
    state.command_finish_one.notify_all();
}

bool is_cmd_ready(MemState &mem, CommandList &command_list) {
    // we check if the cmd starts with a WaitSyncObject and if this is the case if it is ready
    if (!command_list.first || command_list.first->opcode != CommandOpcode::WaitSyncObject)
        return true;

    SceGxmSyncObject *sync = reinterpret_cast<Ptr<SceGxmSyncObject> *>(&command_list.first->data[0])->get(mem);
    const uint32_t timestamp = *reinterpret_cast<uint32_t *>(&command_list.first->data[sizeof(uint32_t)]);

    return sync->timestamp_current >= timestamp;
}

static bool wait_cmd(MemState &mem, CommandList &command_list) {
    // we assume here that the cmd starts with a WaitSyncObject

    SceGxmSyncObject *sync = reinterpret_cast<Ptr<SceGxmSyncObject> *>(&command_list.first->data[0])->get(mem);
    const uint32_t timestamp = *reinterpret_cast<uint32_t *>(&command_list.first->data[sizeof(uint32_t)]);

    // wait 500 micro seconds and then return in case should_display is set to true
    return renderer::wishlist(sync, timestamp, 500);
}

static void process_batch(renderer::State &state, const FeatureState &features, MemState &mem, Config &config, CommandList &command_list) {
    using CommandHandlerFunc = decltype(cmd_handle_set_context);

    const static std::map<CommandOpcode, CommandHandlerFunc *> handlers = {
        { CommandOpcode::SetContext, cmd_handle_set_context },
        { CommandOpcode::SyncSurfaceData, cmd_handle_sync_surface_data },
        { CommandOpcode::MidSceneFlush, cmd_handle_mid_scene_flush },
        { CommandOpcode::CreateContext, cmd_handle_create_context },
        { CommandOpcode::CreateRenderTarget, cmd_handle_create_render_target },
        { CommandOpcode::MemoryMap, cmd_handle_memory_map },
        { CommandOpcode::MemoryUnmap, cmd_handle_memory_unmap },
        { CommandOpcode::Draw, cmd_handle_draw },
        { CommandOpcode::TransferCopy, cmd_handle_transfer_copy },
        { CommandOpcode::TransferDownscale, cmd_handle_transfer_downscale },
        { CommandOpcode::TransferFill, cmd_handle_transfer_fill },
        { CommandOpcode::Nop, cmd_handle_nop },
        { CommandOpcode::SetState, cmd_handle_set_state },
        { CommandOpcode::SignalSyncObject, cmd_handle_signal_sync_object },
        { CommandOpcode::WaitSyncObject, cmd_handle_wait_sync_object },
        { CommandOpcode::SignalNotification, cmd_handle_notification },
        { CommandOpcode::NewFrame, cmd_new_frame },
        { CommandOpcode::DestroyRenderTarget, cmd_handle_destroy_render_target },
        { CommandOpcode::DestroyContext, cmd_handle_destroy_context }
    };

    Command *cmd = command_list.first;

    // Take a batch, and execute it. Hope it's not too large
    do {
        if (cmd == nullptr) {
            break;
        }

        auto handler = handlers.find(cmd->opcode);
        if (handler == handlers.end()) {
            LOG_ERROR("Unimplemented command opcode {}", static_cast<int>(cmd->opcode));
        } else {
            CommandHelper helper(cmd);
            handler->second(state, mem, config, helper, features, command_list.context);
        }

        Command *last_cmd = cmd;
        cmd = cmd->next;

        if (command_list.context) {
            command_list.context->free_func(last_cmd);
        } else {
            generic_command_free(last_cmd);
        }
    } while (true);
}

void process_batches(renderer::State &state, const FeatureState &features, MemState &mem, Config &config) {
    // Phase 1: drain until a displayable frame appears (original behaviour).
    // Phase 2: once should_display is set, do a quick non-blocking sweep for
    //          any remaining ready commands (especially SignalSyncObject) that
    //          were already queued.  This prevents the libretro split-frame
    //          model from stranding sync commands behind a NewFrame.
    //
    // The desktop front-end runs process_batches & render_frame in a tight
    // while(!quit) loop so exiting early on should_display is harmless — the
    // next iteration picks up leftovers microseconds later.  In libretro each
    // retro_run is one host frame (~16 ms); stranding a SignalSyncObject for a
    // full frame can deadlock the display_entry_thread, fill the display queue,
    // and block the guest in sceGxmDisplayQueueAddEntry.

    constexpr int64_t k_max_batch_drain_ms = 16;
    auto max_time = duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + k_max_batch_drain_ms;

    int batches_processed = 0;
    int loops_stalled = 0;

    // ---- phase 1: drain until a frame is ready --------------------------------
    while (!state.should_display) {
        auto cmd_list = state.command_buffer_queue.top(3);

        if (!cmd_list || !is_cmd_ready(mem, *cmd_list)) {
            if (state.context == nullptr) {
                if (batches_processed == 0 && loops_stalled == 0)
                    LOG_INFO("[GXM-BATCH] early-return: context is null (no GXM ctx yet)");
                return;
            }

            if (state.current_backend == Backend::OpenGL && config.current_config.v_sync)
                return;

            if (!cmd_list || !wait_cmd(mem, *cmd_list)) {
                auto curr_time = duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                if (curr_time >= max_time) {
                    if (batches_processed == 0)
                        LOG_INFO("[GXM-BATCH] budget expired: {} loops stalled, qsize={}",
                                 loops_stalled, state.command_buffer_queue.size());
                    goto phase2;
                }

                ++loops_stalled;
                if (loops_stalled == 1 && batches_processed == 0) {
                    uint32_t op = cmd_list ? static_cast<uint32_t>(cmd_list->first ? static_cast<uint32_t>(cmd_list->first->opcode) : 0xFFFF) : 0xFFFF;
                    LOG_INFO("[GXM-BATCH] stalled: cmd_list={} first_op={} qsize={}",
                             cmd_list ? "yes" : "no", op, state.command_buffer_queue.size());
                }
                continue;
            }
        }

        state.command_buffer_queue.pop();
        process_batch(state, features, mem, config, *cmd_list);
        ++batches_processed;
    }

    // ---- phase 2: quick sweep of already-queued ready commands ---------------
    // Use 0-us timeout so we never block waiting for *new* work; we just pick
    // up whatever command lists were already submitted before the NewFrame that
    // set should_display.
phase2:
    while (true) {
        auto cmd_list = state.command_buffer_queue.top(1);  // 1us timeout, truly non-blocking

        if (!cmd_list || !is_cmd_ready(mem, *cmd_list)) {
            // Queue empty or front entry not ready — don't wait, next
            // retro_run will retry.
            break;
        }

        state.command_buffer_queue.pop();
        process_batch(state, features, mem, config, *cmd_list);
        ++batches_processed;
    }
}

void reset_command_list(CommandList &command_list) {
    command_list.first = nullptr;
    command_list.last = nullptr;
}
} // namespace renderer
