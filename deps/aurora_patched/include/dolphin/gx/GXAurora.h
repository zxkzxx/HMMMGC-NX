#ifndef DOLPHIN_GXAURORA_H
#define DOLPHIN_GXAURORA_H

#include <dolphin/types.h>

#if __cplusplus
extern "C" {
#endif

//
// Subcommands for GX_AURORA.
//

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Must be followed by six f32 values: left, top, width, height, nearz, farz.
 */
#define GX_AURORA_LOAD_VIEWPORT_RENDER 0x0001

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Must be followed by four u32 values: left, top, width, height.
 */
#define GX_AURORA_LOAD_SCISSOR_RENDER 0x0002

/**
 * Loads a full 4x4 projection matrix, bypassing GXSetProjection's 6-parameter
 * hardware encoding. Must be followed by sixteen f32 values in row-major order.
 */
#define GX_AURORA_LOAD_PROJECTION_FULL 0x0003

/**
 * Aurora equivalent of CP_REG_ARRAYBASE_ID: sets the base address and size of a vertex array.
 * This command must be followed by a 64-bit memory address, 32-bit size, and 1-byte little-endian flag.
 * The index of the vertex array is given by the lowest 4 bits of the command ID,
 * e.g. writing GX_AURORA_LOAD_ARRAYBASE + 5 will set the vertex array for the sixth vertex attribute.
 * To set strides, use the normal CP_REG_ARRAYSTRIDE_ID register.
 */
#define GX_AURORA_LOAD_ARRAYBASE 0x0010

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
#define GX_AURORA_DEBUG_GROUP_PUSH 0x0020

/**
 * Pops a previously pushed debug group.
 * Followed by nothing.
 */
#define GX_AURORA_DEBUG_GROUP_POP 0x0021

/**
 * Sends a debug marker to the backend graphics API.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 */
#define GX_AURORA_DEBUG_MARKER_INSERT 0x0022

#define GX_AURORA_LOAD_TEXOBJ 0x0030

#define GX_AURORA_LOAD_TLUT 0x0031

#define GX_AURORA_DESTROY_TEXOBJ 0x0032

#define GX_AURORA_DESTROY_TLUT 0x0033

#define GX_AURORA_DESTROY_COPY_TEX 0x0034

#define GX_AURORA_LOAD_COPY_SRC 0x0035

#define GX_AURORA_LOAD_COPY_DST 0x0036

#define GX_AURORA_LOAD_COPY_DEST 0x0037

#define GX_AURORA_REQUEST_DEPTH_SNAPSHOT 0x0038

#define GX_AURORA_BEGIN_OFFSCREEN 0x0039

#define GX_AURORA_END_OFFSCREEN 0x003A

/**
 * Draw primitives with the vertex count derived from a byte length, as written by
 * GXBegin(prim, fmt, GX_AUTO). Must be followed by a u8 draw opcode (vtxfmt|prim),
 * a u32 vertex data byte length, then that many bytes of vertex data. The byte length
 * must be a whole multiple of the current vertex size or zero (no draw).
 */
#define GX_AURORA_DRAW_SIZED 0x0040

/**
 * Draw pre-merged triangles with a prebuilt index buffer, as written by the display
 * list optimizer (aurora::gx::dl::optimize). Must be followed by a u8 draw opcode
 * (vtxfmt | GX_TRIANGLES), a u16 vertex count, a u32 index count, that many u16
 * indices, then vertex count * vertex size bytes of packed vertex data. Index data
 * is always host-endian regardless of stream endianness.
 */
#define GX_AURORA_DRAW_INDEXED 0x0041

/**
 * Sets the EFB content scale (native port, native/patches/aurora/0009): logical coordinates of the
 * EFB (viewport, scissor, EFB copy source and destination size) map to the top-left
 * scale x scale part of the EFB instead of all of it, so the following drawing renders at a
 * lower resolution. Followed by one f32 in (0, 1]; 1 restores the full EFB. Offscreen targets
 * are not affected.
 */
#define GX_AURORA_SET_CONTENT_SCALE 0x0050

/**
 * View delta for an interpolated repaint (native port, native/patches/aurora/0019): followed by a
 * u8. 0 turns it off. 1 is followed by 12 f32 (C, a 3x4 row-major rigid transform), 6 f32 (the
 * perspective projection it applies to, GXSetProjection's encoding: p[0][0], p[0][2], p[1][1],
 * p[1][2], p[2][2], p[2][3]) and 6 f32 (the projection used instead). While on, a draw whose
 * projection is that perspective projection is drawn with every position matrix premultiplied by C,
 * every normal matrix and light direction by C's rotation, light positions by C, and the other
 * projection; any other draw (orthographic, another perspective) is unchanged.
 */
#define GX_AURORA_SET_VIEW_DELTA 0x0051

#define GX2_SET_POLYGON_OFFSET 0x1000


/*
 * Debug marker stuff
 */

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
void GXPushDebugGroup(const char* label);

/**
 * Pop a debug group previously pushed via GXPushDebugGroup().
 */
void GXPopDebugGroup();

/**
 * Sends a debug marker to the backend graphics API. These may show in debugging tools such as RenderDoc.
 */
void GXInsertDebugMarker(const char* label);

typedef enum _AuroraViewportPolicy {
  AURORA_VIEWPORT_FIT = 0,     // Preserve logical aspect in the content framebuffer
  AURORA_VIEWPORT_STRETCH = 1, // Match content framebuffer aspect to the native surface
  AURORA_VIEWPORT_NATIVE = 2,  // Use active framebuffer pixels directly
} AuroraViewportPolicy;

/**
 * Configures content framebuffer sizing and how GXSetViewport/GXSetScissor parameters are applied to rendering.
 * When AURORA_VIEWPORT_NATIVE is used, GXSetTexCopySrc/GXSetTexCopyDst will use native framebuffer resolution.
 */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy);

