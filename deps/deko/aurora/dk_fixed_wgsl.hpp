#pragma once
#include <cstring>

namespace aurora::gfx::dk {
inline const char* find_fixed_wgsl(const char* name) {
    if (std::strcmp(name, "clear_color") == 0) return R"__WGSL__(
struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    return out;
}

struct FragmentOutput {
    @location(0) color: vec4f,
};
@fragment
fn fs_main() -> FragmentOutput {
    var out: FragmentOutput;
    out.color = vec4f(1.0);
    return out;
}
)__WGSL__";
    if (std::strcmp(name, "clear_depth") == 0) return R"__WGSL__(
struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    return out;
}

@fragment
fn fs_main() {
}
)__WGSL__";
    if (std::strcmp(name, "dk_fsr_easu") == 0) return R"__WGSL__(// FSR 1 EASU (edge-adaptive spatial upsampling) of the present pass (switch/deko/aurora/dk_present.cpp,
// options menu: Gráficos > Reescalado > FSR 1, COS_RESCALE 2). Replaces present_resample when on and the
// EFB is smaller than the size the picture is shown at: it reads the EFB and writes the resampled image
// at the shown size. A port of AMD FidelityFX FSR 1.0's FsrEasuF (ffx_fsr1.h, MIT license, Copyright (c)
// 2021 Advanced Micro Devices, Inc.), with 12 point-sampled taps instead of the gathers and exact
// divisions instead of the approximate reciprocals. Compiled into initial_dksh_cache.bin as the named
// records dk_fsr_easu.vs_main and dk_fsr_easu.fs_main. Same bindings as present_resample.wgsl: uniforms 0,
// sampler 1 (linear, clamp: only ever sampled at texel centres), texture 2.

struct Uniforms {
    in_size: vec2<f32>,  // the EFB's width and height in texels
    inv_in_size: vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}


// the texel at integer offset `o` from the cell `fp`, as its colour
fn tap(fp: vec2<f32>, o: vec2<f32>) -> vec3<f32> {
    return textureSampleLevel(t, s, (fp + o + vec2<f32>(0.5, 0.5)) * uniforms.inv_in_size, 0.0).rgb;
}

fn luma(c: vec3<f32>) -> f32 {
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// the direction and length of the edge, from one of the four bilinear cells' weight w and the lumas
// above, left, centre, right and below the pixel it is about (FsrEasuSetF)
fn easu_set(dir: ptr<function, vec2<f32>>, len: ptr<function, f32>, w: f32,
            lA: f32, lB: f32, lC: f32, lD: f32, lE: f32) {
    let dc = lD - lC;
    let cb = lC - lB;
    let lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0e-8);
    let dirX = lD - lB;
    (*dir).x = (*dir).x + dirX * w;
    let sx = clamp(abs(dirX) * lenX, 0.0, 1.0);
    *len = *len + sx * sx * w;
    let ec = lE - lC;
    let ca = lC - lA;
    let lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0e-8);
    let dirY = lE - lA;
    (*dir).y = (*dir).y + dirY * w;
    let sy = clamp(abs(dirY) * lenY, 0.0, 1.0);
    *len = *len + sy * sy * w;
}

