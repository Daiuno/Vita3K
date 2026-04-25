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

#include <renderer/vulkan/functions.h>

#include <renderer/vulkan/gxm_to_vulkan.h>

#include <gxm/functions.h>
#include <renderer/functions.h>

#include <util/log.h>

#include <algorithm>
#include <cmath>

namespace renderer::vulkan {

namespace {

// MoltenVK / MTLDebugRenderCommandEncoder: scissor must lie within the current render pass extent.
void clamp_scissor_to_render_target(VKContext &context) {
    if (!context.render_target)
        return;
    const uint32_t rtw = context.render_target->width;
    const uint32_t rth = context.render_target->height;
    if (rtw == 0 || rth == 0)
        return;

    const int32_t ox = context.scissor.offset.x;
    const int32_t oy = context.scissor.offset.y;
    if (ox < 0 || oy < 0)
        return;

    const auto uox = static_cast<uint32_t>(ox);
    const auto uoy = static_cast<uint32_t>(oy);
    if (uox >= rtw || uoy >= rth) {
        context.scissor.extent.width = 0;
        context.scissor.extent.height = 0;
        return;
    }
    context.scissor.extent.width = std::min(context.scissor.extent.width, rtw - uox);
    context.scissor.extent.height = std::min(context.scissor.extent.height, rth - uoy);
}

// MoltenVK: debug layer rejects NaN/Inf and non-positive *width* / *degenerate* height on setViewport.
// With VK_KHR_maintenance1 (required by this renderer), VkViewport.height may be **negative** for Y-flip;
// treating (height <= 0) as invalid was wrong and forced a full-RT fallback — if that RT was a 1×1
// placeholder, the guest ended up with a 1×1 viewport and stalled GPU/sync (e.g. Libretro + MoltenVK).
void sanitize_viewport_vulkan(VKContext &context) {
    if (!context.render_target)
        return;
    auto &vp = context.viewport;
    const float rtw = static_cast<float>(context.render_target->width);
    const float rth = static_cast<float>(context.render_target->height);

    const bool finite = std::isfinite(vp.x) && std::isfinite(vp.y) && std::isfinite(vp.width) && std::isfinite(vp.height)
        && std::isfinite(vp.minDepth) && std::isfinite(vp.maxDepth);
    // Width must be strictly positive. Height may be negative (Vulkan Y-flip); only ~0 is degenerate.
    const bool width_invalid = !std::isfinite(vp.width) || vp.width <= 0.f;
    const bool height_invalid = !std::isfinite(vp.height) || (std::abs(vp.height) <= 1e-20f);
    if (!finite || width_invalid || height_invalid) {
#ifdef LIBRETRO
        LOG_WARN_ONCE("[VITA3K-LR] Clamping invalid viewport (non-finite, non-positive width, or zero height) to render target {}x{}",
            context.render_target->width, context.render_target->height);
#endif
        vp = vk::Viewport{
            .x = 0.f,
            .y = 0.f,
            .width = std::max(rtw, 1.f),
            .height = std::max(rth, 1.f),
            .minDepth = 0.f,
            .maxDepth = 1.f
        };
        return;
    }
    vp.minDepth = std::clamp(vp.minDepth, 0.f, 1.f);
    vp.maxDepth = std::clamp(vp.maxDepth, 0.f, 1.f);
}

} // namespace

void sync_clipping(VKContext &context) {
    if (!context.render_target)
        return;

    const float res_multiplier = context.state.res_multiplier;

    const int scissor_x = context.record.region_clip_min.x;
    const int scissor_y = context.record.region_clip_min.y;

    const unsigned int scissor_w = std::max(context.record.region_clip_max.x - context.record.region_clip_min.x + 1, 0);
    const unsigned int scissor_h = std::max(context.record.region_clip_max.y - context.record.region_clip_min.y + 1, 0);

    switch (context.record.region_clip_mode) {
    case SCE_GXM_REGION_CLIP_NONE:
        // make the scissor the size of the framebuffer
        context.scissor = vk::Rect2D{ { 0, 0 }, { context.render_target->width, context.render_target->height } };
        break;
    case SCE_GXM_REGION_CLIP_ALL:
        context.scissor = vk::Rect2D{};
        break;
    case SCE_GXM_REGION_CLIP_OUTSIDE:
        context.scissor = vk::Rect2D{
            { static_cast<int32_t>(scissor_x * res_multiplier), static_cast<int32_t>(scissor_y * res_multiplier) },
            { static_cast<uint32_t>(scissor_w * res_multiplier), static_cast<uint32_t>(scissor_h * res_multiplier) }
        };
        break;
    case SCE_GXM_REGION_CLIP_INSIDE:
        // TODO: Implement SCE_GXM_REGION_CLIP_INSIDE
        LOG_WARN("STUB SCE_GXM_REGION_CLIP_INSIDE");
        context.scissor = vk::Rect2D{ { 0, 0 }, { context.render_target->width, context.render_target->height } };
        break;
    }

    // Vulkan does not allow the offset to be negative
    if (context.scissor.offset.x < 0) {
        context.scissor.extent.width = std::max(context.scissor.extent.width - context.scissor.offset.x, 0U);
        context.scissor.offset.x = 0;
    }

    if (context.scissor.offset.y < 0) {
        context.scissor.extent.height = std::max(context.scissor.extent.height - context.scissor.offset.y, 0U);
        context.scissor.offset.y = 0;
    }

    clamp_scissor_to_render_target(context);

    if (!context.is_recording)
        return;

    context.render_cmd.setScissor(0, context.scissor);
}

void sync_stencil_func(VKContext &context, const bool is_back) {
    if (!context.is_recording)
        return;

    vk::StencilFaceFlags face;
    GxmStencilStateValues *state;
    if (context.record.two_sided == SCE_GXM_TWO_SIDED_DISABLED) {
        // the back state is not used when two sided is disabled
        if (is_back)
            return;

        face = vk::StencilFaceFlagBits::eFrontAndBack;
        state = &context.record.front_stencil_state_values;
    } else {
        face = is_back ? vk::StencilFaceFlagBits::eBack : vk::StencilFaceFlagBits::eFront;
        state = is_back ? &context.record.back_stencil_state_values : &context.record.front_stencil_state_values;
    }

    context.render_cmd.setStencilCompareMask(face, state->compare_mask);
    context.render_cmd.setStencilReference(face, state->ref);
    context.render_cmd.setStencilWriteMask(face, state->write_mask);
}

void sync_depth_bias(VKContext &context) {
    if (!context.is_recording)
        return;

    context.render_cmd.setDepthBias(static_cast<float>(context.record.depth_bias_unit), 0.0, static_cast<float>(context.record.depth_bias_slope));
}

void sync_depth_data(VKContext &context) {
    if (context.record.depth_stencil_surface.force_load)
        return;

    vk::ClearDepthStencilValue clear_value{
        .depth = context.record.depth_stencil_surface.background_depth
    };
    vk::ClearAttachment clear_attachment{
        .aspectMask = vk::ImageAspectFlagBits::eDepth,
        .clearValue = { .depthStencil = clear_value }
    };
    vk::ClearRect clear_rect{
        .rect = vk::Rect2D{
            .offset = { 0, 0 },
            .extent = { context.render_target->width, context.render_target->height } },
        .baseArrayLayer = 0,
        .layerCount = 1
    };
    context.render_cmd.clearAttachments(clear_attachment, clear_rect);
}

void sync_stencil_data(VKContext &context, const MemState &mem) {
    if (context.record.depth_stencil_surface.force_load)
        return;

    vk::ClearDepthStencilValue clear_value{
        .stencil = context.record.depth_stencil_surface.stencil
    };
    vk::ClearAttachment clear_attachment{
        .aspectMask = vk::ImageAspectFlagBits::eStencil,
        .clearValue = { .depthStencil = clear_value }
    };
    vk::ClearRect clear_rect{
        .rect = vk::Rect2D{
            .offset = { 0, 0 },
            .extent = { context.render_target->width, context.render_target->height } },
        .baseArrayLayer = 0,
        .layerCount = 1
    };
    context.render_cmd.clearAttachments(clear_attachment, clear_rect);
}

void sync_point_line_width(VKContext &context, const bool is_front) {
    if (!context.is_recording)
        return;

    if (is_front && context.state.physical_device_features.wideLines)
        context.render_cmd.setLineWidth(context.record.line_width * context.state.res_multiplier);
}

void sync_viewport_flat(VKContext &context) {
    if (!context.render_target)
        return;
    context.viewport = vk::Viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(context.render_target->width),
        .height = static_cast<float>(context.render_target->height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    sanitize_viewport_vulkan(context);

    if (!context.is_recording)
        return;
    context.render_cmd.setViewport(0, context.viewport);
}

void sync_viewport_real(VKContext &context, const float xOffset, const float yOffset, const float zOffset,
    const float xScale, const float yScale, const float zScale) {
    if (!context.render_target)
        return;
    if (xScale < 0)
        LOG_ERROR("Game is using a viewport with negative width!");

    const float w = std::abs(2 * xScale);
    const float h = 2 * yScale;
    const float x = xOffset - std::abs(xScale);
    const float y = yOffset - yScale;

    const float res_multiplier = context.state.res_multiplier;

    // Degenerate or non-finite GXM viewport: e.g. yScale==0 → h==0; NaN scales bypass (NaN <= x) is
    // false and would reach sanitize. MoltenVK rejects setViewport; sanitize clamps to RT — on a 1×1
    // transient attachment that stalls sync. Fall back to flat full-RT viewport (matches GL abs path).
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(res_multiplier)) {
        sync_viewport_flat(context);
        return;
    }
    {
        const float vw = w * res_multiplier;
        const float vh = h * res_multiplier;
        if (!std::isfinite(vw) || !std::isfinite(vh) || vw <= 1e-6f || std::abs(vh) <= 1e-6f) {
            sync_viewport_flat(context);
            return;
        }
    }

    // https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkViewport.html
    // https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vertexpostproc-viewport
    // on gxm: x_f = xOffset + xScale * (x / w)
    // on vulkan: x_f = (x + width/2) + width/2 * (x / w)
    // the depth viewport is applied directly in the shader
    context.viewport = vk::Viewport{
        .x = x * res_multiplier,
        .y = y * res_multiplier,
        .width = w * res_multiplier,
        .height = h * res_multiplier,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    sanitize_viewport_vulkan(context);

    if (!context.is_recording)
        return;
    context.render_cmd.setViewport(0, context.viewport);
}

void sync_visibility_buffer(VKContext &context, Ptr<uint32_t> buffer, uint32_t stride) {
    if (!buffer) {
        context.current_visibility_buffer = nullptr;
        return;
    }

    auto ite = context.visibility_buffers.find(buffer.address());
    if (ite == context.visibility_buffers.end()) {
        // create a new query pool
        vk::QueryPoolCreateInfo pool_info{
            .queryType = vk::QueryType::eOcclusion,
            .queryCount = static_cast<uint32_t>(stride / sizeof(uint32_t))
        };
        vk::QueryPool query_pool = context.state.device.createQueryPool(pool_info);

        context.visibility_buffers[buffer.address()] = { buffer.address(), nullptr, 0, static_cast<uint32_t>(stride / sizeof(uint32_t)), query_pool };
        ite = context.visibility_buffers.find(buffer.address());

        std::tie(ite->second.gpu_buffer, ite->second.buffer_offset) = context.state.get_matching_mapping(buffer.cast<void>());
        // the + 1 is to make computing the ranges easier in context.cpp
        ite->second.queries_used.resize(ite->second.size + 1, false);
    }

    context.current_visibility_buffer = &ite->second;
}

void sync_visibility_index(VKContext &context, bool enable, uint32_t index, bool is_increment) {
    if (context.current_visibility_buffer == nullptr) {
        context.current_query_idx = enable ? index : -1;
        context.is_query_op_increment = is_increment;
        return;
    }

    if (index >= context.current_visibility_buffer->size) {
        LOG_WARN_ONCE("Using visibility index {} which is too big for the buffer", index);
        index = 0;
    }

    if (!enable) {
        if (context.is_in_query) {
            context.render_cmd.endQuery(context.current_visibility_buffer->query_pool, context.current_query_idx);
            context.is_in_query = false;
        }

        context.current_query_idx = -1;
        return;
    }

    // do not end the query if it's the same index
    if (context.is_in_query && context.current_query_idx != index) {
        context.render_cmd.endQuery(context.current_visibility_buffer->query_pool, context.current_query_idx);
        context.is_in_query = false;
    }
    context.current_query_idx = index;
    context.is_query_op_increment = is_increment;
}

void refresh_pipeline(VKContext &context) {
    context.refresh_pipeline = true;
}

} // namespace renderer::vulkan