/**
 * Sets the aspect ratio (width / height) that AURORA_VIEWPORT_FIT keeps for the content framebuffer.
 * 0 (the default) keeps the aspect of the configured render mode (fbWidth x efbHeight, 4:3 for 640x480).
 * A wider value presents the logical framebuffer stretched to it, the way a widescreen TV shows an
 * anamorphic picture; viewports, scissors and copies scale as they do for any content size.
 */
void AuroraSetFitAspect(float aspect);

/**
 * Retrieves the current content framebuffer size.
 */
void AuroraGetRenderSize(u32* width, u32* height);

/**
 * Flush pending GX state and wait for FIFO processing without signaling a draw-done callback.
 */
void AuroraGXSync(void);

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetViewport.
 */
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetScissor.
 */
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht);

void GX2SetPolygonOffset(f32 mFrontOffset, f32 mFrontScale, f32 mBackOffset, f32 mBackScale, f32 mClamp);

/**
 * Load an arbitrary 4x4 projection matrix, avoiding the 6-parameter hardware encoding.
 */
void GXSetProjectionFull(const void* mtx);

/**
 * Renders what follows into the top-left `scale` fraction of the EFB (GX_AURORA_SET_CONTENT_SCALE);
 * the current viewport and scissor are mapped again. 1 = the whole EFB.
 */
void AuroraSetContentScale(f32 scale);

/**
 * GX_AURORA_SET_VIEW_DELTA, recorded in the command stream (so a FIFO worker applies it to the draws
 * written after it). delta: a 3x4 row-major matrix, or NULL to turn it off (then the other two are
 * ignored). projFrom / projTo: six floats each in GXSetProjection's perspective encoding.
 */
void AuroraSetViewDelta(const f32* delta, const f32* projFrom, const f32* projTo);

/**
 * Create an offscreen framebuffer and switch rendering to it.
 * All subsequent GX rendering will target this framebuffer until GXRestoreFrameBuffer() is called.
 * Use GXCopyTex to resolve the offscreen content into a texture.
 */
void GXCreateFrameBuffer(u32 width, u32 height);

/**
 * Restore rendering to the main EFB framebuffer.
 * Must be called after GXCreateFrameBuffer() to resume normal rendering.
 */
void GXRestoreFrameBuffer(void);

#if __cplusplus
}
#endif

#endif
