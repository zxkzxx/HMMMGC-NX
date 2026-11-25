#ifndef AURORA_TEXTURE_HPP
#define AURORA_TEXTURE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace aurora::texture {

/// Wildcard hash values used by the replacement filename convention ("$" fields).
inline constexpr uint64_t kWildcardTextureHash = 0xFFFFFFFFFFFFFFFFull;
inline constexpr uint64_t kWildcardTlutHash = 0xFFFFFFFFFFFFFFFEull;

struct TextureSourceKey {
  uint64_t textureHash = 0;
  uint64_t tlutHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  bool hasTlut = false;

  bool operator==(const TextureSourceKey&) const = default;
};

struct TexturePointerKey {
  const void* data = nullptr;

  bool operator==(const TexturePointerKey&) const = default;
};

using ReplacementKey = std::variant<TexturePointerKey, TextureSourceKey>;

struct RawTextureReplacement {
  std::span<const uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 1;
  uint32_t gxFormat = 0;
  std::string_view label = {};
};

struct ReplacementOptions {
  int32_t priority = 0;
};

struct ReplacementRegistration {
  uint64_t id = 0;
  ReplacementKey key;
};

struct ReplacementGroup {
  std::vector<ReplacementRegistration> registrations;
};

/// Parses a replacement filename of the form "tex1_{w}x{h}[_m]_{texhash}[_{tluthash}]_{fmt}[_arb].dds|.png"
/// into the source key it addresses. Hash fields may be "$" (see the wildcard constants above).
/// Returns nullopt if the filename is invalid.
std::optional<TextureSourceKey> parse_replacement_filename(std::string_view filename) noexcept;

/// Callback source for replacement files that do not live on the filesystem.
///
/// read() loads the entire encoded file at `path` into outBytes, returning false if the file is
/// unavailable. It must be thread safe: it is invoked lazily from Aurora worker threads without
/// internal registry locks held. `userData` must remain valid until unregistration returns;
/// unregistration may wait for one in-flight read callback to finish.
///
/// The callback must not call back into aurora::texture.
struct VirtualFileSource {
  bool (*read)(void* userData, const char* path, std::vector<uint8_t>& outBytes) = nullptr;
  void* userData = nullptr;
};

/// Registers an encoded (.dds/.png) replacement served through callbacks instead of the filesystem.
/// `path` is a relative '/'-separated virtual path whose final component follows the replacement
/// filename convention and determines the key. Mip sidecars are probed lazily by deriving
/// "{stem}_mipN{ext}" from `path` and calling source.read for each level until it returns false.
///
/// Returns ID 0 (invalid) if the filename does not parse or source.read is null.
ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,
                                                     ReplacementOptions options = {});

/// Registers many virtual replacements at once (cos_native patch 0011): one registry lock, one read
/// state shared by the batch (an unregistration cancels the reads of the whole batch), no
/// per-entry thumbnail queueing. `rescan` asks the texture-object cache to look the static
/// textures up again gradually (ReplacementRuntimeConfig::rescanBudgetBytesPerFrame) instead of
/// clearing it: pass false for all but the last batch of a pack registered over several frames.
/// Returns one registration per path, ID 0 where the filename does not parse.
std::vector<ReplacementRegistration> register_virtual_replacements(std::span<const std::string_view> paths,
                                                                   VirtualFileSource source,
                                                                   ReplacementOptions options = {},
                                                                   bool rescan = true);

ReplacementRegistration register_replacement(ReplacementKey key, RawTextureReplacement replacement,
                                             ReplacementOptions options = {});
void unregister_replacement(const ReplacementRegistration& registration);
void unregister_replacements(std::span<const ReplacementRegistration> registrations);
void unregister_replacements(const ReplacementGroup& group);
/// Like unregister_replacements, but the texture objects that use a removed replacement keep it
/// until the gradual rescan (rescanBudgetBytesPerFrame) looks them up again, so turning a pack off
/// does not re-resolve (and re-upload) every original texture in one frame. `rescan`: false for
/// all but the last batch of a pack removed over several frames.
void unregister_replacements_gradual(std::span<const ReplacementRegistration> registrations, bool rescan = true);
/// Queues a new full load of every cached (fully loaded) file or virtual replacement for which
/// `predicate(userData, path, width, height, mips)` returns true, with the size of the cached GPU
/// texture; the cached texture stays in use until the new load is published (e.g. after the
/// source changed the size it serves). Returns the number queued. Called on the GX thread.
size_t reload_cached_replacements(bool (*predicate)(void* userData, std::string_view path, uint32_t width,
                                                    uint32_t height, uint32_t mips),
                                  void* userData);
void unregister_replacements(const ReplacementKey& key);
void clear_replacements();

ReplacementGroup load_replacement_directory(const std::filesystem::path& root, ReplacementOptions options = {});
void reload_replacement_directory(const std::filesystem::path& root, ReplacementGroup& group,
                                  ReplacementOptions options = {});

