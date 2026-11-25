#pragma once
#include_next <dolphin/gx/GXGeometry.h>
#ifdef __cplusplus
extern "C++" {
inline void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride) {
  ::GXSetArray(attr, data, size, stride, false);
}
}
#endif