// one tap's Lanczos-like weight (FsrEasuTapF); `off` is the tap's offset from the output pixel in input texels
fn easu_tap(acc: ptr<function, vec3<f32>>, wsum: ptr<function, f32>, off: vec2<f32>, dir: vec2<f32>,
            len2: vec2<f32>, lob: f32, clp: f32, c: vec3<f32>) {
    var v = vec2<f32>(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);
    v = v * len2;
    let d2 = min(v.x * v.x + v.y * v.y, clp);
    var wB = 0.4 * d2 - 1.0;
    var wA = lob * d2 - 1.0;
    wB = wB * wB;
    wA = wA * wA;
    wB = 1.5625 * wB - 0.5625;
    let w = wB * wA;
    *acc = *acc + c * w;
    *wsum = *wsum + w;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    // the output pixel's place in the input, in texels, relative to the centre of cell f
    let pp0 = in.uv * uniforms.in_size - vec2<f32>(0.5, 0.5);
    let fp = floor(pp0);
    let pp = pp0 - fp;

    //    b c
    //  e f g h
    //  i j k l
    //    n o
    let cb = tap(fp, vec2<f32>(0.0, -1.0));
    let cc = tap(fp, vec2<f32>(1.0, -1.0));
    let ce = tap(fp, vec2<f32>(-1.0, 0.0));
    let cf = tap(fp, vec2<f32>(0.0, 0.0));
    let cg = tap(fp, vec2<f32>(1.0, 0.0));
    let ch = tap(fp, vec2<f32>(2.0, 0.0));
    let ci = tap(fp, vec2<f32>(-1.0, 1.0));
    let cj = tap(fp, vec2<f32>(0.0, 1.0));
    let ck = tap(fp, vec2<f32>(1.0, 1.0));
    let cl = tap(fp, vec2<f32>(2.0, 1.0));
    let cn = tap(fp, vec2<f32>(0.0, 2.0));
    let co = tap(fp, vec2<f32>(1.0, 2.0));
    let lb = luma(cb);
    let lc = luma(cc);
    let le = luma(ce);
    let lf = luma(cf);
    let lg = luma(cg);
    let lh = luma(ch);
    let li = luma(ci);
    let lj = luma(cj);
    let lk = luma(ck);
    let ll = luma(cl);
    let ln = luma(cn);
    let lo = luma(co);

    // the edge's direction and length: the four bilinear cells around the pixel, weighted
    var dir = vec2<f32>(0.0, 0.0);
    var len = 0.0;
    easu_set(&dir, &len, (1.0 - pp.x) * (1.0 - pp.y), lb, le, lf, lg, lj);
    easu_set(&dir, &len, pp.x * (1.0 - pp.y), lc, lf, lg, lh, lk);
    easu_set(&dir, &len, (1.0 - pp.x) * pp.y, lf, li, lj, lk, ln);
    easu_set(&dir, &len, pp.x * pp.y, lg, lj, lk, ll, lo);

    // normalise, with the flat case pointing along x
    let dirR2 = dir.x * dir.x + dir.y * dir.y;
    let zro = dirR2 < (1.0 / 32768.0);
    let dirR = select(inverseSqrt(dirR2), 1.0, zro);
    dir = vec2<f32>(select(dir.x, 1.0, zro), dir.y) * dirR;
    // length {0 to 2} -> {0 to 1}, shaped with a square
    len = len * 0.5;
    len = len * len;
    // stretch the kernel: 1 along an axis, sqrt(2) on a diagonal
    let stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    // anisotropic lengths after the rotation: x along the edge, y across it
    let len2 = vec2<f32>(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    // the window shifts from +-sqrt(2) to just beyond 2 with the amount of edge
    let lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    let clp = 1.0 / lob;

    // the four nearest texels bound the result (no ringing beyond them)
    let min4 = min(min(cf, cg), min(cj, ck));
    let max4 = max(max(cf, cg), max(cj, ck));
    var acc = vec3<f32>(0.0, 0.0, 0.0);
    var wsum = 0.0;
    easu_tap(&acc, &wsum, vec2<f32>(0.0, -1.0) - pp, dir, len2, lob, clp, cb);
    easu_tap(&acc, &wsum, vec2<f32>(1.0, -1.0) - pp, dir, len2, lob, clp, cc);
    easu_tap(&acc, &wsum, vec2<f32>(-1.0, 0.0) - pp, dir, len2, lob, clp, ce);
    easu_tap(&acc, &wsum, vec2<f32>(0.0, 0.0) - pp, dir, len2, lob, clp, cf);
    easu_tap(&acc, &wsum, vec2<f32>(1.0, 0.0) - pp, dir, len2, lob, clp, cg);
    easu_tap(&acc, &wsum, vec2<f32>(2.0, 0.0) - pp, dir, len2, lob, clp, ch);
    easu_tap(&acc, &wsum, vec2<f32>(-1.0, 1.0) - pp, dir, len2, lob, clp, ci);
    easu_tap(&acc, &wsum, vec2<f32>(0.0, 1.0) - pp, dir, len2, lob, clp, cj);
    easu_tap(&acc, &wsum, vec2<f32>(1.0, 1.0) - pp, dir, len2, lob, clp, ck);
    easu_tap(&acc, &wsum, vec2<f32>(2.0, 1.0) - pp, dir, len2, lob, clp, cl);
    easu_tap(&acc, &wsum, vec2<f32>(0.0, 2.0) - pp, dir, len2, lob, clp, cn);
    easu_tap(&acc, &wsum, vec2<f32>(1.0, 2.0) - pp, dir, len2, lob, clp, co);
    return vec4<f32>(min(max4, max(min4, acc / wsum)), 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "dk_fsr_rcas") == 0) return R"__WGSL__(// FSR 1 RCAS (robust contrast-adaptive sharpening) of the present pass (switch/deko/aurora/dk_present.cpp,
// options menu: Gráficos > Reescalado > FSR 1). Replaces xfb_copy in the final draw when FSR 1 is on: it
// reads the EASU picture, FXAA's picture when FXAA is on too, or the EFB itself when it already has the
// shown size, and writes the swapchain image in the picture's viewport. A port of AMD FidelityFX FSR 1.0's FsrRcasF (ffx_fsr1.h,
// MIT license, Copyright (c) 2021 Advanced Micro Devices, Inc.), without the optional noise removal and with
// exact divisions. Compiled into initial_dksh_cache.bin as the named records dk_fsr_rcas.vs_main and
// dk_fsr_rcas.fs_main. Same bindings as present_resample.wgsl: uniforms 0, sampler 1, texture 2.

struct Uniforms {
    inv_size: vec2<f32>,  // 1 / the picture's width and height in texels
    sharpness: vec2<f32>,  // x: the lobe's scale, 2^-stops (1 sharpest); y: the options menu's gamma exponent (present_gamma.wgsl), 1 = none
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}


fn tap(uv: vec2<f32>) -> vec3<f32> {
    return textureSampleLevel(t, s, uv, 0.0).rgb;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let px = uniforms.inv_size;
    let uv = in.uv;
    //   b
    // d e f
    //   h
    let b = tap(uv + vec2<f32>(0.0, -px.y));
    let d = tap(uv + vec2<f32>(-px.x, 0.0));
    let e = tap(uv);
    let f = tap(uv + vec2<f32>(px.x, 0.0));
    let h = tap(uv + vec2<f32>(0.0, px.y));
    // the ring's extremes
    let mn4 = min(min(min(b, d), f), h);
    let mx4 = max(max(max(b, d), f), h);
    // the largest lobe that cannot clip: towards black, towards white
    let hitMin = min(mn4, e) / max(4.0 * mx4, vec3<f32>(1.0e-5));
    let hitMax = (vec3<f32>(1.0) - max(mx4, e)) / min(4.0 * mn4 - vec3<f32>(4.0), vec3<f32>(-1.0e-5));
    let lobeC = max(-hitMin, hitMax);
    let lobe = max(-0.1875, min(max(max(lobeC.r, lobeC.g), lobeC.b), 0.0)) * uniforms.sharpness.x;
    let rcpL = 1.0 / (4.0 * lobe + 1.0);
    return vec4<f32>(pow(clamp((lobe * (b + d + h + f) + e) * rcpL, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(uniforms.sharpness.y)), 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "dk_fxaa") == 0) return R"__WGSL__(// FXAA of the present pass (switch/deko/aurora/dk_present.cpp, options menu: Gráficos > Suavizado de
// bordes, COS_FXAA). Replaces xfb_copy when on: it reads the resampled picture and writes the swapchain
// image in the picture's viewport, one full-screen triangle. Timothy Lottes' FXAA 3.11 (quality), as
// in his reference: pixels under a contrast threshold are left alone; for the rest the edge's
// orientation and its end on both sides are found by walking along it (eight taps at most, steps of
// 1 to 12 texels), and the pixel is moved across the edge by how far it lies from the nearer end
// (long diagonals and curves come out smooth, short features hardly blur), with a sub-pixel blend for
// isolated bright pixels. Compiled into initial_dksh_cache.bin like dk_test_pattern, as the
// named records dk_fxaa.vs_main and dk_fxaa.fs_main. Same bindings as present_resample.wgsl:
// uniforms 0, sampler 1, texture 2. The uniforms carry the options menu's strength (Suave, Normal, Fuerte).

struct Uniforms {
    inv_size: vec2<f32>,  // 1 / the picture's width and height in texels
    thresholds: vec2<f32>,  // the contrast an edge needs: an absolute minimum, and a share of the local maximum luma
    subpix: vec2<f32>,      // x: the sub-pixel blend's strength (0 to 1); y: the options menu's gamma exponent (present_gamma.wgsl), 1 = none
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}


fn luma(c: vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.299, 0.587, 0.114));
}

