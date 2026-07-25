#include "telemetry_history_store.h"

#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"
#include "telemetry.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <variant>

namespace fs = std::filesystem;

namespace lemon {

namespace {

// --- span attribute helpers -------------------------------------------------
// span_details["attributes"] is an array of {"key": "...", "value": {"stringValue"|
// "intValue"|"doubleValue"|"boolValue": ...}} objects (see InferenceSpan::build_common_attributes
// / set_attribute in telemetry.cpp). These mirror the small helpers WebSocketServer::broadcast_span
// already uses inline for a couple of keys, generalized to any key/type.

std::optional<std::string> attr_string(const nlohmann::json& attrs, const std::string& key) {
    if (!attrs.is_array()) return std::nullopt;
    for (const auto& a : attrs) {
        if (!a.is_object() || a.value("key", std::string()) != key) continue;
        if (!a.contains("value") || !a["value"].is_object()) return std::nullopt;
        const auto& v = a["value"];
        if (v.contains("stringValue") && v["stringValue"].is_string()) {
            return v["stringValue"].get<std::string>();
        }
        if (v.contains("intValue") && v["intValue"].is_number_integer()) {
            return std::to_string(v["intValue"].get<int64_t>());
        }
        if (v.contains("doubleValue") && v["doubleValue"].is_number()) {
            return std::to_string(v["doubleValue"].get<double>());
        }
        if (v.contains("boolValue") && v["boolValue"].is_boolean()) {
            return v["boolValue"].get<bool>() ? "true" : "false";
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> attr_number(const nlohmann::json& attrs, const std::string& key) {
    if (!attrs.is_array()) return std::nullopt;
    for (const auto& a : attrs) {
        if (!a.is_object() || a.value("key", std::string()) != key) continue;
        if (!a.contains("value") || !a["value"].is_object()) return std::nullopt;
        const auto& v = a["value"];
        if (v.contains("intValue") && v["intValue"].is_number_integer()) {
            return static_cast<double>(v["intValue"].get<int64_t>());
        }
        if (v.contains("doubleValue") && v["doubleValue"].is_number()) {
            return v["doubleValue"].get<double>();
        }
        if (v.contains("stringValue") && v["stringValue"].is_string()) {
            try {
                return std::stod(v["stringValue"].get<std::string>());
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// First matching attribute among a list of candidate keys, tried in order.
// Used to cover both "openinference" (llm./embedding./reranker.-prefixed) and
// "otel_genai" (gen_ai.-prefixed) semantics, whichever the deployment has
// telemetry.otlp.semantics configured to emit — see get_telemetry_semantics()
// in telemetry.cpp. If a deployment disables both semantics, these fields are
// simply left empty/null; that's an accepted limitation, not a bug.
std::optional<std::string> attr_string_any(const nlohmann::json& attrs,
                                            std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto v = attr_string(attrs, key)) return v;
    }
    return std::nullopt;
}

std::optional<double> attr_number_any(const nlohmann::json& attrs,
                                       std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto v = attr_number(attrs, key)) return v;
    }
    return std::nullopt;
}

std::string truncate_preview(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    if (max_chars == 0) return "";
    return s.substr(0, max_chars) + "... [TRUNCATED]";
}

// Minimal bound-parameter variant for the dynamically-built WHERE clauses in
// query()/summary(). Keeps parameter binding out-of-order-proof: we build the
// SQL text and this vector together, then bind by position in a single pass.
struct BindParam {
    enum class Kind { Text, Int64, Double } kind;
    std::string text;
    int64_t i = 0;
    double d = 0.0;

    static BindParam of(const std::string& v) { return {Kind::Text, v, 0, 0.0}; }
    static BindParam of(int64_t v) { return {Kind::Int64, {}, v, 0.0}; }
    static BindParam of(double v) { return {Kind::Double, {}, 0, v}; }
};

void bind_params(sqlite3_stmt* stmt, const std::vector<BindParam>& params) {
    int idx = 1;
    for (const auto& p : params) {
        switch (p.kind) {
            case BindParam::Kind::Text:
                sqlite3_bind_text(stmt, idx, p.text.c_str(), -1, SQLITE_TRANSIENT);
                break;
            case BindParam::Kind::Int64:
                sqlite3_bind_int64(stmt, idx, p.i);
                break;
            case BindParam::Kind::Double:
                sqlite3_bind_double(stmt, idx, p.d);
                break;
        }
        ++idx;
    }
}

// Appends the WHERE fragments (each starting with " AND ...") common to both
// query() and summary(): time range, model/route/backend filters (filters may
// be null for summary(), which only takes a time range), and the
// admin/own-token/own-session access scope mirrored from
// WebSocketServer::broadcast_span()'s filtering rules.
void append_common_where(std::string& where,
                         std::vector<BindParam>& params,
                         std::optional<int64_t> since_ms,
                         std::optional<int64_t> until_ms,
                         const HistoryAccessScope& scope) {
    if (since_ms) {
        where += " AND timestamp_ms >= ?";
        params.push_back(BindParam::of(*since_ms));
    }
    if (until_ms) {
        where += " AND timestamp_ms <= ?";
        params.push_back(BindParam::of(*until_ms));
    }

    if (scope.is_admin) {
        // Admins see every row - no additional restriction.
    } else if (!scope.auth_token_hash.empty()) {
        where += " AND auth_token_hash = ?";
        params.push_back(BindParam::of(scope.auth_token_hash));
    } else if (!scope.client_session_id.empty()) {
        where += " AND client_session_id = ?";
        params.push_back(BindParam::of(scope.client_session_id));
    } else {
        // Unauthenticated caller with no session id: matches nothing, same as
        // the WS spans stream's guest branch when it has no session to key on.
        where += " AND 0";
    }
}

} // namespace

nlohmann::json HistoryRecord::to_json() const {
    nlohmann::json j = {
        {"id", id},
        {"request_id", request_id},
        {"timestamp_ms", timestamp_ms},
        {"model_name", model_name},
        {"route", route},
        {"route_decision", route_decision.empty() ? nlohmann::json(nullptr) : nlohmann::json(route_decision)},
        {"prompt_tokens", prompt_tokens >= 0 ? nlohmann::json(prompt_tokens) : nlohmann::json(nullptr)},
        {"completion_tokens", completion_tokens >= 0 ? nlohmann::json(completion_tokens) : nlohmann::json(nullptr)},
        {"latency_ms", latency_ms >= 0 ? nlohmann::json(latency_ms) : nlohmann::json(nullptr)},
        {"ttft_seconds", ttft_seconds >= 0 ? nlohmann::json(ttft_seconds) : nlohmann::json(nullptr)},
        {"tokens_per_second", tokens_per_second >= 0 ? nlohmann::json(tokens_per_second) : nlohmann::json(nullptr)},
        {"backend", backend},
        {"device", device},
        {"success", success},
        {"error", error},
        {"preview", preview},
    };
    return j;
}

TelemetryHistoryStore::TelemetryHistoryStore(const std::string& cache_dir) {
    open_database(cache_dir);
    if (!db_) return;

    run_migrations();
    if (!db_) return;

    bool history_enabled = true;
    if (auto* config = RuntimeConfig::global()) {
        history_enabled = config->telemetry_history_enabled();
    }
    if (history_enabled) {
        listener_handle_ = telemetry::register_span_listener(
            [this](const nlohmann::json& span) { this->record_span(span); });
    }

    // The pruning thread runs regardless of whether capture is currently
    // enabled, so previously-collected rows still age out per
    // max_age_days/max_rows even if the admin later disables capture.
    prune_thread_ = std::thread(&TelemetryHistoryStore::prune_loop, this);
}

TelemetryHistoryStore::~TelemetryHistoryStore() {
    if (listener_handle_ != 0) {
        telemetry::unregister_span_listener(listener_handle_);
        listener_handle_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(prune_mutex_);
        prune_shutdown_ = true;
    }
    prune_cv_.notify_all();
    if (prune_thread_.joinable()) {
        prune_thread_.join();
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void TelemetryHistoryStore::open_database(const std::string& cache_dir) {
    try {
        fs::path dir = utils::path_from_utf8(cache_dir);
        std::error_code ec;
        if (!dir.empty() && !fs::exists(dir, ec)) {
            fs::create_directories(dir, ec);
            if (ec) {
                LOG(WARNING, "TelemetryHistory") << "Failed to create cache dir " << cache_dir
                                                 << " for telemetry_history.db: " << ec.message() << std::endl;
            }
        }

        fs::path db_path = dir / "telemetry_history.db";
        sqlite3* handle = nullptr;
        int rc = sqlite3_open_v2(
            utils::path_to_utf8(db_path).c_str(), &handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            LOG(ERROR, "TelemetryHistory") << "Failed to open " << utils::path_to_utf8(db_path)
                                           << ": " << (handle ? sqlite3_errmsg(handle) : "unknown error")
                                           << std::endl;
            if (handle) sqlite3_close(handle);
            db_ = nullptr;
            return;
        }

        // WAL + NORMAL sync keeps per-request insert latency low without
        // risking corruption on crash (WAL only risks losing the last few
        // uncommitted transactions, never the file itself). busy_timeout
        // avoids SQLITE_BUSY errors from the pruning thread racing an insert.
        char* errmsg = nullptr;
        sqlite3_exec(handle, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errmsg);
        if (errmsg) { sqlite3_free(errmsg); errmsg = nullptr; }
        sqlite3_exec(handle, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &errmsg);
        if (errmsg) { sqlite3_free(errmsg); errmsg = nullptr; }
        sqlite3_busy_timeout(handle, 5000);

        db_ = handle;
    } catch (const std::exception& e) {
        LOG(ERROR, "TelemetryHistory") << "Failed to open telemetry_history.db: " << e.what() << std::endl;
        db_ = nullptr;
    } catch (...) {
        LOG(ERROR, "TelemetryHistory") << "Failed to open telemetry_history.db: unknown error" << std::endl;
        db_ = nullptr;
    }
}

void TelemetryHistoryStore::run_migrations() {
    static const char* kSchema = R"SQL(
        CREATE TABLE IF NOT EXISTS telemetry_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id TEXT,
            timestamp_ms INTEGER NOT NULL,
            model_name TEXT,
            route TEXT,
            route_decision TEXT,
            prompt_tokens INTEGER,
            completion_tokens INTEGER,
            latency_ms REAL,
            ttft_seconds REAL,
            tokens_per_second REAL,
            backend TEXT,
            device TEXT,
            success INTEGER NOT NULL,
            error TEXT,
            preview TEXT,
            auth_token_hash TEXT,
            client_session_id TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_telemetry_history_timestamp_ms ON telemetry_history(timestamp_ms);
        CREATE INDEX IF NOT EXISTS idx_telemetry_history_model_name ON telemetry_history(model_name);
        CREATE INDEX IF NOT EXISTS idx_telemetry_history_route ON telemetry_history(route);
        CREATE INDEX IF NOT EXISTS idx_telemetry_history_backend ON telemetry_history(backend);
    )SQL";

    std::lock_guard<std::mutex> lock(db_mutex_);
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, kSchema, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        LOG(ERROR, "TelemetryHistory") << "Failed to create telemetry_history schema: "
                                       << (errmsg ? errmsg : "unknown error") << std::endl;
        if (errmsg) sqlite3_free(errmsg);
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    // CREATE TABLE IF NOT EXISTS is a no-op on a pre-existing DB, so a store
    // created before ttft_seconds existed needs an explicit ALTER TABLE to
    // pick it up.
    bool has_ttft_column = false;
    sqlite3_stmt* pragma_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(telemetry_history);", -1, &pragma_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
            const unsigned char* col_name = sqlite3_column_text(pragma_stmt, 1);
            if (col_name && std::string(reinterpret_cast<const char*>(col_name)) == "ttft_seconds") {
                has_ttft_column = true;
                break;
            }
        }
    }
    sqlite3_finalize(pragma_stmt);
    if (!has_ttft_column) {
        sqlite3_exec(db_, "ALTER TABLE telemetry_history ADD COLUMN ttft_seconds REAL;", nullptr, nullptr, &errmsg);
        if (errmsg) {
            LOG(WARNING, "TelemetryHistory") << "Failed to add ttft_seconds column: " << errmsg << std::endl;
            sqlite3_free(errmsg);
        }
    }
}

void TelemetryHistoryStore::record_span(const nlohmann::json& span_details) {
    if (!db_) return;

    try {
        const nlohmann::json empty_attrs = nlohmann::json::array();
        const nlohmann::json& attrs = span_details.contains("attributes") ? span_details["attributes"] : empty_attrs;

        std::string request_id = span_details.value("traceId", "");
        std::string route = span_details.value("name", "");

        std::string model_name = attr_string_any(attrs, {"llm.model_name", "embedding.model_name",
                                                          "reranker.model_name", "gen_ai.request.model"})
                                      .value_or("");
        std::string route_decision = attr_string(attrs, "lemon.route_decision").value_or("");
        std::string backend = attr_string(attrs, "llm.backend").value_or("");
        std::string device = attr_string(attrs, "llm.device_type").value_or("");
        std::string auth_token_hash = attr_string(attrs, "lemon.auth_token_hash").value_or("");
        std::string client_session_id = attr_string(attrs, "lemon.client_session_id").value_or("");

        auto prompt_tokens_d = attr_number_any(
            attrs, {"llm.usage.prompt_tokens", "embedding.usage.prompt_tokens",
                   "reranker.usage.prompt_tokens", "gen_ai.usage.input_tokens"});
        auto completion_tokens_d = attr_number_any(
            attrs, {"llm.usage.completion_tokens", "embedding.usage.completion_tokens",
                   "reranker.usage.completion_tokens", "gen_ai.usage.output_tokens"});
        auto tokens_per_second = attr_number(attrs, "llm.performance.tokens_per_second");
        auto ttft_seconds = attr_number(attrs, "llm.performance.time_to_first_token");

        // status.code: 1 == success (InferenceSpan::end_with_success), 2 == error
        // (InferenceSpan::end_with_error). Spans that were merely cancel()'d never
        // reach emit_span() at all (cancel() returns before submit_span()), so
        // every span this listener sees legitimately completed one way or the other.
        bool success = true;
        std::string error_message;
        if (span_details.contains("status") && span_details["status"].is_object()) {
            int code = span_details["status"].value("code", 1);
            success = (code == 1);
            if (!success) {
                error_message = span_details["status"].value("message", "");
            }
        }

        int64_t start_nano = 0, end_nano = 0;
        try {
            start_nano = std::stoll(span_details.value("startTimeUnixNano", "0"));
            end_nano = std::stoll(span_details.value("endTimeUnixNano", "0"));
        } catch (...) {
            start_nano = 0;
            end_nano = 0;
        }
        int64_t timestamp_ms = start_nano / 1000000;
        double latency_ms = (end_nano > start_nano) ? static_cast<double>(end_nano - start_nano) / 1000000.0 : 0.0;

        bool store_previews = false;
        int preview_max_chars = 200;
        if (auto* config = RuntimeConfig::global()) {
            store_previews = config->telemetry_history_store_previews();
            preview_max_chars = config->telemetry_history_preview_max_chars();
        }

        std::string preview;
        if (store_previews) {
            std::string input_val = attr_string(attrs, "input.value").value_or("");
            std::string output_val = attr_string(attrs, "output.value").value_or("");
            std::string combined;
            if (!input_val.empty()) combined += "prompt: " + input_val;
            if (!output_val.empty()) {
                if (!combined.empty()) combined += "\n---\n";
                combined += "response: " + output_val;
            }
            preview = truncate_preview(combined, static_cast<size_t>((std::max)(0, preview_max_chars)));
        }

        static const char* kInsert = R"SQL(
            INSERT INTO telemetry_history
                (request_id, timestamp_ms, model_name, route, route_decision,
                 prompt_tokens, completion_tokens, latency_ms, ttft_seconds, tokens_per_second,
                 backend, device, success, error, preview, auth_token_hash, client_session_id)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )SQL";

        std::lock_guard<std::mutex> lock(db_mutex_);
        if (!db_) return;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
            LOG(WARNING, "TelemetryHistory") << "Failed to prepare insert: " << sqlite3_errmsg(db_) << std::endl;
            return;
        }

        int idx = 1;
        sqlite3_bind_text(stmt, idx++, request_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, idx++, timestamp_ms);
        sqlite3_bind_text(stmt, idx++, model_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, route.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, route_decision.c_str(), -1, SQLITE_TRANSIENT);
        if (prompt_tokens_d) sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(*prompt_tokens_d));
        else sqlite3_bind_null(stmt, idx++);
        if (completion_tokens_d) sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(*completion_tokens_d));
        else sqlite3_bind_null(stmt, idx++);
        sqlite3_bind_double(stmt, idx++, latency_ms);
        if (ttft_seconds) sqlite3_bind_double(stmt, idx++, *ttft_seconds);
        else sqlite3_bind_null(stmt, idx++);
        if (tokens_per_second) sqlite3_bind_double(stmt, idx++, *tokens_per_second);
        else sqlite3_bind_null(stmt, idx++);
        sqlite3_bind_text(stmt, idx++, backend.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, device.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, idx++, success ? 1 : 0);
        sqlite3_bind_text(stmt, idx++, error_message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, preview.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, auth_token_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, client_session_id.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            LOG(WARNING, "TelemetryHistory") << "Failed to insert history row: " << sqlite3_errmsg(db_) << std::endl;
        }
        sqlite3_finalize(stmt);
    } catch (const std::exception& e) {
        LOG(WARNING, "TelemetryHistory") << "record_span failed: " << e.what() << std::endl;
    } catch (...) {
        LOG(WARNING, "TelemetryHistory") << "record_span failed with unknown error" << std::endl;
    }
}

