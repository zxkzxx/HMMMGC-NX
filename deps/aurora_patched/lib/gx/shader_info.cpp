#include "shader_info.hpp"

#include "../gfx/recording.hpp"

#include <cmath>

#include <tracy/Tracy.hpp>

namespace aurora::gx {
namespace {
constexpr Module Log{"aurora::gx"};

bool is_alpha_bump_channel(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

Vec4<float> texture_size_bias(const gfx::TextureBind& tex) {
  auto width = static_cast<float>(tex.texObj.width());
  auto height = static_cast<float>(tex.texObj.height());
  float vpBias = 0.f;
  if (tex.ref && tex.ref->hasArbitraryMips) {
    const float viewportScale =
        std::min(g_gxState.renderViewport.width / std::max(g_gxState.logicalViewport.width, 1.f),
                 g_gxState.renderViewport.height / std::max(g_gxState.logicalViewport.height, 1.f));
    const float replacementScale = static_cast<float>(tex.ref->size.width) / std::max(width, 1.f);
    vpBias = std::log2(viewportScale / std::max(replacementScale, 0.001f));
  }
  return {width, height, tex.texObj.lod_bias() + vpBias, 0.0f};
}

void color_arg_reg_info(GXTevColorArg arg, const TevStage& stage, ShaderInfo& info) {
  switch (arg) {
  case GX_CC_CPREV:
  case GX_CC_APREV:
    if (!info.writesTevReg.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CC_C0:
  case GX_CC_A0:
    if (!info.writesTevReg.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CC_C1:
  case GX_CC_A1:
    if (!info.writesTevReg.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CC_C2:
  case GX_CC_A2:
    if (!info.writesTevReg.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CC_TEXC:
  case GX_CC_TEXA:
    if (stage.texMapId != GX_TEXMAP_NULL) {
      CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");
      info.sampledTexCoords.set(stage.texCoordId);
      info.sampledTextures.set(stage.texMapId);
    }
    break;
  case GX_CC_RASC:
  case GX_CC_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      info.sampledColorChannels.set(color_channel(stage.channelId));
    }
    break;
  case GX_CC_KONST:
    switch (stage.kcSel) {
    case GX_TEV_KCSEL_K0:
    case GX_TEV_KCSEL_K0_R:
    case GX_TEV_KCSEL_K0_G:
    case GX_TEV_KCSEL_K0_B:
    case GX_TEV_KCSEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KCSEL_K1:
    case GX_TEV_KCSEL_K1_R:
    case GX_TEV_KCSEL_K1_G:
    case GX_TEV_KCSEL_K1_B:
    case GX_TEV_KCSEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KCSEL_K2:
    case GX_TEV_KCSEL_K2_R:
    case GX_TEV_KCSEL_K2_G:
    case GX_TEV_KCSEL_K2_B:
    case GX_TEV_KCSEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KCSEL_K3:
    case GX_TEV_KCSEL_K3_R:
    case GX_TEV_KCSEL_K3_G:
    case GX_TEV_KCSEL_K3_B:
    case GX_TEV_KCSEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

void alpha_arg_reg_info(GXTevAlphaArg arg, const TevStage& stage, ShaderInfo& info) {
  switch (arg) {
  case GX_CA_APREV:
    if (!info.writesTevReg.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CA_A0:
    if (!info.writesTevReg.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CA_A1:
    if (!info.writesTevReg.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CA_A2:
    if (!info.writesTevReg.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CA_TEXA:
    if (stage.texMapId != GX_TEXMAP_NULL) {
      CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");
      info.sampledTexCoords.set(stage.texCoordId);
      info.sampledTextures.set(stage.texMapId);
    }
    break;
  case GX_CA_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      info.sampledColorChannels.set(color_channel(stage.channelId));
    }
    break;
  case GX_CA_KONST:
    switch (stage.kaSel) {
    case GX_TEV_KASEL_K0_R:
    case GX_TEV_KASEL_K0_G:
    case GX_TEV_KASEL_K0_B:
    case GX_TEV_KASEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KASEL_K1_R:
    case GX_TEV_KASEL_K1_G:
    case GX_TEV_KASEL_K1_B:
    case GX_TEV_KASEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KASEL_K2_R:
    case GX_TEV_KASEL_K2_G:
    case GX_TEV_KASEL_K2_B:
    case GX_TEV_KASEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KASEL_K3_R:
    case GX_TEV_KASEL_K3_G:
    case GX_TEV_KASEL_K3_B:
    case GX_TEV_KASEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

f32 tex_offset(GXTexOffset offs) noexcept {
  switch (offs) {
  default:
  case GX_TO_ZERO:
    return 0.f;
  case GX_TO_SIXTEENTH:
    return 1.f / 16.f;
  case GX_TO_EIGHTH:
    return 1.f / 8.f;
  case GX_TO_FOURTH:
    return 1.f / 4.f;
  case GX_TO_HALF:
    return 1.f / 2.f;
  case GX_TO_ONE:
    return 1.f;
  }
}

u32 point_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].pointOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

u32 line_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].lineOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}
} // namespace

// A stage may name a texture coordinate past the texgens set up (GXSetNumTexGens): the hardware then
// reads texture coordinate 0 (Dolphin, PixelShaderGen.cpp, "tex coord 0 is used"). The shader
// generator has no source for such a coordinate and aborts ("unhandled tcg src"), so stages are
// pointed at GX_TEXCOORD0 here, as the hardware does. Wind Waker's frozen Mothula samples
// GX_TEXCOORD2 with two texgens. With no texgen at all there is nothing to fall back to.
void resolve_ungenerated_texcoords(ShaderConfig& config) noexcept {
  if (config.tcgs[0].src == GX_MAX_TEXGENSRC) {
    return;
  }
  const auto resolve = [&](GXTexCoordID& id) {
    if (id != GX_TEXCOORD_NULL && (id >= MaxTexCoord || config.tcgs[id].src == GX_MAX_TEXGENSRC)) {
      id = GX_TEXCOORD0;
    }
  };
  for (u32 i = 0; i < config.tevStageCount && i < MaxTevStages; ++i) {
    resolve(config.tevStages[i].texCoordId);
  }
  for (u32 i = 0; i < config.numIndStages && i < MaxIndStages; ++i) {
    resolve(config.indStages[i].texCoordId);
  }
}

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ZoneScoped;

  ShaderInfo info{
      // render/logical viewport size, proj
      .uniformSize = 8 + 8 + 64,
  };

  if (config.lineMode != 0) {
    info.uniformSize += 4 + 4 + 4 + 4; // line_width, line_aspect_y, line_tex_offset, line_texcoord_mask
    info.lineMode = config.lineMode;
  }

  for (int attr = 0; attr < config.attrs.size(); attr++) {
    const auto attrType = config.attrs[attr].attrType;
    if ((attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX)) && attrType == GX_DIRECT) {
      info.indexAttr.set(attr);
    } else if (attrType == GX_INDEX8 || attrType == GX_INDEX16) {
      info.indexAttr.set(attr);
    }
  }

  // 10 position matrices, 10 texture matrices, 10 normal matrices.
  info.uniformSize += sizeof(Mat3x4<float>) * 30;

  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    // Color pass
    color_arg_reg_info(stage.colorPass.a, stage, info);
    color_arg_reg_info(stage.colorPass.b, stage, info);
    color_arg_reg_info(stage.colorPass.c, stage, info);
    color_arg_reg_info(stage.colorPass.d, stage, info);
    info.writesTevReg.set(stage.colorOp.outReg);

    // Alpha pass
    alpha_arg_reg_info(stage.alphaPass.a, stage, info);
    alpha_arg_reg_info(stage.alphaPass.b, stage, info);
    alpha_arg_reg_info(stage.alphaPass.c, stage, info);
    alpha_arg_reg_info(stage.alphaPass.d, stage, info);
    if (!info.writesTevReg.test(stage.alphaOp.outReg)) {
      // If we're writing alpha to a register that's not been
      // written to in the shader, load from uniform buffer
      info.loadsTevReg.set(stage.alphaOp.outReg);
      info.writesTevReg.set(stage.alphaOp.outReg);
    }
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    // Skip if not enabled
    if (stage.indTexStage >= config.numIndStages) {
      continue;
    }
    const bool usesIndStage = stage.indTexMtxId != GX_ITM_OFF || is_alpha_bump_channel(stage.channelId);
    const bool usesTevTexCoord = stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF ||
                                 stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    if (usesTevTexCoord && stage.texCoordId != GX_TEXCOORD_NULL) {
      info.sampledTexCoords.set(stage.texCoordId);
    }
    if (usesTevTexCoord && stage.texMapId != GX_TEXMAP_NULL &&
        (stage.indTexMtxId != GX_ITM_OFF || stage.indTexAddPrev)) {
      info.sampledTextures.set(stage.texMapId);
    }
    if (!usesIndStage) {
      continue;
    }
    info.usedIndStages.set(stage.indTexStage);
    const auto& indStage = config.indStages[stage.indTexStage];
    info.sampledTextures.set(indStage.texMapId);
    info.sampledTexCoords.set(indStage.texCoordId);
    info.sampledIndTextures.set(indStage.texMapId);
    // Track which indirect matrix is used
    if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_0);
    } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_S0);
    } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_T0);
    }
  }

  // Emboss bump needs its source texcoord generated and a light enabled
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      info.sampledTexCoords.set(tcg.embossSrc);
      info.lightingEnabled = true;
    }
    // GX_TG_SRTG reads the R and G of a lit colour channel's output, so that channel must be
    // computed even when no TEV stage rasterizes it
    if (tcg.type == GX_TG_SRTG && (tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1)) {
      info.sampledColorChannels.set(tcg.src - GX_TG_COLOR0);
    }
  }