fn tap(uv: vec2<f32>) -> vec3<f32> {
    return textureSampleLevel(t, s, uv, 0.0).rgb;
}

fn tapl(uv: vec2<f32>) -> f32 {
    return luma(tap(uv));
}

fn fxaa_color(in: VertexOutput) -> vec4<f32> {
    let px = uniforms.inv_size;
    let uv = in.uv;
    let rgbM = tap(uv);
    let lM = luma(rgbM);
    let lN = tapl(uv + vec2<f32>(0.0, -px.y));
    let lS = tapl(uv + vec2<f32>(0.0, px.y));
    let lW = tapl(uv + vec2<f32>(-px.x, 0.0));
    let lE = tapl(uv + vec2<f32>(px.x, 0.0));
    let lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    let lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    let range = lMax - lMin;
    // flat or low-contrast areas (text on flat panels, soft gradients) stay as they are
    if (range < max(uniforms.thresholds.x, lMax * uniforms.thresholds.y)) {
        return vec4<f32>(rgbM, 1.0);
    }
    let lNW = tapl(uv + vec2<f32>(-px.x, -px.y));
    let lNE = tapl(uv + vec2<f32>(px.x, -px.y));
    let lSW = tapl(uv + vec2<f32>(-px.x, px.y));
    let lSE = tapl(uv + vec2<f32>(px.x, px.y));

    let lNS = lN + lS;
    let lWE = lW + lE;
    let lNWSW = lNW + lSW;
    let lNESE = lNE + lSE;
    let edgeHorz = abs(-2.0 * lW + lNWSW) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lNESE);
    let edgeVert = abs(-2.0 * lS + (lSW + lSE)) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lN + (lNW + lNE));
    let horzSpan = edgeHorz >= edgeVert;

    // across the edge: N/S for a horizontal edge, W/E for a vertical one
    let lA = select(lW, lN, horzSpan);
    let lB = select(lE, lS, horzSpan);
    let gradA = lA - lM;
    let gradB = lB - lM;
    let towardA = abs(gradA) >= abs(gradB);
    let gradient = max(abs(gradA), abs(gradB));
    var stepLen = select(px.x, px.y, horzSpan);
    if (towardA) { stepLen = -stepLen; }
    let lEdge = select(lB + lM, lA + lM, towardA);  // the pair's sum: the edge's mean is half of it

    // sub-pixel blend: how far the pixel stands out from its 3x3 average
    let blur = (2.0 * (lNS + lWE) + lNWSW + lNESE) / 12.0;
    let subpix = clamp(abs(blur - lM) / range, 0.0, 1.0);
    let subpixAmount = (-2.0 * subpix + 3.0) * subpix * subpix * uniforms.subpix.x;

    // walk along the edge both ways until its luma leaves the edge's mean by a quarter of the gradient
    var posB = uv;
    if (horzSpan) { posB.y = posB.y + stepLen * 0.5; } else { posB.x = posB.x + stepLen * 0.5; }
    let along = select(vec2<f32>(0.0, px.y), vec2<f32>(px.x, 0.0), horzSpan);
    let mean = lEdge * 0.5;
    let limit = gradient * 0.25;
    var posN = posB - along;
    var posP = posB + along;
    var endN = tapl(posN) - mean;
    var endP = tapl(posP) - mean;
    var doneN = abs(endN) >= limit;
    var doneP = abs(endP) >= limit;
    for (var i = 0; i < 7; i = i + 1) {
        if (doneN && doneP) { break; }
        let adv = select(select(select(2.0, 4.0, i == 5), 12.0, i == 6), 1.5, i == 0);
        if (!doneN) { posN = posN - along * adv; endN = tapl(posN) - mean; doneN = abs(endN) >= limit; }
        if (!doneP) { posP = posP + along * adv; endP = tapl(posP) - mean; doneP = abs(endP) >= limit; }
    }
    // distances along the edge: x for a horizontal edge, y for a vertical one
    let dstN = select(uv.y - posN.y, uv.x - posN.x, horzSpan);
    let dstP = select(posP.y - uv.y, posP.x - uv.x, horzSpan);
    let nearN = dstN < dstP;
    let endNear = select(endP, endN, nearN);
    let dstNear = select(dstP, dstN, nearN);
    // the end must lie on the side of the edge the pixel is not on, else the span does not cross
    let centreBelow = (lM - mean) < 0.0;
    let crosses = (endNear < 0.0) != centreBelow;
    let offsetEdge = select(0.0, 0.5 - dstNear / (dstN + dstP), crosses);
    let offset = max(offsetEdge, subpixAmount);
    var posM = uv;
    if (horzSpan) { posM.y = posM.y + offset * stepLen; } else { posM.x = posM.x + offset * stepLen; }
    return vec4<f32>(tap(posM), 1.0);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let c = fxaa_color(in);
    return vec4<f32>(pow(clamp(c.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(uniforms.subpix.y)), 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "dk_ssao") == 0) return R"__WGSL__(// SPDX-License-Identifier: MIT