HistoryQueryResult TelemetryHistoryStore::query(const HistoryQueryFilters& filters,
                                                const HistoryAccessScope& scope) const {
    HistoryQueryResult result;
    if (!db_) return result;

    try {
        std::string where = " WHERE 1=1";
        std::vector<BindParam> params;

        if (filters.model) {
            where += " AND model_name = ?";
            params.push_back(BindParam::of(*filters.model));
        }
        if (filters.route) {
            where += " AND route = ?";
            params.push_back(BindParam::of(*filters.route));
        }
        if (filters.backend) {
            where += " AND backend = ?";
            params.push_back(BindParam::of(*filters.backend));
        }
        append_common_where(where, params, filters.since_ms, filters.until_ms, scope);

        std::lock_guard<std::mutex> lock(db_mutex_);
        if (!db_) return result;

        // Total count (ignores limit/offset) for pagination metadata.
        {
            std::string count_sql = "SELECT COUNT(*) FROM telemetry_history" + where + ";";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, count_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_params(stmt, params);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    result.total = sqlite3_column_int64(stmt, 0);
                }
            } else {
                LOG(WARNING, "TelemetryHistory") << "Failed to prepare count query: " << sqlite3_errmsg(db_) << std::endl;
            }
            sqlite3_finalize(stmt);
        }

        // Page of rows, newest first.
        {
            std::string select_sql =
                "SELECT id, request_id, timestamp_ms, model_name, route, route_decision, "
                "prompt_tokens, completion_tokens, latency_ms, ttft_seconds, tokens_per_second, backend, "
                "device, success, error, preview FROM telemetry_history" + where +
                " ORDER BY timestamp_ms DESC, id DESC LIMIT ? OFFSET ?;";
            std::vector<BindParam> select_params = params;
            select_params.push_back(BindParam::of(static_cast<int64_t>(filters.limit)));
            select_params.push_back(BindParam::of(static_cast<int64_t>(filters.offset)));

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, select_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_params(stmt, select_params);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    HistoryRecord rec;
                    int col = 0;
                    rec.id = sqlite3_column_int64(stmt, col++);
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.request_id = reinterpret_cast<const char*>(t);
                    col++;
                    rec.timestamp_ms = sqlite3_column_int64(stmt, col++);
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.model_name = reinterpret_cast<const char*>(t);
                    col++;
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.route = reinterpret_cast<const char*>(t);
                    col++;
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.route_decision = reinterpret_cast<const char*>(t);
                    col++;
                    rec.prompt_tokens = sqlite3_column_type(stmt, col) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, col);
                    col++;
                    rec.completion_tokens = sqlite3_column_type(stmt, col) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, col);
                    col++;
                    rec.latency_ms = sqlite3_column_type(stmt, col) == SQLITE_NULL ? -1.0 : sqlite3_column_double(stmt, col);
                    col++;
                    rec.ttft_seconds = sqlite3_column_type(stmt, col) == SQLITE_NULL ? -1.0 : sqlite3_column_double(stmt, col);
                    col++;
                    rec.tokens_per_second = sqlite3_column_type(stmt, col) == SQLITE_NULL ? -1.0 : sqlite3_column_double(stmt, col);
                    col++;
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.backend = reinterpret_cast<const char*>(t);
                    col++;
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.device = reinterpret_cast<const char*>(t);
                    col++;
                    rec.success = sqlite3_column_int(stmt, col) != 0;
                    col++;
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.error = reinterpret_cast<const char*>(t);
                    col++;
                    if (auto* t = sqlite3_column_text(stmt, col)) rec.preview = reinterpret_cast<const char*>(t);
                    col++;
                    result.records.push_back(std::move(rec));
                }
            } else {
                LOG(WARNING, "TelemetryHistory") << "Failed to prepare select query: " << sqlite3_errmsg(db_) << std::endl;
            }
            sqlite3_finalize(stmt);
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "TelemetryHistory") << "query failed: " << e.what() << std::endl;
    } catch (...) {
        LOG(WARNING, "TelemetryHistory") << "query failed with unknown error" << std::endl;
    }

    return result;
}