  info.uniformSize += info.loadsTevReg.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cc.lightingEnabled || cca.lightingEnabled) {
        info.lightingEnabled = true;
      }
    }
  }
  if (info.lightingEnabled) {
    // Lights + light state for all channels
    info.uniformSize += 16 + sizeof(Light) * GX::MaxLights;
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cc.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cca.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
    }
  }
  info.uniformSize += info.sampledKColors.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.postMtx != GX_PTIDENTITY) {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      info.usesPTTexMtx.set(postMtxIdx);
    }
  }
  if (info.usesPTTexMtx.any()) {
    info.uniformSize += sizeof(Mat3x4<float>) * MaxPTTexMtx;
  }
  if (config.fogType != GX_FOG_NONE) {
    info.usesFog = true;
    info.uniformSize += sizeof(Fog);
  }
  info.uniformSize += MaxTexCoord * sizeof(Vec4<float>);
  if (info.usedIndTexMtxs.any()) {
    info.uniformSize += MaxIndTexMtxs * sizeof(Mat2x4<float>);
  }
  info.uniformSize += info.sampledTextures.count() * sizeof(Vec4<float>);
  info.uniformSize = gfx::align_uniform(info.uniformSize);
  if (info.uniformSize > MaxUniformSize) {
    Log.fatal("Uniform size exceeds maximum: {} > {}", info.uniformSize, MaxUniformSize);
  }
  return info;
}