// Light screen-space ambient occlusion of the present pass (switch/deko/aurora/dk_present.cpp; options menu:
// Gráficos > Oclusión ambiental, COS_SSAO). Three fixed shaders of this file, all one full-screen triangle:
//
//   fs_ao    reduced resolution (half or quarter, by the level): four to eight depth taps in a disc round the pixel, rotated per pixel (interleaved
//            gradient noise), on the EFB depth buffer (Depth32Float, reversed Z: 1 at the near plane, 0 at
//            the far plane and for the sky). No normals: with the perspective projection the depth value is
//            an affine function of 1/z, so it is an affine function of the screen position on a plane. The
//            plane through the pixel is taken from its depth gradient (the smaller one-sided difference of
//            each axis, so an edge does not give a false slope); a tap whose depth is above the plane (nearer
//            to the camera) occludes by how much, relative to the pixel's own depth, and no longer counts when
//            it is much nearer (a foreground object: no halo). The disc's radius is a world size
//            (proportional to the depth value, about the pixel size of 1 / z), clamped in pixels.
//   fs_blur  reduced resolution: 3x3 blur of that, each texel weighted by how near its depth is to the pixel's
//            (the sky and other objects do not bleed into it).
//   fs_apply the present resolution: the occlusion upsampled (bilinear) and drawn over the resampled picture
//            with the standard blend (colour 0, alpha = strength * occlusion): picture * (1 - alpha).
//
// Compiled into initial_dksh_cache.bin like dk_fxaa, as the named records dk_ssao.vs_main, dk_ssao.fs_ao,
// dk_ssao.fs_blur and dk_ssao.fs_apply (scripts/switch/build_native.sh checks them).

