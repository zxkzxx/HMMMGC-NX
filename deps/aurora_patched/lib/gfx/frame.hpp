#pragma once

#include "resources.hpp"
#include "frame_packet.hpp"

#include <optional>

namespace aurora::gfx::detail {

inline constexpr size_t FrameSlotCount = 2;
#if defined(AURORA_GFX_DEKO3D)
// the deko3d NRO: the staging buffers are GPU memory read in place (switch/deko/aurora), one more than
// the frame slots so that the FIFO thread records a frame while the GPU reads the two before it
inline constexpr size_t StagingBufferCount = FrameSlotCount + 1;
#else
inline constexpr size_t StagingBufferCount = FrameSlotCount + 3;
#endif
inline constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize +
                                              StorageBufferSize + (UseTextureBuffer ? TextureUploadSize : 0);

const wgpu::Buffer& staging_buffer(size_t slot);

struct RegisteredDrawType {
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

struct RegisteredEncoderTaskType {
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
};

std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id);
std::optional<RegisteredEncoderTaskType> find_runtime_encoder_task_type(EncoderTaskId id);

} // namespace aurora::gfx::detail

namespace aurora::gfx {
void initialize();
void shutdown();
bool begin_frame();
void end_frame(EndFrameCallback callback);
uint32_t current_frame() noexcept;
void after_submit() noexcept;
void gpu_synchronize();
void after_present() noexcept;
float calculate_fps() noexcept;
} // namespace aurora::gfx