bool has_replacement(const GXTexObj* obj, const GXTlutObj* tlut = nullptr);

/// Runtime tuning of the replacement streamer (cos_native patch 0010). Call before registering
/// replacements; the worker count only applies before the first load starts the worker pool.
struct ReplacementRuntimeConfig {
  /// GPU bytes the replacement cache may hold. Entries bound within the last `keepRecentFrames`
  /// frames are not evicted; a load that cannot fit is dropped (the original texture stays) and
  /// retried after `retryFrames` frames.
  uint64_t cacheBudgetBytes = 4294967296ull;
  /// Upload bytes published per frame (at least one texture is always published).
  uint64_t publishBudgetBytesPerFrame = 12ull * 1024ull * 1024ull;
  /// Queue a low-priority mip-tail ("thumbnail") read of every DDS entry at registration. Off: the
  /// original texture is shown until the full replacement has loaded.
  bool eagerThumbnails = true;
  /// Loader threads; 0 = half the hardware threads, 1..4.
  uint32_t workerCount = 0;
  uint32_t keepRecentFrames = 60;
  uint32_t retryFrames = 300;
  /// Time budget of one frame's publishing (cos_native patch 0011), in an estimate of the GPU-side
  /// work (texture creation and upload, which a deferred GL backend runs later on its own thread):
  /// publishCostNsPerTexture + levels * publishCostNsPerLevel + MiB * publishCostNsPerMiB per
  /// texture. 0: only the byte budget. At least one texture is always published.
  uint64_t publishBudgetNsPerFrame = 0;
  uint64_t publishCostNsPerTexture = 0;
  uint64_t publishCostNsPerLevel = 0;
  uint64_t publishCostNsPerMiB = 0;
  /// Source bytes of static textures the texture-object cache may look up again per frame after a
  /// gradual rescan request (register_virtual_replacements, unregister_replacements_gradual); at
  /// least one texture per frame. 0: no limit.
  uint64_t rescanBudgetBytesPerFrame = 0;
};
void set_runtime_config(const ReplacementRuntimeConfig& config) noexcept;
ReplacementRuntimeConfig runtime_config() noexcept;

/// Cumulative counters since start (or the last reset_stats), plus current cache state.
struct ReplacementStats {
  uint64_t lookupHits = 0;      ///< texture resolves whose source key has a replacement
  uint64_t lookupMisses = 0;    ///< texture resolves whose source key has none
  uint64_t loadsCompleted = 0;  ///< full loads decoded by the workers
  uint64_t loadsFailed = 0;
  uint64_t loadedBytes = 0;     ///< decoded texture bytes (all mips) of the completed loads
  uint64_t loadNsTotal = 0;     ///< worker time spent in the completed and failed loads
  uint64_t loadNsMax = 0;
  uint64_t publishes = 0;       ///< GPU textures created from completed loads
  uint64_t publishedBytes = 0;
  uint64_t evictions = 0;
  uint64_t budgetRejects = 0;   ///< completed loads dropped because the cache budget was full
  uint64_t cacheBytes = 0;      ///< current
  uint64_t cacheEntries = 0;    ///< current
  uint64_t pendingLoads = 0;    ///< current
  uint64_t registered = 0;      ///< current source-key registrations
  // cos_native patch 0011: the GX thread's publishing, per frame.
  uint64_t publishNsTotal = 0;     ///< time spent publishing (texture creation calls, upload copies)
  uint64_t publishNsMax = 0;       ///< ... in the slowest frame
  uint64_t publishEstNsMax = 0;    ///< the largest estimated cost published in one frame
  uint64_t publishEstNsTotal = 0;  ///< the estimated cost of everything published
  uint64_t publishMaxPerFrame = 0; ///< the most textures published in one frame
  uint64_t publishFramesLimited = 0; ///< frames that left ready textures for later (time or bytes)
  uint64_t reloadsQueued = 0;      ///< reload_cached_replacements loads
};
ReplacementStats stats() noexcept;
void reset_stats() noexcept;

/// Called with every source key the runtime computes for a static texture (a texture-object cache
/// miss), before the replacement lookup; for census tools. Setting an observer makes the runtime
/// compute source keys even with no replacement registered. Called on the GX thread.
using SourceKeyObserver = void (*)(const TextureSourceKey& key, void* userData);
void set_source_key_observer(SourceKeyObserver observer, void* userData) noexcept;

/// The texture-object cache's lookups (cos_native patch 0011), cumulative: content hashing on
/// cache misses (the GX thread), the slowest frame's hashing, texture objects looked up again by a
/// gradual rescan, and whether a rescan is still in progress.
struct ResolveStats {
  uint64_t hashNsTotal = 0;
  uint64_t hashNsMaxFrame = 0;
  uint64_t hashedBytesTotal = 0;
  uint64_t rescanned = 0;
  uint64_t rescanFrames = 0;
  bool rescanActive = false;
};
ResolveStats resolve_stats() noexcept;
void reset_resolve_stats() noexcept;

} // namespace aurora::texture

#endif