struct Uniforms {
    depth_dims: vec2<f32>,  // the depth buffer's size in texels
    out_dims: vec2<f32>,    // the target's size in texels
    // x: tap radius in depth-buffer pixels per unit of depth value (the depth value is about near / z),
    // y, z: the radius' minimum and maximum in depth-buffer pixels, w: occlusion bias (relative depth)
    radius: vec4<f32>,
    // x: strength of the final darkening (fs_apply), y: range where the occlusion fades out (relative depth),
    // z: the number of taps of fs_ao (4 to 8)
    apply: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var depth_tex: texture_depth_2d;
@group(0) @binding(2) var ao_tex: texture_2d<f32>;
@group(0) @binding(3) var ao_sampler: sampler;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

const SKY: f32 = 0.0005;  // depth values below it (the far plane, the sky) take no occlusion

fn load_depth(c: vec2<i32>) -> f32 {
    let m = vec2<i32>(u.depth_dims) - vec2<i32>(1, 1);
    return textureLoad(depth_tex, clamp(c, vec2<i32>(0, 0), m), 0);
}

@fragment
fn fs_ao(in: VertexOutput) -> @location(0) vec4<f32> {
    let c = vec2<i32>(in.uv * u.depth_dims);
    let dc = load_depth(c);
    if (dc < SKY) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    // the plane's slope per depth-buffer pixel: the smaller of the two one-sided differences
    let dxp = load_depth(c + vec2<i32>(2, 0)) - dc;
    let dxm = dc - load_depth(c - vec2<i32>(2, 0));
    let dyp = load_depth(c + vec2<i32>(0, 2)) - dc;
    let dym = dc - load_depth(c - vec2<i32>(0, 2));
    let gx = select(dxp, dxm, abs(dxm) < abs(dxp)) * 0.5;
    let gy = select(dyp, dym, abs(dym) < abs(dyp)) * 0.5;

    let r = clamp(u.radius.x * dc, u.radius.y, u.radius.z);
    let noise = fract(52.9829189 * fract(dot(in.position.xy, vec2<f32>(0.06711056, 0.00583715))));
    let bias = u.radius.w;
    var occ = 0.0;
    let taps = i32(u.apply.z);
    let inv_taps = 1.0 / u.apply.z;
    for (var i = 0; i < taps; i = i + 1) {
        let a = f32(i) * 2.39996323 + noise * 6.2831853;
        let rad = sqrt((f32(i) + 0.5) * inv_taps) * r;
        let o = vec2<i32>(round(vec2<f32>(cos(a), sin(a)) * rad));
        let ds = load_depth(c + o);
        let dp = dc + gx * f32(o.x) + gy * f32(o.y);
        let dz = (ds - dp) / dc;
        occ = occ + smoothstep(bias, bias * 4.0, dz) * (1.0 - smoothstep(u.apply.y, u.apply.y * 2.4, dz));
    }
    return vec4<f32>(occ * inv_taps, 0.0, 0.0, 1.0);
}

@fragment
fn fs_blur(in: VertexOutput) -> @location(0) vec4<f32> {
    let h = vec2<i32>(in.uv * u.out_dims);
    let sd = u.depth_dims / u.out_dims;  // depth-buffer pixels per target texel
    let hm = vec2<i32>(u.out_dims) - vec2<i32>(1, 1);
    let dc = load_depth(vec2<i32>((vec2<f32>(h) + vec2<f32>(0.5, 0.5)) * sd));
    var sum = 0.0;
    var wsum = 0.0;
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let t = clamp(h + vec2<i32>(x, y), vec2<i32>(0, 0), hm);
            let d = load_depth(vec2<i32>((vec2<f32>(t) + vec2<f32>(0.5, 0.5)) * sd));
            let w = (select(1.0, 2.0, x == 0 && y == 0) + select(0.0, 0.5, x == 0 || y == 0)) *
                    max(0.0, 1.0 - abs(d - dc) / (max(dc, SKY) * 0.06));
            sum = sum + textureLoad(ao_tex, t, 0).r * w;
            wsum = wsum + w;
        }
    }
    return vec4<f32>(sum / max(wsum, 0.0001), 0.0, 0.0, 1.0);
}

