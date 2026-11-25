#pragma once

#include "gx.hpp"

namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
// Points TEV and indirect stages that name a texture coordinate no texgen produces at GX_TEXCOORD0.
void resolve_ungenerated_texcoords(ShaderConfig& config) noexcept;
gfx::Range build_uniform(const ShaderInfo& info) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