// GX_AURORA_SET_VIEW_DELTA (native/patches/aurora/0019): on, and the draw uses the perspective
// projection it names (the world view's), so the view moves by the delta for this draw.
static bool view_delta_applies() noexcept {
  const auto& vd = g_gxState.viewDelta;
  if (!vd.active) {
    return false;
  }
  const auto& p = g_gxState.proj;
  if (p.m3[2] != -1.f || p.m3[3] != 0.f) {
    return false; // orthographic
  }
  return p.m0[0] == vd.projFrom[0] && p.m0[2] == vd.projFrom[1] && p.m1[1] == vd.projFrom[2] &&
         p.m1[2] == vd.projFrom[3] && p.m2[2] == vd.projFrom[4] && p.m2[3] == vd.projFrom[5];
}

// d = C * m for row-major 3x4 affine matrices; withTranslation false: C's rotation only (normals).
static Mat3x4<float> view_delta_mul(const Mat3x4<float>& c, const Mat3x4<float>& m, bool withTranslation) noexcept {
  const Vec4<float>* cr[3] = {&c.m0, &c.m1, &c.m2};
  const Vec4<float>* mr[3] = {&m.m0, &m.m1, &m.m2};
  Mat3x4<float> d;
  Vec4<float>* dr[3] = {&d.m0, &d.m1, &d.m2};
  for (int i = 0; i < 3; ++i) {
    const Vec4<float>& ci = *cr[i];
    for (int j = 0; j < 4; ++j) {
      (*dr[i])[j] = ci[0] * (*mr[0])[j] + ci[1] * (*mr[1])[j] + ci[2] * (*mr[2])[j];
    }
    if (withTranslation) {
      (*dr[i])[3] += ci[3];
    }
  }
  return d;
}