@fragment
fn fs_apply(in: VertexOutput) -> @location(0) vec4<f32> {
    let occ = textureSampleLevel(ao_tex, ao_sampler, in.uv, 0.0).r;
    return vec4<f32>(0.0, 0.0, 0.0, clamp(occ * u.apply.x, 0.0, 1.0));
}
)__WGSL__";
    if (std::strcmp(name, "dk_test_pattern") == 0) return R"__WGSL__(// SPDX-License-Identifier: MIT
// The deko3d test pattern (switch/deko/pattern.cpp, COS_DK_TEST_PATTERN=1). Written in WGSL with
// WebGPU's conventions (clip-space y up, z from 0 to 1, texture row 0 at v = 0) and compiled the way
// Aurora's shaders are: native/tools/dksh_cache (Tint's GLSL writer with the deko3d options and the
// post-pass, then uam) into initial_dksh_cache.bin as the named records dk_test_pattern.vs_main and
// dk_test_pattern.fs_main. Vertices are pulled from a storage buffer by vertex_index, as Aurora's GX
// vertex shaders pull theirs, so one picture checks the whole path phase 3 uses.

struct Vertex {
    pos: vec4f,    // clip space (w = 1)
    color: vec4f,
    uv: vec4f,     // xy: texture coordinates; z: 1 = the texture's colour, 0 = the vertex colour
};

@group(0) @binding(0) var<storage, read> verts: array<Vertex>;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec4f,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    let v = verts[index];
    var out: VertexOutput;
    out.pos = v.pos;
    out.color = v.color;
    out.uv = v.uv;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let t = textureSample(tex, samp, in.uv.xy);
    return mix(in.color, t, in.uv.z);
}
)__WGSL__";
    if (std::strcmp(name, "present_gamma") == 0) return R"__WGSL__(// SPDX-License-Identifier: MIT
// The present pass's picture with the options menu's gamma (Gráficos > Gamma; dk_present.cpp): Aurora's
// xfb_copy (webgpu/gpu.cpp create_copy_pipeline, fs_opaque) plus colour^exponent, the one place the
// output is finalized. The game's colours reach the swapchain as they are (RGBA8 Unorm, no sRGB
// conversion anywhere), so they are encoded for a GameCube TV (CRT, about 2.4) and the Switch's panel
// (about 2.2) shows them too bright: an exponent above 1 (2.4 / 2.2) darkens them back. Exponent 1 is not drawn with this
// shader at all (xfb_copy is). ImGui is drawn after this pass, so the menus are not changed.
// Bindings as xfb_copy's (sampler 0, texture 1) plus the uniform at 2; compiled like the test pattern:
// native/tools/dksh_cache, records present_gamma.vs_main and present_gamma.fs_main.

