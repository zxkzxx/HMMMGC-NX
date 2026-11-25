#pragma once

#include "internal.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <system_error>
#if defined(__SWITCH__)
#include <cstdlib>
#include <string_view>
#endif

namespace aurora::sqlite {

// Sets a cache database's journal and sync settings (path: the database file).
inline int apply_cache_pragmas(sqlite3* db, const std::string& path, Module& log) {
#if defined(__SWITCH__)
  // Horizon (the Switch's SD card through newlib; WAL is compiled out): a rollback journal kept
  // next to the database (PERSIST: created once, its header zeroed at each commit instead of the
  // file being deleted; with the build's exclusive locking it stays open) and no syncs. The game is
  // usually closed with HOME, which kills the process: a commit cut short is rolled back from the
  // journal when the cache is next opened. With journal_mode=MEMORY a killed commit or VACUUM left
  // a damaged file (Mac kill -9 test with the Switch's sqlite build: 113 of 150 kills damaged it
  // with MEMORY, 0 of 150 with PERSIST, TRUNCATE or DELETE). synchronous=OFF is enough for that:
  // what write() handed the file system before the kill reaches the card; only a power loss in the
  // middle of a commit could still damage the file (then deleted and rebuilt, see is_damaged).
  //
  // COS_SWITCH_SQLITE_JOURNAL (env.txt): persist (default), truncate, delete or memory (no journal
  // file). A journal file that does not work falls back to memory with one line: at open a write
  // that makes sqlite create the journal (the user_version rewrite below) is tried first, and a
  // write that fails later with an I/O error switches too (journal_fallback). 2026-10-04: on the
  // console every write with PERSIST failed with SQLITE_IOERR_FSTAT, because sqlite stats the
  // database (open for writing) when it creates the journal and libnx's stat cannot open a file
  // that is open for writing; switch/aurora/sqlite_horizon.c now answers that stat from sqlite's
  // own descriptor.
  const char* mode = "MEMORY";
  if (const char* env = std::getenv("COS_SWITCH_SQLITE_JOURNAL"); env != nullptr && env[0] != '\0') {
    const std::string value{env};
    if (value == "memory" || value == "MEMORY") {
      mode = "MEMORY";
    } else if (value == "truncate" || value == "TRUNCATE") {
      mode = "TRUNCATE";
    } else if (value == "delete" || value == "DELETE") {
      mode = "DELETE";
    }
  }
  const std::string pragmas = std::string{"PRAGMA journal_mode="} + mode +
                              "; PRAGMA journal_size_limit=4194304; PRAGMA synchronous=OFF;";
  int ret = sqlite3_exec(db, pragmas.c_str(), nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    return ret;
  }
  if (std::string_view{mode} != "MEMORY") {
    // Rewrites the header's user_version with its own value: page 1 goes through the journal,
    // which sqlite creates (and, with exclusive locking, keeps open) here.
    int version = 0;
    ret = sqlite3_exec(
        db, "PRAGMA user_version",
        [](void* out, int, char** values, char**) {
          *static_cast<int*>(out) = values[0] != nullptr ? std::atoi(values[0]) : 0;
          return 0;
        },
        &version, nullptr);
    if (ret == SQLITE_OK) {
      const std::string probe =
          "BEGIN IMMEDIATE; PRAGMA user_version=" + std::to_string(version) + "; COMMIT;";
      ret = sqlite3_exec(db, probe.c_str(), nullptr, nullptr, nullptr);
    }
    if (ret != SQLITE_OK) {
      if (!sqlite3_get_autocommit(db)) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      }
      const int primary = ret & 0xff;
      if (primary != SQLITE_IOERR && primary != SQLITE_CANTOPEN) {
        return ret; // a damaged file: the caller deletes it and starts again
      }
      log.warn("{}: writing through the {} journal failed ({}); journal_mode=MEMORY for this run", path, mode,
               sqlite3_errstr(ret));
      mode = "MEMORY";
      ret = sqlite3_exec(db, "PRAGMA journal_mode=MEMORY;", nullptr, nullptr, nullptr);
      if (ret != SQLITE_OK) {
        return ret;
      }
    }
  }
  log.info("{}: journal_mode={}, synchronous=OFF", path, mode);
  return SQLITE_OK;
#else
  (void)path;
  (void)log;
  // WAL mode + NORMAL = no need for disk syncs, consistent but not durable is fine.
  return sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
#endif
}