static Vec4<float> view_delta_vec(const Mat3x4<float>& c, const Vec4<float>& v, bool point) noexcept {
  const f32 w = point ? 1.f : 0.f;
  return Vec4<float>{c.m0[0] * v[0] + c.m0[1] * v[1] + c.m0[2] * v[2] + c.m0[3] * w,
                     c.m1[0] * v[0] + c.m1[1] * v[1] + c.m1[2] * v[2] + c.m1[3] * w,
                     c.m2[0] * v[0] + c.m2[1] * v[1] + c.m2[2] * v[2] + c.m2[3] * w, v[3]};
}

static void fill_uniform(ByteBuffer& buf, const ShaderInfo& info) noexcept {
  buf.reserve_extra(info.uniformSize);
  const bool viewDelta = view_delta_applies();

  buf.append<f32>(g_gxState.renderViewport.width);
  buf.append<f32>(g_gxState.renderViewport.height);
  buf.append<f32>(g_gxState.logicalViewport.width);
  buf.append<f32>(g_gxState.logicalViewport.height);
  if (info.lineMode != 0) {
    if (info.lineMode == 3) { // GX_POINTS
      buf.append<f32>(static_cast<f32>(g_gxState.pointSize) / 6.f);
      buf.append<f32>(1.0f);
      buf.append<f32>(tex_offset(g_gxState.pointTexOffset));
      buf.append<u32>(point_texcoord_mask());
    } else { // GX_LINES / GX_LINESTRIP
      buf.append<f32>(static_cast<f32>(g_gxState.lineWidth) / 6.f);
      buf.append<f32>(g_gxState.lineHalfAspect ? 0.5f : 1.f);
      buf.append<f32>(tex_offset(g_gxState.lineTexOffset));
      buf.append<u32>(line_texcoord_mask());
    }
  }
  auto proj = g_gxState.proj;
  if (viewDelta) {
    // the world view's projection swapped (xf_load_projection's perspective layout)
    const auto& to = g_gxState.viewDelta.projTo;
    proj.m0[0] = to[0];
    proj.m0[2] = to[1];
    proj.m1[1] = to[2];
    proj.m1[2] = to[3];
    proj.m2[2] = to[4];
    proj.m2[3] = to[5];
  }
  if constexpr (UseReversedZ) {
    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  buf.append(proj);

  const auto& delta = g_gxState.viewDelta.delta;
  for (int i = 0; i < MaxPnMtx; i++) {
    if (viewDelta) {
      buf.append(view_delta_mul(delta, g_gxState.pnMtx[i].pos, true));
    } else {
      buf.append(g_gxState.pnMtx[i].pos);
    }
  }

  for (int i = 0; i < MaxTexMtx; i++) {
    buf.append(g_gxState.texMtxs[i]);
  }

  for (int i = 0; i < MaxPnMtx; i++) {
    if (viewDelta) {
      buf.append(view_delta_mul(delta, g_gxState.pnMtx[i].nrm, false));
    } else {
      buf.append(g_gxState.pnMtx[i].nrm);
    }
  }

  for (int i = 0; i < info.loadsTevReg.size(); ++i) {
    if (info.loadsTevReg.test(i)) {
      buf.append(g_gxState.colorRegs[i]);
    }
  }
  if (info.lightingEnabled) {
    // Lights
    static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
    if (viewDelta) {
      // lights are given in view space: they move with the view
      auto lights = g_gxState.lights;
      for (auto& light : lights) {
        light.pos = view_delta_vec(delta, light.pos, true);
        light.dir = view_delta_vec(delta, light.dir, false);
      }
      buf.append(lights);
    } else {
      buf.append(g_gxState.lights);
    }
    // Light state for all channels
    for (int i = 0; i < 4; ++i) {
      buf.append<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong());
    }
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& ccc = g_gxState.colorChannelConfig[i];
    const auto& ccs = g_gxState.colorChannelState[i];
    if (ccc.lightingEnabled && ccc.ambSrc == GX_SRC_REG) {
      buf.append(ccs.ambColor);
    }
    if (ccc.matSrc == GX_SRC_REG) {
      buf.append(ccs.matColor);
    }
    const auto& ccca = g_gxState.colorChannelConfig[i + GX_ALPHA0];
    const auto& ccsa = g_gxState.colorChannelState[i + GX_ALPHA0];
    if (ccca.lightingEnabled && ccca.ambSrc == GX_SRC_REG) {
      buf.append(ccsa.ambColor);
    }
    if (ccca.matSrc == GX_SRC_REG) {
      buf.append(ccsa.matColor);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      buf.append(g_gxState.kcolors[i]);
    }
  }
  if (info.usesPTTexMtx.any()) {
    for (int i = 0; i < info.usesPTTexMtx.size(); ++i) {
      buf.append(g_gxState.ptTexMtxs[i]);
    }
  }
  if (info.usesFog) {
    const auto& state = g_gxState.fog;
    const float logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
    const float renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
    Fog fog{
        .color = state.color,
        .a = state.a,
        .b = state.b,
        .c = state.c,
        .rangeCenter = ((static_cast<float>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                       1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
    };
    for (u32 i = 0; i < state.rangeK.size(); ++i) {
      const u32 source = (i & ~1u) | (1u - (i & 1u));
      fog.rangeK[i / 4][i % 4] = static_cast<float>(state.rangeK[source]) / 64.f;
    }
    fog.rangeK[2][2] = fog.rangeK[2][1];
    fog.rangeK[2][3] = fog.rangeK[2][1];
    buf.append(fog);
  }
  for (const auto& scale : g_gxState.texCoordScales) {
    buf.append(Vec4{static_cast<f32>(scale.scaleS) + 1.0f, static_cast<f32>(scale.scaleT) + 1.0f, 0.0f, 0.0f});
  }
  if (info.usedIndTexMtxs.any()) {
    for (u32 i = 0; i < MaxIndTexMtxs; ++i) {
      const auto& mtx = g_gxState.indTexMtxs[i];
      buf.append(Vec4{mtx.mtx.m0.x, mtx.mtx.m0.y, mtx.mtx.m1.x, mtx.mtx.m1.y});
      buf.append(Vec4{mtx.mtx.m2.x, mtx.mtx.m2.y, std::exp2f(mtx.scaleExp), 0.0f});
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    buf.append(texture_size_bias(get_texture(static_cast<GXTexMapID>(i))));
  }
}

gfx::Range build_uniform(const ShaderInfo& info) noexcept {
  ZoneScoped;
  static ByteBuffer buf;
  buf.clear();
  fill_uniform(buf, info);
  return gfx::push_uniform(buf.data(), buf.size());
}
} // namespace aurora::gx