struct Params {
    exponent: vec4<f32>,  // x: the exponent
};

@group(0) @binding(0)
var efb_sampler: sampler;
@group(0) @binding(1)
var efb_texture: texture_2d<f32>;
@group(0) @binding(2)
var<uniform> params: Params;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    return vec4(pow(clamp(color.rgb, vec3(0.0), vec3(1.0)), vec3(params.exponent.x)), 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "present_resample") == 0) return R"__WGSL__(
struct Uniforms {
    sampler_mode: u32,
    frame_width: f32,
    frame_height: f32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var t: texture_2d<f32>;

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.position = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

// BEGIN AREA SAMPLER REGION

fn sample_by_pixel(pixel: vec2<i32>) -> vec4<f32> {
    let source_dims = textureDimensions(t);
    let max_coord = vec2<i32>(source_dims) - vec2<i32>(1, 1);
    let coord = clamp(pixel, vec2<i32>(0, 0), max_coord);
    return textureLoad(t, coord, 0);
}

fn sample_area(frag_position: vec4<f32>) -> vec4<f32> {
    let source_size = vec2<f32>(textureDimensions(t));
    let target_size = max(vec2<f32>(uniforms.frame_width, uniforms.frame_height), vec2<f32>(1.0, 1.0));

    let source_min = clamp((frag_position.xy - vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;
    let source_max = clamp((frag_position.xy + vec2<f32>(0.5, 0.5)) / target_size,
                           vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0)) * source_size;

    let first_pixel = vec2<i32>(floor(source_min));
    let last_pixel = vec2<i32>(ceil(source_max));
    let max_iterations: i32 = 16;

    var avg_color = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    var total_weight = 0.0;

    for (var iy: i32 = 0; iy < max_iterations; iy = iy + 1) {
        let source_y = first_pixel.y + iy;
        if (source_y < last_pixel.y) {
            let y0 = f32(source_y);
            let weight_y = max(min(source_max.y, y0 + 1.0) - max(source_min.y, y0), 0.0);

            for (var ix: i32 = 0; ix < max_iterations; ix = ix + 1) {
                let source_x = first_pixel.x + ix;
                if (source_x < last_pixel.x) {
                    let x0 = f32(source_x);
                    let weight_x = max(min(source_max.x, x0 + 1.0) - max(source_min.x, x0), 0.0);
                    let weight = weight_x * weight_y;
                    avg_color += weight * sample_by_pixel(vec2<i32>(source_x, source_y));
                    total_weight += weight;
                }
            }
        }
    }

    return avg_color / max(total_weight, 0.000001);
}

// END AREA SAMPLER REGION

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    var color = textureSample(t, s, in.uv);
    if (uniforms.sampler_mode == 1u) {
        color = sample_area(in.position);
    }
    return vec4(color.rgb, 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_a8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let a = sample_efb(in.uv).a;
    return vec4f(a, a, a, a);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_b8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let b = sample_efb(in.uv).b;
    return vec4f(b, b, b, b);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_blit") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return sample_efb(in.uv);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_depth_snapshot") == 0) return R"__WGSL__(
@group(0) @binding(0) var src: texture_depth_2d;

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
    return vec4f(positions[vi], 0.0, 1.0);
}