// After a write failed with rc: whether the cache's journal setting was changed so the write can be
// tried once more (the Switch patch 0006 falls back from a journal file to journal_mode=MEMORY on an
// I/O error). The failed transaction must already be over.
inline bool journal_fallback(sqlite3* db, int rc, const std::string& path, Module& log) {
#if defined(__SWITCH__)
  const int primary = rc & 0xff;
  if (db == nullptr || (primary != SQLITE_IOERR && primary != SQLITE_CANTOPEN)) {
    return false;
  }
  if (!sqlite3_get_autocommit(db)) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  std::string current;
  sqlite3_exec(
      db, "PRAGMA journal_mode",
      [](void* out, int, char** values, char**) {
        *static_cast<std::string*>(out) = values[0] != nullptr ? values[0] : "";
        return 0;
      },
      &current, nullptr);
  if (current == "memory" || current == "off") {
    return false;
  }
  if (sqlite3_exec(db, "PRAGMA journal_mode=MEMORY;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    return false;
  }
  log.warn("{}: a write through the {} journal failed ({}); journal_mode=MEMORY for this run", path, current,
           sqlite3_errstr(rc));
  return true;
#else
  (void)db;
  (void)rc;
  (void)path;
  (void)log;
  return false;
#endif
}

// A result that means the file itself is damaged (a torn write, a truncated or overwritten file):
// the cache is deleted and started again from an empty file instead of failing every statement.
inline bool is_damaged(int rc) {
  rc &= 0xff;
  return rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB;
}

// Whether a cache that failed to open with this result is deleted and created once more.
inline bool reset_on_open_failure(int rc) {
#if defined(__SWITCH__)
  // Horizon: any failure (the console's failures did not all say "malformed").
  return rc != SQLITE_OK;
#else
  return is_damaged(rc);
#endif
}

// Removes a cache database and the files sqlite keeps next to it, so the caller can start it again
// from an empty file. Returns the size the database had, or -1.
inline long long reset_cache_file(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  const long long result = ec ? -1 : static_cast<long long>(size);
  for (const char* suffix : {"", "-journal", "-wal", "-shm"}) {
    std::filesystem::remove(path + suffix, ec);
  }
  return result;
}

// Logs the first few errors of a kind and then only every 1000th, so a cache that keeps failing
// cannot flood the log. Returns whether this one is logged; `count` is the total so far.
struct ErrorLimiter {
  uint64_t count = 0;
  bool next() {
    ++count;
    return count <= 3 || count % 1000 == 0;
  }
};

inline int exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

template <typename T>
int exec(sqlite3* db, const char* sql, T callback, char** errmsg = nullptr) {
  return sqlite3_exec(
      db, sql,
      [](void* cb, int argc, char** argv, char** columns) -> int {
        auto& fp = *static_cast<T*>(cb);
        if constexpr (std::is_same_v<decltype(fp(0, 0, 0)), void>) {
          fp(argc, argv, columns);
          return 0;
        } else {
          return fp(argc, argv, columns);
        }
      },
      &callback, errmsg);
}

class Transaction {
public:
  Transaction(sqlite3* db, Module& log, bool immediate = false) : m_db(db), m_log(log) {
    const auto type = immediate ? "BEGIN IMMEDIATE" : "BEGIN";
    const auto ret = sqlite3_exec(m_db, type, nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
      m_log.error("Failed to start transaction: {}", sqlite3_errmsg(m_db));
      return;
    }
    m_active = true;
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(Transaction&&) = delete;

  ~Transaction() {
    // sqlite3_get_autocommit: an I/O error or a full disk may already have rolled it back.
    if (!m_active || sqlite3_get_autocommit(m_db)) {
      return;
    }

    const auto ret = sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
      m_log.error("Failed to roll back transaction (uh oh?): {}", sqlite3_errmsg(m_db));
    }
  }

  int commit() {
    if (!m_active) {
      return SQLITE_OK;
    }

    const auto ret = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
      m_log.error("Failed to commit transaction: {}", sqlite3_errmsg(m_db));
      return ret;
    }

    m_active = false;
    return SQLITE_OK;
  }

  explicit operator bool() const { return m_active; }

private:
  sqlite3* m_db = nullptr;
  Module& m_log;
  bool m_active = false;
};

} // namespace aurora::sqlite
