#pragma once

// Calamansi Juice 2 addition (not present in upstream lemonade-sdk/lemonade):
// a persistent, SQLite-backed history of completed inference requests
// (model, tokens, latency, tokens/sec, backend, device, success/error),
// queryable via REST and rendered by the GUI's "History" tab. Survives
// server restarts. Never stores full prompt/response text unless
// telemetry.history.store_previews is explicitly enabled in config.json.
//
// See docs/calamansi/history.md and MERGING.md for how this fits into the
// fork's upstream-tracking policy.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace lemon {

// Who is asking. Mirrors the three-tier scoping already used by
// WebSocketServer::broadcast_span() (websocket_server.cpp) for the
// /spans/stream endpoint: an admin token sees everything, a token matching a
// stored request's auth_token_hash sees only its own requests, and an
// unauthenticated/guest caller is scoped to its client_session_id.
struct HistoryAccessScope {
    bool is_admin = false;
    // Hash of the requester's bearer token (telemetry::hash_token()). Empty
    // when the requester is unauthenticated/guest.
    std::string auth_token_hash;
    // X-Client-Session-Id of the requester, used only when auth_token_hash is
    // empty (guest scoping).
    std::string client_session_id;
};

struct HistoryQueryFilters {
    std::optional<int64_t> since_ms;
    std::optional<int64_t> until_ms;
    std::optional<std::string> model;
    std::optional<std::string> route;
    std::optional<std::string> backend;
    int limit = 50;
    int offset = 0;
};

struct HistoryRecord {
    int64_t id = 0;
    std::string request_id;
    int64_t timestamp_ms = 0;
    std::string model_name;
    std::string route;
    std::string route_decision;
    int prompt_tokens = -1;
    int completion_tokens = -1;
    double latency_ms = -1.0;
    double tokens_per_second = -1.0;
    std::string backend;
    std::string device;
    bool success = true;
    std::string error;
    // Only populated when telemetry.history.store_previews is enabled; empty
    // otherwise (this is the opt-in prompt/response preview, not raw
    // completion text — see record_span()).
    std::string preview;

    nlohmann::json to_json() const;
};

struct HistoryQueryResult {
    std::vector<HistoryRecord> records;
    int64_t total = 0;
};

// A persistent (SQLite-backed) log of completed inference spans, independent
// of the OTLP telemetry pipeline in telemetry.cpp. Owned by Server, alongside
// WebSocketServer, for the lifetime of the process.
//
// Thread-safety: every public method is safe to call from any thread. SQLite
// calls are serialized behind db_mutex_ (the amalgamation build is compiled
// SQLITE_THREADSAFE=1, but a single shared connection still needs external
// serialization for our access pattern). A SQLite failure is always logged
// and swallowed — never propagated to the caller — matching the
// "telemetry must never break the request path" spirit of telemetry.cpp.
class TelemetryHistoryStore {
public:
    // Opens/creates <cache_dir>/telemetry_history.db and ensures the schema
    // exists. If telemetry_history_enabled() is true in the current
    // RuntimeConfig, also registers as a telemetry span listener and starts
    // the background pruning thread. Never throws — a DB open/schema failure
    // is logged and leaves the store permanently inert (is_available()
    // reports false; all read/write methods become safe no-ops).
    explicit TelemetryHistoryStore(const std::string& cache_dir);
    ~TelemetryHistoryStore();

    TelemetryHistoryStore(const TelemetryHistoryStore&) = delete;
    TelemetryHistoryStore& operator=(const TelemetryHistoryStore&) = delete;

    bool is_available() const { return db_ != nullptr; }

    // Span listener entry point (registered with telemetry::register_span_listener
    // when enabled). Public so it can also be unit-tested directly without
    // going through the full HTTP + span pub/sub stack. Extracts attributes,
    // computes latency, and inserts one row. Never throws.
    void record_span(const nlohmann::json& span_details);

    // Returns matching rows (newest first) plus the total match count
    // (ignoring limit/offset) for pagination.
    HistoryQueryResult query(const HistoryQueryFilters& filters,
                             const HistoryAccessScope& scope) const;

    // Server-computed aggregates for the GUI's charts: avg tokens/sec by
    // model, request count by day, and overall error rate — all computed via
    // SQL GROUP BY so the GUI never has to pull raw rows to chart them.
    nlohmann::json summary(std::optional<int64_t> since_ms,
                           std::optional<int64_t> until_ms,
                           const HistoryAccessScope& scope) const;

    // Deletes all rows. Used by the "Clear History" GUI action
    // (POST /internal/telemetry/history/clear).
    void clear();

private:
    void open_database(const std::string& cache_dir);
    void run_migrations();
    void prune_loop();
    void prune_once();

    sqlite3* db_ = nullptr;
    mutable std::mutex db_mutex_;

    // telemetry::SpanListenerHandle, stored as its underlying type so this
    // header does not need to include telemetry.h (same reasoning as
    // websocket_server.h's telemetry_listener_handle_).
    std::size_t listener_handle_ = 0;

    std::thread prune_thread_;
    std::mutex prune_mutex_;
    std::condition_variable prune_cv_;
    bool prune_shutdown_ = false;
};

} // namespace lemon