@fragment fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let depth = textureLoad(src, vec2i(pos.xy), 0);
    return vec4f(depth, 0.0, 0.0, 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_depth_snapshot_ms") == 0) return R"__WGSL__(
@group(0) @binding(0) var src: texture_depth_multisampled_2d;

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
    return vec4f(positions[vi], 0.0, 1.0);
}

@fragment fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let depth = textureLoad(src, vec2i(pos.xy), 0);
    return vec4f(depth, 0.0, 0.0, 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_g8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let g = sample_efb(in.uv).g;
    return vec4f(g, g, g, g);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_gb8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    return vec4f(c.g, c.g, c.g, c.b);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_i4") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let rgb = sample_efb(in.uv).rgb;
    let i = quantize4(intensity(rgb));
    return vec4f(i, i, i, i);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_i8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let rgb = sample_efb(in.uv).rgb;
    let i = intensity(rgb);
    return vec4f(i, i, i, i);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_ia4") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    let i = quantize4(intensity(c.rgb));
    let a = quantize4(c.a);
    return vec4f(i, i, i, a);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_ia8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    let i = intensity(c.rgb);
    return vec4f(i, i, i, c.a);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_r4") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let r = quantize4(sample_efb(in.uv).r);
    return vec4f(r, r, r, r);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_r8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let r = sample_efb(in.uv).r;
    return vec4f(r, r, r, r);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_ra4") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    let r = quantize4(c.r);
    return vec4f(r, r, r, quantize4(c.a));
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_ra8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    return vec4f(c.r, c.r, c.r, c.a);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_rg8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    return vec4f(c.r, c.r, c.r, c.g);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_rgb565") == 0) return R"__WGSL__(
@group(0) @binding(0) var src_samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;

struct Uniforms {
    offset: vec2f,
    scale: vec2f,
    opaqueAlpha: u32,
};
@group(0) @binding(2) var<uniform> ubuf: Uniforms;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * ubuf.scale + ubuf.offset;
    return out;
}

fn sample_efb(uv: vec2f) -> vec4f {
    let c = textureSample(src, src_samp, uv);
    return vec4f(c.rgb, select(c.a, 1.0, ubuf.opaqueAlpha != 0u));
}

fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

fn quantize4(v: f32) -> f32 {
    return floor(v * 16.0) / 15.0;
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = sample_efb(in.uv);
    return vec4f(c.rgb, 1.0);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_z16") == 0) return R"__WGSL__(
@group(0) @binding(0) var src: texture_depth_2d;

struct UVTransform {
    offset: vec2f,
    scale: vec2f,
};
@group(0) @binding(1) var<uniform> uv_xf: UVTransform;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * uv_xf.scale + uv_xf.offset;
    return out;
}

fn gx_z24(uv: vec2f) -> u32 {
    let texSize = vec2i(textureDimensions(src));
    let coord = clamp(vec2i(floor(uv * vec2f(texSize))), vec2i(0), texSize - vec2i(1));
    let depth = textureLoad(src, coord, 0);
    return min(u32(clamp(1.0 - depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let z16 = gx_z24(in.uv) >> 8u;
    let i = f32((z16 >> 8u) & 0xFFu) / 255.0;
    let a = f32(z16 & 0xFFu) / 255.0;
    return vec4f(i, i, i, a);
}
)__WGSL__";
    if (std::strcmp(name, "tex_copy_conv_z8") == 0) return R"__WGSL__(
@group(0) @binding(0) var src: texture_depth_2d;

struct UVTransform {
    offset: vec2f,
    scale: vec2f,
};
@group(0) @binding(1) var<uniform> uv_xf: UVTransform;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

var<private> positions: array<vec2f, 3> = array(
    vec2f(-1.0, 1.0),
    vec2f(-1.0, -3.0),
    vec2f(3.0, 1.0),
);
var<private> uvs: array<vec2f, 3> = array(
    vec2f(0.0, 0.0),
    vec2f(0.0, 2.0),
    vec2f(2.0, 0.0),
);

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4f(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi] * uv_xf.scale + uv_xf.offset;
    return out;
}

fn gx_z24(uv: vec2f) -> u32 {
    let texSize = vec2i(textureDimensions(src));
    let coord = clamp(vec2i(floor(uv * vec2f(texSize))), vec2i(0), texSize - vec2i(1));
    let depth = textureLoad(src, coord, 0);
    return min(u32(clamp(1.0 - depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}

@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let z8 = f32((gx_z24(in.uv) >> 16u) & 0xFFu) / 255.0;
    return vec4f(z8, z8, z8, z8);
}
)__WGSL__";
    if (std::strcmp(name, "xfb_copy") == 0) return R"__WGSL__(
@group(0) @binding(0)
var efb_sampler: sampler;
@group(0) @binding(1)
var efb_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4<f32>(pos[vtxIdx], 0.0, 1.0);
    out.uv = uvs[vtxIdx];
    return out;
}

@fragment
fn fs_opaque(in: VertexOutput) -> @location(0) vec4<f32> {
    let color = textureSample(efb_texture, efb_sampler, in.uv);
    return vec4(color.rgb, 1.0);
}

@fragment
fn fs_premultiplied_alpha(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(efb_texture, efb_sampler, in.uv);
}
)__WGSL__";
    return nullptr;
}
}  // namespace aurora::gfx::dk
