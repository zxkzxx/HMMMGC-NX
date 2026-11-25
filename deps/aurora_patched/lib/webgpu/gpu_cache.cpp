#include <cstring>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <filesystem>
#include <vector>

#include "../internal.hpp"
#include "../io.hpp"
#include "../sqlite_utils.hpp"

#include <sqlite3.h>
#include <fmt/format.h>
#if defined(AURORA_CACHE_USE_ZSTD)
#include <zstd.h>
#endif
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace aurora::webgpu {
static Module Log("aurora::gpu::cache");

static sqlite3* db;
static sqlite3_stmt* load_stmt;
static sqlite3_stmt* store_stmt;
static bool cache_broken;
// Times this run deleted a damaged cache and started an empty one (cache_recover).
static int cache_resets;
static sqlite::ErrorLimiter cache_errors;
static std::mutex cache_mutex;
static std::vector<XXH128_hash_t> cache_keys_used;
#if defined(AURORA_CACHE_USE_ZSTD)
static std::vector<uint8_t> compress_buffer;
#endif

constexpr int CACHE_SCHEMA = 2;
// % of rows pruned to trigger a full VACUUM
constexpr uint64_t VacuumPrunePercentThreshold = 25;

static std::filesystem::path cache_path() {
#if defined(AURORA_GFX_DEKO3D)
  // the GL NRO's file would be pruned to this NRO's keys
  return io::fs_path_from_string(g_config.cachePath) / "dawn_cache_dk.db";
#else
  return io::fs_path_from_string(g_config.cachePath) / "dawn_cache.db";
#endif
}

// Finalizes the statements and closes the connection (sqlite3_close refuses while statements are
// left, which would keep the file open).
static void cache_close() {
  if (load_stmt != nullptr) {
    sqlite3_finalize(load_stmt);
    load_stmt = nullptr;
  }
  if (store_stmt != nullptr) {
    sqlite3_finalize(store_stmt);
    store_stmt = nullptr;
  }
  if (db != nullptr) {
    sqlite3_close(db);
    db = nullptr;
  }
}

static void init_abort() {
  cache_broken = true;
  cache_close();
}

// Resets a statement and drops its bindings when the scope ends, on every path: a statement left
// busy after an error makes every later bind fail with SQLITE_MISUSE.
struct StatementScope {
  sqlite3_stmt* stmt;
  ~StatementScope() {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }
};

// A transaction that rolls back unless committed, without logging (the caller reports failures
// through cache_failed, rate-limited).
struct CacheTransaction {
  int rc;
  bool active;
  explicit CacheTransaction(const char* begin) : rc(sqlite::exec(db, begin)), active(rc == SQLITE_OK) {}
  ~CacheTransaction() {
    if (active) {
      sqlite::exec(db, "ROLLBACK");
    }
  }
  int commit() {
    rc = sqlite::exec(db, "COMMIT");
    if (rc == SQLITE_OK) {
      active = false;
    }
    return rc;
  }
};

// Why the last open failed, kept for one summary line (cache_init) instead of a line per step.
static std::string cache_open_error;

template <typename... T>
static void open_error(fmt::format_string<T...> fmt, T&&... args) {
  if (cache_open_error.empty()) {
    cache_open_error = fmt::format(fmt, std::forward<T>(args)...);
  }
}

// Returns SQLITE_OK or the error (taken before the rollback, which clears it).
static int ensure_schema_up_to_date() {
  CacheTransaction tx("BEGIN IMMEDIATE");
  if (!tx.active) {
    open_error("Failed to open schema check transaction: {}", sqlite3_errmsg(db));
    return tx.rc;
  }

  auto ret = sqlite::exec(db, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
  if (ret != SQLITE_OK) {
    open_error("Failed to create schema table: {}", sqlite3_errmsg(db));
    return ret;
  }

  bool match = false;
  auto cmd = fmt::format("SELECT * FROM aurora_schema WHERE value = {}", CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str(), [&match](int, char**, char**) { match = true; }, nullptr);
  if (ret != SQLITE_OK) {
    open_error("Failed to check schema table: {}", sqlite3_errmsg(db));
    return ret;
  }

  if (match) {
    return tx.commit();
  }

  cmd = fmt::format(
      R"(DROP TABLE IF EXISTS cache;
CREATE TABLE cache (
  key BLOB PRIMARY KEY NOT NULL,
  value BLOB NOT NULL,
  size INTEGER NOT NULL,
  compressed INTEGER NOT NULL
);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
      CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    open_error("Failed to update schema: {}", sqlite3_errmsg(db));
    return ret;
  }

  ret = tx.commit();
  if (ret != SQLITE_OK) {
    open_error("Failed to commit the schema: {}", sqlite3_errmsg(db));
  }
  return ret;
}

static int cache_init_core() {
  Log.debug("SQLite version {}", sqlite3_libversion());
  cache_open_error.clear();

  std::string file = io::fs_path_to_string(cache_path());
  Log.debug("Using dawn cache at {}", file);
  const char* openFile = file.c_str(); /* HMMMGC_NX_SQLITE */
#if defined(__SWITCH__)
  if (file.rfind("sdmc:", 0) == 0) openFile += 5;
#endif
  auto ret = sqlite3_open(openFile, &db);
  if (ret != SQLITE_OK) {
    open_error("Failed to open database: {}", sqlite3_errmsg(db));
    return ret;
  }

  ret = sqlite::apply_cache_pragmas(db, file, Log);
  if (ret != SQLITE_OK) {
    open_error("Failed to set pragmas: {}", sqlite3_errmsg(db));
    return ret;
  }

  ret = ensure_schema_up_to_date();
  if (ret != SQLITE_OK) {
    return ret;
  }

  // A cheap look at the file before using it: counts the rows through the key index (a few pages,
  // no values read). Damage elsewhere is caught by the first statement that meets it (cache_failed).
  ret = sqlite::exec(db, "SELECT count(*) FROM cache");
  if (ret != SQLITE_OK) {
    open_error("Failed to read the cache table: {}", sqlite3_errmsg(db));
    return ret;
  }

  ret = sqlite3_prepare_v3(db, "SELECT value, size, compressed FROM cache WHERE key = ?", -1, SQLITE_PREPARE_PERSISTENT,
                           &load_stmt, nullptr);
  if (ret != SQLITE_OK) {
    open_error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return ret;
  }

  ret = sqlite3_prepare_v3(db, "REPLACE INTO cache (key, value, size, compressed) VALUES (?, ?, ?, ?)", -1,
                           SQLITE_PREPARE_PERSISTENT, &store_stmt, nullptr);
  if (ret != SQLITE_OK) {
    open_error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return ret;
  }

  return SQLITE_OK;
}

static std::optional<uint64_t> select_uint64(const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  auto ret = sqlite3_prepare_v3(db, sql, -1, 0, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement '{}': {}", sql, sqlite3_errmsg(db));
    return std::nullopt;
  }

  std::optional<uint64_t> result;
  ret = sqlite3_step(stmt);
  if (ret == SQLITE_ROW) {
    result = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  } else if (ret != SQLITE_DONE) {
    Log.error("Failed to execute statement '{}': {}", sql, sqlite3_errmsg(db));
  }

  sqlite3_finalize(stmt);
  return result;
}

static bool fill_used_key_table(const std::vector<XXH128_hash_t>& usedKeys) {
  auto ret = sqlite::exec(db,
                          "CREATE TEMP TABLE IF NOT EXISTS cache_keys_used ("
                          "key BLOB PRIMARY KEY NOT NULL"
                          ");"
                          "DELETE FROM cache_keys_used;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare dawn cache prune key table: {}", sqlite3_errmsg(db));
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  ret = sqlite3_prepare_v3(db, "INSERT OR IGNORE INTO cache_keys_used (key) VALUES (?)", -1, 0, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare dawn cache prune key insert: {}", sqlite3_errmsg(db));
    return false;
  }

  for (const auto& keyHash : usedKeys) {
    ret = sqlite3_bind_blob(stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
      Log.error("Failed to bind dawn cache prune key: {}", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return false;
    }

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
      Log.error("Failed to insert dawn cache prune key: {}", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return false;
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  sqlite3_finalize(stmt);
  return true;
}

static bool cache_init() {
  if (cache_broken) {
    return false;
  }

  if (db) {
    return true;
  }

  const int ret = cache_init_core();
  if (ret != SQLITE_OK) {
    const auto file = io::fs_path_to_string(cache_path());
    const std::string why = cache_open_error;
    init_abort();
    if (!sqlite::reset_on_open_failure(ret)) {
      Log.error("GPU cache {} could not be opened ({}); running without it", file, why);
      return false;
    }
    // A damaged file: start once more from an empty one.
    const long long oldSize = sqlite::reset_cache_file(file);
    ++cache_resets;
    cache_broken = false;
    if (cache_init_core() != SQLITE_OK) {
      Log.error("GPU cache {} could not be opened ({}); deleted it ({} bytes) but a new one failed too ({}); "
                "running without it",
                file, why, oldSize, cache_open_error);
      init_abort();
      return false;
    }
    Log.warn("GPU cache {} could not be opened ({}); deleted it ({} bytes) and started an empty one", file, why,
             oldSize);
  }

  Log.debug("SQLite cache init succeeded");

  return true;
}

// A statement failed. A damaged file (a write torn by a killed process, a truncated or overwritten
// file) is reported once, deleted and replaced by an empty cache, so the run goes on caching
// instead of failing (and logging) every lookup. Other errors are logged rate-limited. After two
// resets in one run the cache is turned off for the rest of it.
static void cache_failed(const char* what, int rc) {
  if (!sqlite::is_damaged(rc)) {
    if (cache_errors.next()) {
      Log.error("{} failed: {} ({} cache errors so far)", what, sqlite3_errstr(rc), cache_errors.count);
    }
    return;
  }

  const auto file = io::fs_path_to_string(cache_path());
  cache_close();
  if (cache_resets >= 2) {
    Log.error("{} failed: {}; the GPU cache was already rebuilt {} times this run, running without it", what,
              sqlite3_errstr(rc), cache_resets);
    cache_broken = true;
    return;
  }
  ++cache_resets;
  const long long oldSize = sqlite::reset_cache_file(file);
  if (cache_init_core() != SQLITE_OK) {
    init_abort();
    Log.error("GPU cache {} is damaged ({}: {}); deleted it ({} bytes) but a new one failed ({}); running "
              "without it",
              file, what, sqlite3_errstr(rc), oldSize, cache_open_error);
    return;
  }
  Log.warn("GPU cache {} is damaged ({}: {}); deleted it ({} bytes) and started an empty one", file, what,
           sqlite3_errstr(rc), oldSize);
}

// One lookup with the connection open. Returns SQLITE_OK or the error; foundSize is the stored size
// (0 for a miss), loaded whether value was filled.
static int load_locked(const XXH128_hash_t& keyHash, void* value, size_t valueSize, size_t& foundSize,
                       bool& loaded) {
  foundSize = 0;
  loaded = false;
  CacheTransaction tx("BEGIN");
  if (!tx.active) {
    return tx.rc;
  }

  StatementScope scope{load_stmt};
  auto ret = sqlite3_bind_blob(load_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
  if (ret != SQLITE_OK) {
    return ret;
  }

  ret = sqlite3_step(load_stmt);
  if (ret == SQLITE_DONE) {
    // Miss
    return SQLITE_OK;
  }
  if (ret != SQLITE_ROW) {
    return ret;
  }

  // Hit
  const auto foundPtr = sqlite3_column_blob(load_stmt, 0);
  foundSize = sqlite3_column_int64(load_stmt, 1);
  const bool compressed = sqlite3_column_int(load_stmt, 2) != 0;

  if (value && valueSize == foundSize) {
    loaded = true;
    if (compressed) {
#if defined(AURORA_CACHE_USE_ZSTD)
      const auto compSize = sqlite3_column_bytes(load_stmt, 0);
      const auto zstdRet = ZSTD_decompress(value, valueSize, foundPtr, compSize);
      if (ZSTD_isError(zstdRet)) {
        Log.error("zstd decompression error: {}", ZSTD_getErrorName(zstdRet));
        foundSize = 0;
        loaded = false;
      } else if (zstdRet != foundSize) {
        Log.error("zstd decompression size mismatch: expected {}, got {}", foundSize, zstdRet);
        foundSize = 0;
        loaded = false;
      }
#else
      Log.error("Cache entry is zstd-compressed but zstd support is disabled");
      foundSize = 0;
      loaded = false;
#endif
    } else {
      if (foundSize != 0 && !foundPtr) {
        Log.error("Cache entry is missing raw value data");
        foundSize = 0;
        loaded = false;
      } else if (foundSize != 0) {
        std::memcpy(value, foundPtr, foundSize);
      }
    }
  }
  return SQLITE_OK;
}

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void*) {
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return 0;
  }

  const auto keyHash = XXH128(key, keySize, 0);
  size_t foundSize = 0;
  bool loaded = false;
  const int ret = load_locked(keyHash, value, valueSize, foundSize, loaded);
  if (ret != SQLITE_OK) {
    cache_failed("Looking up cache key", ret);
    return 0;
  }

  if (loaded) {
    cache_keys_used.push_back(keyHash);
  }

  return foundSize;
}

static int store_locked(const XXH128_hash_t& keyHash, void const* value, size_t valueSize) {
  CacheTransaction tx("BEGIN IMMEDIATE");
  if (!tx.active) {
    return tx.rc;
  }

  const void* storedValue = value;
  sqlite3_uint64 storedValueSize = valueSize;
  int compressed = 0;
#if defined(AURORA_CACHE_USE_ZSTD)
  const auto bound = ZSTD_compressBound(valueSize);
  if (ZSTD_isError(bound)) {
    Log.error("Failed to calculate ZSTD_compressBound: {}", ZSTD_getErrorName(bound));
    return SQLITE_OK;
  }

  if (compress_buffer.size() < bound) {
    compress_buffer.resize(bound);
  }

  const auto compressRet = ZSTD_compress(compress_buffer.data(), compress_buffer.size(), value, valueSize, 0);
  if (ZSTD_isError(compressRet)) {
    Log.error("ZSTD compression error: {}", ZSTD_getErrorName(compressRet));
    return SQLITE_OK;
  }

  if (compressRet < valueSize) {
    storedValue = compress_buffer.data();
    storedValueSize = compressRet;
    compressed = 1;
  }
#endif

  {
    StatementScope scope{store_stmt};
    int ret = sqlite3_bind_blob64(store_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
    if (ret == SQLITE_OK) {
      ret = sqlite3_bind_blob64(store_stmt, 2, storedValue, storedValueSize,
                                compressed ? SQLITE_STATIC : SQLITE_TRANSIENT);
    }
    if (ret == SQLITE_OK) {
      ret = sqlite3_bind_int64(store_stmt, 3, static_cast<sqlite3_int64>(valueSize));
    }
    if (ret == SQLITE_OK) {
      ret = sqlite3_bind_int(store_stmt, 4, compressed);
    }
    if (ret != SQLITE_OK) {
      return ret;
    }
    ret = sqlite3_step(store_stmt);
    if (ret != SQLITE_DONE) {
      return ret;
    }
  }

  return tx.commit();
}

void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void*) {
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return;
  }

  const auto keyHash = XXH128(key, keySize, 0);
  int ret = store_locked(keyHash, value, valueSize);
  if (ret != SQLITE_OK && sqlite::journal_fallback(db, ret, io::fs_path_to_string(cache_path()), Log)) {
    ret = store_locked(keyHash, value, valueSize);
  }
  if (ret != SQLITE_OK) {
    cache_failed("Storing cache entry", ret);
    return;
  }
  cache_keys_used.push_back(keyHash);
}

void cache_prune() {
  std::lock_guard lock(cache_mutex);
  if (!cache_init()) {
    return;
  }
  if (cache_keys_used.empty()) {
    Log.warn("Skipping Dawn cache prune because no cache keys were used");
    return;
  }

  uint64_t totalRows = 0;
  sqlite3_int64 deletedRows = 0;
  const auto prune = [&]() -> int {
    CacheTransaction tx("BEGIN IMMEDIATE");
    if (!tx.active) {
      return tx.rc;
    }

    if (!fill_used_key_table(cache_keys_used)) {
      return sqlite3_errcode(db) != SQLITE_OK ? sqlite3_errcode(db) : SQLITE_ERROR;
    }

    const auto totalRowsResult = select_uint64("SELECT COUNT(*) FROM cache");
    if (!totalRowsResult) {
      return sqlite3_errcode(db) != SQLITE_OK ? sqlite3_errcode(db) : SQLITE_ERROR;
    }
    totalRows = *totalRowsResult;

    const auto ret = sqlite::exec(db,
                                  "DELETE FROM cache "
                                  "WHERE NOT EXISTS ("
                                  "  SELECT 1 FROM cache_keys_used WHERE cache_keys_used.key = cache.key"
                                  ")");
    if (ret != SQLITE_OK) {
      return ret;
    }
    deletedRows = sqlite3_changes64(db);

    return tx.commit();
  };
  int pruneRet = prune();
  if (pruneRet != SQLITE_OK && sqlite::journal_fallback(db, pruneRet, io::fs_path_to_string(cache_path()), Log)) {
    pruneRet = prune();
  }
  if (const int ret = pruneRet; ret != SQLITE_OK) {
    cache_failed("Pruning the Dawn cache", ret);
    return;
  }

  if (deletedRows == 0) {
    Log.debug("Dawn cache prune completed; no stale entries found");
    return;
  }

  Log.info("Pruned {} stale Dawn cache entries", deletedRows);

#if defined(__SWITCH__)
  // Horizon: no VACUUM. It rewrites the whole file (tens of MiB on the SD card, twice with the
  // rollback journal) for seconds with cache_mutex held, so every Dawn lookup on the render worker
  // waits; the freed pages are reused by later inserts instead. No WAL to checkpoint either.
  (void)totalRows;
#else
  // VACUUM if we removed at least 25% of the rows
  if (totalRows != 0 && static_cast<uint64_t>(deletedRows) * 100ull >= totalRows * VacuumPrunePercentThreshold) {
    if (const auto ret = sqlite::exec(db, "VACUUM;"); ret != SQLITE_OK) {
      Log.warn("Failed to vacuum dawn cache after pruning: {}", sqlite3_errmsg(db));
      return;
    }
  }

  if (const auto ret = sqlite::exec(db, "PRAGMA wal_checkpoint(TRUNCATE);"); ret != SQLITE_OK) {
    Log.warn("Failed to checkpoint dawn cache WAL: {}", sqlite3_errmsg(db));
  }
#endif
}

void cache_shutdown() {
  std::lock_guard lock(cache_mutex);
#if defined(AURORA_CACHE_USE_ZSTD)
  compress_buffer.clear();
#endif
  cache_keys_used.clear();
  cache_close();
}

} // namespace aurora::webgpu