nlohmann::json TelemetryHistoryStore::summary(std::optional<int64_t> since_ms,
                                              std::optional<int64_t> until_ms,
                                              const HistoryAccessScope& scope) const {
    nlohmann::json result = {
        {"avg_tokens_per_second_by_model", nlohmann::json::array()},
        {"requests_by_day", nlohmann::json::array()},
        {"overall", {{"total_requests", 0}, {"total_errors", 0}, {"error_rate", 0.0}}},
    };
    if (!db_) return result;

    try {
        std::string where = " WHERE 1=1";
        std::vector<BindParam> params;
        append_common_where(where, params, since_ms, until_ms, scope);

        std::lock_guard<std::mutex> lock(db_mutex_);
        if (!db_) return result;

        // Avg tokens/sec by model - SQL GROUP BY, never pulls raw rows into C++.
        {
            std::string sql = "SELECT model_name, AVG(tokens_per_second) as avg_tps, COUNT(*) as cnt "
                              "FROM telemetry_history" + where +
                              " AND tokens_per_second IS NOT NULL GROUP BY model_name ORDER BY cnt DESC;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_params(stmt, params);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    std::string model = sqlite3_column_text(stmt, 0)
                        ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : "";
                    double avg_tps = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 1);
                    int64_t cnt = sqlite3_column_int64(stmt, 2);
                    result["avg_tokens_per_second_by_model"].push_back({
                        {"model", model},
                        {"avg_tokens_per_second", avg_tps},
                        {"request_count", cnt},
                    });
                }
            }
            sqlite3_finalize(stmt);
        }

        // Request count + error count by day.
        {
            std::string sql =
                "SELECT date(timestamp_ms/1000, 'unixepoch') as day, COUNT(*) as cnt, "
                "SUM(CASE WHEN success=0 THEN 1 ELSE 0 END) as errs "
                "FROM telemetry_history" + where + " GROUP BY day ORDER BY day ASC;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_params(stmt, params);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    std::string day = sqlite3_column_text(stmt, 0)
                        ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : "";
                    int64_t cnt = sqlite3_column_int64(stmt, 1);
                    int64_t errs = sqlite3_column_int64(stmt, 2);
                    double error_rate = cnt > 0 ? static_cast<double>(errs) / static_cast<double>(cnt) : 0.0;
                    result["requests_by_day"].push_back({
                        {"date", day},
                        {"count", cnt},
                        {"errors", errs},
                        {"error_rate", error_rate},
                    });
                }
            }
            sqlite3_finalize(stmt);
        }

        // Overall totals.
        {
            std::string sql = "SELECT COUNT(*), SUM(CASE WHEN success=0 THEN 1 ELSE 0 END) "
                              "FROM telemetry_history" + where + ";";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_params(stmt, params);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    int64_t total = sqlite3_column_int64(stmt, 0);
                    int64_t errors = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 1);
                    double error_rate = total > 0 ? static_cast<double>(errors) / static_cast<double>(total) : 0.0;
                    result["overall"] = {
                        {"total_requests", total},
                        {"total_errors", errors},
                        {"error_rate", error_rate},
                    };
                }
            }
            sqlite3_finalize(stmt);
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "TelemetryHistory") << "summary failed: " << e.what() << std::endl;
    } catch (...) {
        LOG(WARNING, "TelemetryHistory") << "summary failed with unknown error" << std::endl;
    }

    return result;
}

void TelemetryHistoryStore::clear() {
    if (!db_) return;
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!db_) return;
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, "DELETE FROM telemetry_history;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        LOG(WARNING, "TelemetryHistory") << "Failed to clear history: " << (errmsg ? errmsg : "unknown error") << std::endl;
        if (errmsg) sqlite3_free(errmsg);
    }
}

void TelemetryHistoryStore::prune_once() {
    if (!db_) return;

    int max_age_days = 30;
    int max_rows = 10000;
    if (auto* config = RuntimeConfig::global()) {
        max_age_days = config->telemetry_history_max_age_days();
        max_rows = config->telemetry_history_max_rows();
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!db_) return;

    if (max_age_days > 0) {
        int64_t cutoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() -
            static_cast<int64_t>(max_age_days) * 24LL * 60LL * 60LL * 1000LL;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM telemetry_history WHERE timestamp_ms < ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, cutoff_ms);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                LOG(WARNING, "TelemetryHistory") << "Failed to prune by age: " << sqlite3_errmsg(db_) << std::endl;
            }
        }
        sqlite3_finalize(stmt);
    }

    if (max_rows > 0) {
        sqlite3_stmt* stmt = nullptr;
        static const char* kTrim =
            "DELETE FROM telemetry_history WHERE id NOT IN "
            "(SELECT id FROM telemetry_history ORDER BY id DESC LIMIT ?);";
        if (sqlite3_prepare_v2(db_, kTrim, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, max_rows);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                LOG(WARNING, "TelemetryHistory") << "Failed to prune by row cap: " << sqlite3_errmsg(db_) << std::endl;
            }
        }
        sqlite3_finalize(stmt);
    }
}

void TelemetryHistoryStore::prune_loop() {
    constexpr auto kPruneInterval = std::chrono::minutes(10);

    // Run an initial prune shortly after startup (bounded backlog from a
    // previous run with a stricter retention policy shouldn't wait 10 minutes
    // to shrink), then on the regular interval.
    std::unique_lock<std::mutex> lock(prune_mutex_);
    if (prune_cv_.wait_for(lock, std::chrono::seconds(30), [this] { return prune_shutdown_; })) {
        return;
    }
    lock.unlock();
    prune_once();

    while (true) {
        std::unique_lock<std::mutex> wait_lock(prune_mutex_);
        if (prune_cv_.wait_for(wait_lock, kPruneInterval, [this] { return prune_shutdown_; })) {
            return;
        }
        wait_lock.unlock();
        prune_once();
    }
}

} // namespace lemon
