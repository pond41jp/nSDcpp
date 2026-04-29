#include "routes.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "common/log.h"

#ifdef SD_USE_POSTGRESQL
#include <libpq-fe.h>
#endif

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct PostgresConfig {
    std::string host            = "127.0.0.1";
    int port                    = 5432;
    std::string username;
    std::string password;
    std::string dbname;
    std::string sslmode         = "prefer";
    std::string master_password = "nSDcpp-master";
};

std::mutex g_config_mutex;

static fs::path config_path() {
    return fs::current_path() / "nSDcpp.json";
}

static json to_config_json(const PostgresConfig& cfg) {
    return {
        {"postgresql",
         {
             {"host", cfg.host},
             {"port", cfg.port},
             {"username", cfg.username},
             {"password", cfg.password},
             {"dbname", cfg.dbname},
             {"sslmode", cfg.sslmode},
             {"master_password", cfg.master_password},
         }},
    };
}

static PostgresConfig config_from_json(const json& j) {
    PostgresConfig cfg;
    if (!j.contains("postgresql") || !j["postgresql"].is_object()) {
        return cfg;
    }
    const json& pg = j["postgresql"];
    cfg.host       = pg.value("host", cfg.host);
    cfg.port       = pg.value("port", cfg.port);
    cfg.username   = pg.value("username", cfg.username);
    cfg.password   = pg.value("password", cfg.password);
    cfg.dbname     = pg.value("dbname", cfg.dbname);
    cfg.sslmode    = pg.value("sslmode", cfg.sslmode);
    cfg.master_password = pg.value("master_password", cfg.master_password);
    return cfg;
}

static PostgresConfig load_config() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    const fs::path p = config_path();
    if (!fs::exists(p)) {
        return PostgresConfig{};
    }
    std::ifstream ifs(p);
    if (!ifs) {
        return PostgresConfig{};
    }
    json j;
    ifs >> j;
    return config_from_json(j);
}

static bool save_config(const PostgresConfig& cfg, std::string& error) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::ofstream ofs(config_path());
    if (!ofs) {
        error = "failed to open nSDcpp.json for write";
        return false;
    }
    ofs << std::setw(2) << to_config_json(cfg) << "\n";
    return true;
}

static std::vector<uint8_t> xor_stream_cipher(const std::vector<uint8_t>& input, const std::string& password) {
    std::vector<uint8_t> output(input.size(), 0);
    if (password.empty()) {
        return input;
    }
    size_t key_index = 0;
    uint64_t state   = static_cast<uint64_t>(std::hash<std::string>{}(password));
    for (size_t i = 0; i < input.size(); ++i) {
        if (key_index == 0) {
            state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        }
        const uint8_t k = static_cast<uint8_t>((state >> ((key_index % 8) * 8)) & 0xFF);
        output[i]        = input[i] ^ k;
        key_index        = (key_index + 1) % 8;
    }
    return output;
}

static std::string base64_encode_bytes(const std::vector<uint8_t>& bytes) {
    return base64_encode(bytes);
}

static bool base64_decode_bytes(const std::string& in, std::vector<uint8_t>& out) {
    static const std::string k_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> t(256, -1);
    for (int i = 0; i < 64; ++i) {
        t[k_chars[i]] = i;
    }
    int val = 0;
    int valb = -8;
    out.clear();
    for (unsigned char c : in) {
        if (t[c] == -1) {
            if (c == '=') {
                break;
            }
            continue;
        }
        val = (val << 6) + t[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

static std::string obfuscate_text(const std::string& plaintext, const std::string& password) {
    std::vector<uint8_t> bytes(plaintext.begin(), plaintext.end());
    const std::vector<uint8_t> obf = xor_stream_cipher(bytes, password);
    return base64_encode_bytes(obf);
}

static bool deobfuscate_text(const std::string& ciphertext, const std::string& password, std::string& plaintext) {
    std::vector<uint8_t> bytes;
    if (!base64_decode_bytes(ciphertext, bytes)) {
        return false;
    }
    const std::vector<uint8_t> deobf = xor_stream_cipher(bytes, password);
    plaintext.assign(deobf.begin(), deobf.end());
    return true;
}

#ifdef SD_USE_POSTGRESQL
static std::string sql_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

static std::string sql_text_array_literal(const std::vector<std::string>& tags) {
    std::string lit = "{";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) {
            lit += ",";
        }
        lit += "\"";
        for (char c : tags[i]) {
            if (c == '"' || c == '\\') {
                lit.push_back('\\');
            }
            lit.push_back(c);
        }
        lit += "\"";
    }
    lit += "}";
    return lit;
}

struct PgConnHolder {
    PGconn* conn = nullptr;
    ~PgConnHolder() {
        if (conn != nullptr) {
            PQfinish(conn);
        }
    }
};

static bool ensure_table(PGconn* conn, std::string& error) {
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS sdcpp_records ("
        "id BIGSERIAL PRIMARY KEY,"
        "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "rating DOUBLE PRECISION NOT NULL DEFAULT 0,"
        "tags TEXT[] NOT NULL DEFAULT '{}',"
        "prompt TEXT NOT NULL DEFAULT '',"
        "generation_params_json JSONB NOT NULL DEFAULT '{}'::jsonb,"
        "payload_text TEXT NOT NULL,"
        "is_encrypted BOOLEAN NOT NULL DEFAULT FALSE,"
        "has_password BOOLEAN NOT NULL DEFAULT FALSE,"
        "password_cipher TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_sdcpp_records_created_at ON sdcpp_records(created_at DESC);"
        "CREATE INDEX IF NOT EXISTS idx_sdcpp_records_rating ON sdcpp_records(rating DESC);"
        "CREATE INDEX IF NOT EXISTS idx_sdcpp_records_tags ON sdcpp_records USING GIN(tags);";

    PGresult* result = PQexec(conn, ddl);
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        error = PQerrorMessage(conn);
        PQclear(result);
        return false;
    }
    PQclear(result);
    return true;
}

static bool open_connection(const PostgresConfig& cfg, PgConnHolder& holder, std::string& error) {
    if (cfg.username.empty() || cfg.dbname.empty()) {
        error = "postgresql username/dbname is not configured";
        return false;
    }
    std::ostringstream conninfo;
    conninfo << "host=" << cfg.host
             << " port=" << cfg.port
             << " user=" << cfg.username
             << " password=" << cfg.password
             << " dbname=" << cfg.dbname
             << " sslmode=" << cfg.sslmode;
    holder.conn = PQconnectdb(conninfo.str().c_str());
    if (PQstatus(holder.conn) != CONNECTION_OK) {
        error = PQerrorMessage(holder.conn);
        return false;
    }
    return ensure_table(holder.conn, error);
}
#endif

static std::vector<std::string> collect_tags(const json& body) {
    std::vector<std::string> tags;
    if (body.contains("tags") && body["tags"].is_array()) {
        for (const auto& t : body["tags"]) {
            if (t.is_string()) {
                tags.push_back(t.get<std::string>());
            }
        }
    }
    std::string seed_tag = "seed:";
    if (body.contains("seed")) {
        seed_tag += body["seed"].dump();
    } else if (body.contains("generation_conditions") && body["generation_conditions"].is_object() &&
               body["generation_conditions"].contains("seed")) {
        seed_tag += body["generation_conditions"]["seed"].dump();
    } else {
        seed_tag += "unknown";
    }
    const auto it = std::find(tags.begin(), tags.end(), seed_tag);
    if (it == tags.end()) {
        tags.push_back(seed_tag);
    }
    return tags;
}

static json merge_generation_conditions_from_body(const json& body) {
    json cond = body.value("generation_conditions", json::object());
    if (!cond.is_object()) {
        cond = json::object();
    }

    const std::vector<std::string> scalar_keys = {
        "prompt",
        "negative_prompt",
        "width",
        "height",
        "clip_skip",
        "seed",
        "batch_count",
        "strength",
        "control_strength",
        "output_format",
        "output_compression",
        "cache_mode",
        "cache_option",
        "scm_mask",
        "scm_policy_dynamic",
        "sampler",
        "scheduler",
        "steps",
        "cfg_scale",
        "sample_steps",
        "eta",
        "shifted_timestep",
        "flow_shift",
        "video_frames",
        "fps",
        "moe_boundary",
        "vace_strength",
    };

    for (const auto& key : scalar_keys) {
        if (!cond.contains(key) && body.contains(key)) {
            cond[key] = body[key];
        }
    }

    const std::vector<std::string> object_keys = {
        "sample_params",
        "high_noise_sample_params",
        "vae_tiling_params",
        "guidance",
        "hires",
        "lora",
    };
    for (const auto& key : object_keys) {
        if (!cond.contains(key) && body.contains(key)) {
            cond[key] = body[key];
        }
    }

    return cond;
}

static bool body_has_generation_condition_fields(const json& body) {
    static const std::vector<std::string> keys = {
        "generation_conditions", "prompt", "negative_prompt", "width", "height", "clip_skip",
        "seed", "batch_count", "strength", "output_format", "output_compression", "sampler",
        "scheduler", "steps", "sample_params", "high_noise_sample_params", "vae_tiling_params",
        "guidance", "hires", "lora"
    };
    for (const auto& key : keys) {
        if (body.contains(key)) {
            return true;
        }
    }
    return false;
}

static void register_unavailable_postgres_endpoints(httplib::Server& svr) {
    auto unavailable = [](const httplib::Request&, httplib::Response& res) {
        res.status = 501;
        res.set_content(R"({"error":"PostgreSQL support is not enabled at build time. Rebuild with -DSD_POSTGRESQL=ON"})",
                        "application/json");
    };

    svr.Post("/sdcpp/v1/postgres/config", unavailable);
    svr.Post("/sdcpp/v1/postgres/records", unavailable);
    svr.Post("/sdcpp/v1/postgres/query", unavailable);
    svr.Patch(R"(/sdcpp/v1/postgres/records/(\d+))", unavailable);
    svr.Delete(R"(/sdcpp/v1/postgres/records/(\d+))", unavailable);
}

}  // namespace

void register_postgres_endpoints(httplib::Server& svr, ServerRuntime&) {
#ifndef SD_USE_POSTGRESQL
    register_unavailable_postgres_endpoints(svr);
#else
    svr.Post("/sdcpp/v1/postgres/config", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body       = json::parse(req.body.empty() ? "{}" : req.body);
            PostgresConfig cfg = load_config();
            cfg.host           = body.value("host", cfg.host);
            cfg.port           = body.value("port", cfg.port);
            cfg.username       = body.value("username", cfg.username);
            cfg.password       = body.value("password", cfg.password);
            cfg.dbname         = body.value("dbname", cfg.dbname);
            cfg.sslmode        = body.value("sslmode", cfg.sslmode);
            cfg.master_password = body.value("master_password", cfg.master_password);

            std::string error;
            if (!save_config(cfg, error)) {
                res.status = 500;
                res.set_content(json({{"error", error}}).dump(), "application/json");
                return;
            }

            PgConnHolder conn;
            if (!open_connection(cfg, conn, error)) {
                res.status = 400;
                res.set_content(json({{"error", "connection failed"}, {"message", error}}).dump(), "application/json");
                return;
            }
            res.status = 200;
            res.set_content(json({
                {"ok", true},
                {"host", cfg.host},
                {"port", cfg.port},
                {"username", cfg.username},
                {"dbname", cfg.dbname},
            }).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", "invalid request"}, {"message", e.what()}}).dump(), "application/json");
        }
    });

    svr.Post("/sdcpp/v1/postgres/records", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            PostgresConfig cfg = load_config();
            std::string error;
            PgConnHolder holder;
            if (!open_connection(cfg, holder, error)) {
                res.status = 400;
                res.set_content(json({{"error", error}}).dump(), "application/json");
                return;
            }

            std::vector<std::string> tags = collect_tags(body);
            json generation_conditions = merge_generation_conditions_from_body(body);
            json payload = {
                {"image_base64", body.value("image_base64", "")},
                {"comment", body.value("comment", "")},
                {"rating", body.value("rating", 0.0)},
                {"tags", tags},
                {"generation_conditions", generation_conditions},
                {"seed", body.contains("seed") ? body["seed"] : generation_conditions.value("seed", json(nullptr))},
                {"timings",
                 {
                     {"total_generation_sec", body.value("total_generation_sec", 0.0)},
                     {"tokenizer_sec", body.value("tokenizer_sec", 0.0)},
                     {"sampler_total_sec", body.value("sampler_total_sec", 0.0)},
                     {"sampler_avg_its", body.value("sampler_avg_its", 0.0)},
                     {"decode_sec", body.value("decode_sec", 0.0)},
                 }},
            };

            const std::string image_password = body.value("image_password", "");
            const bool has_password          = !image_password.empty();
            const bool encrypted             = has_password;
            std::string payload_text         = payload.dump();
            std::string password_cipher;
            if (has_password) {
                payload_text = obfuscate_text(payload_text, image_password);
                password_cipher = obfuscate_text(image_password, cfg.master_password);
            }

            std::string prompt;
            if (payload["generation_conditions"].is_object()) {
                prompt = payload["generation_conditions"].value("prompt", "");
            }

            const std::string generation_json = generation_conditions.dump();
            const std::string sql =
                "INSERT INTO sdcpp_records "
                "(rating, tags, prompt, generation_params_json, payload_text, is_encrypted, has_password, password_cipher) VALUES ("
                + std::to_string(payload.value("rating", 0.0)) + ","
                + sql_quote(sql_text_array_literal(tags)) + "::text[],"
                + sql_quote(prompt) + ","
                + sql_quote(generation_json) + "::jsonb,"
                + sql_quote(payload_text) + ","
                + (encrypted ? "TRUE" : "FALSE") + ","
                + (has_password ? "TRUE" : "FALSE") + ","
                + (has_password ? sql_quote(password_cipher) : "NULL")
                + ") RETURNING id, created_at;";

            PGresult* result = PQexec(holder.conn, sql.c_str());
            if (PQresultStatus(result) != PGRES_TUPLES_OK) {
                error = PQerrorMessage(holder.conn);
                PQclear(result);
                res.status = 500;
                res.set_content(json({{"error", "insert failed"}, {"message", error}}).dump(), "application/json");
                return;
            }
            json out = {
                {"id", std::stoll(PQgetvalue(result, 0, 0))},
                {"created_at", PQgetvalue(result, 0, 1)},
                {"encrypted", encrypted},
                {"has_password", has_password},
            };
            PQclear(result);
            res.status = 201;
            res.set_content(out.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", "invalid request"}, {"message", e.what()}}).dump(), "application/json");
        }
    });

    svr.Post("/sdcpp/v1/postgres/query", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            PostgresConfig cfg = load_config();
            std::string error;
            PgConnHolder holder;
            if (!open_connection(cfg, holder, error)) {
                res.status = 400;
                res.set_content(json({{"error", error}}).dump(), "application/json");
                return;
            }

            std::vector<std::string> where;
            if (body.contains("tags_and") && body["tags_and"].is_array() && !body["tags_and"].empty()) {
                std::vector<std::string> tags;
                for (const auto& t : body["tags_and"]) {
                    if (t.is_string()) {
                        tags.push_back(t.get<std::string>());
                    }
                }
                where.push_back("tags @> " + sql_quote(sql_text_array_literal(tags)) + "::text[]");
            }
            if (body.value("password_protected_only", false)) {
                where.push_back("has_password = TRUE");
            }
            const std::string prompt_keyword = body.value("prompt_keyword", "");
            if (!prompt_keyword.empty()) {
                where.push_back("prompt ILIKE " + sql_quote("%" + prompt_keyword + "%"));
            }
            if (body.contains("same_generation_as_id")) {
                const int64_t ref_id = body["same_generation_as_id"].get<int64_t>();
                where.push_back("(generation_params_json - 'prompt') = ((SELECT generation_params_json FROM sdcpp_records WHERE id = " + std::to_string(ref_id) + ") - 'prompt')");
            }

            std::string order_column = "created_at";
            if (body.value("order_by", std::string("generation")) == "rating") {
                order_column = "rating";
            }
            std::string order_dir = body.value("order", std::string("desc")) == "asc" ? "ASC" : "DESC";
            int limit             = std::clamp(body.value("limit", 50), 1, 500);

            std::string sql =
                "SELECT id, created_at, rating, tags::text, prompt, payload_text, is_encrypted, has_password, password_cipher "
                "FROM sdcpp_records";
            if (!where.empty()) {
                sql += " WHERE ";
                for (size_t i = 0; i < where.size(); ++i) {
                    if (i > 0) {
                        sql += " AND ";
                    }
                    sql += where[i];
                }
            }
            sql += " ORDER BY " + order_column + " " + order_dir + " LIMIT " + std::to_string(limit);

            PGresult* result = PQexec(holder.conn, sql.c_str());
            if (PQresultStatus(result) != PGRES_TUPLES_OK) {
                error = PQerrorMessage(holder.conn);
                PQclear(result);
                res.status = 500;
                res.set_content(json({{"error", "query failed"}, {"message", error}}).dump(), "application/json");
                return;
            }

            json passwords = body.value("passwords", json::object());
            json rows      = json::array();
            for (int i = 0; i < PQntuples(result); ++i) {
                const int64_t id      = std::stoll(PQgetvalue(result, i, 0));
                const bool encrypted  = std::string(PQgetvalue(result, i, 6)) == "t";
                const bool has_pwd    = std::string(PQgetvalue(result, i, 7)) == "t";
                const std::string raw = PQgetvalue(result, i, 5);

                json row = {
                    {"id", id},
                    {"created_at", PQgetvalue(result, i, 1)},
                    {"rating", std::stod(PQgetvalue(result, i, 2))},
                    {"tags_text", PQgetvalue(result, i, 3)},
                    {"prompt", PQgetvalue(result, i, 4)},
                    {"encrypted", encrypted},
                    {"has_password", has_pwd},
                };

                if (!encrypted) {
                    row["payload"] = json::parse(raw, nullptr, false);
                } else {
                    std::string given_password;
                    const std::string id_key = std::to_string(id);
                    if (passwords.contains(id_key) && passwords[id_key].is_string()) {
                        given_password = passwords[id_key].get<std::string>();
                    }
                    std::string encrypted_password = PQgetisnull(result, i, 8) ? "" : PQgetvalue(result, i, 8);
                    std::string stored_password;
                    bool ok_master = deobfuscate_text(encrypted_password, cfg.master_password, stored_password);
                    if (!ok_master || given_password != stored_password) {
                        row["payload"] = nullptr;
                        row["error"]   = "password required or invalid";
                    } else {
                        std::string plain_payload;
                        if (!deobfuscate_text(raw, given_password, plain_payload)) {
                            row["payload"] = nullptr;
                            row["error"]   = "failed to decrypt payload";
                        } else {
                            row["payload"] = json::parse(plain_payload, nullptr, false);
                        }
                    }
                }
                rows.push_back(std::move(row));
            }
            PQclear(result);
            res.status = 200;
            res.set_content(json({{"records", rows}}).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", "invalid request"}, {"message", e.what()}}).dump(), "application/json");
        }
    });

    svr.Patch(R"(/sdcpp/v1/postgres/records/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const int64_t id = std::stoll(req.matches[1]);
            json body = json::parse(req.body.empty() ? "{}" : req.body);
            PostgresConfig cfg = load_config();
            std::string error;
            PgConnHolder holder;
            if (!open_connection(cfg, holder, error)) {
                res.status = 400;
                res.set_content(json({{"error", error}}).dump(), "application/json");
                return;
            }

            const std::string select_sql =
                "SELECT payload_text, is_encrypted, has_password, password_cipher FROM sdcpp_records WHERE id = "
                + std::to_string(id);
            PGresult* selected = PQexec(holder.conn, select_sql.c_str());
            if (PQresultStatus(selected) != PGRES_TUPLES_OK || PQntuples(selected) == 0) {
                PQclear(selected);
                res.status = 404;
                res.set_content(R"({"error":"record not found"})", "application/json");
                return;
            }

            std::string payload_text = PQgetvalue(selected, 0, 0);
            const bool encrypted     = std::string(PQgetvalue(selected, 0, 1)) == "t";
            std::string password_cipher = PQgetisnull(selected, 0, 3) ? "" : PQgetvalue(selected, 0, 3);
            PQclear(selected);

            std::string current_password = body.value("current_password", "");
            std::string new_password     = body.value("image_password", "");
            json payload_json;
            if (encrypted) {
                std::string stored_password;
                if (!deobfuscate_text(password_cipher, cfg.master_password, stored_password) ||
                    stored_password != current_password) {
                    res.status = 403;
                    res.set_content(R"({"error":"invalid current_password"})", "application/json");
                    return;
                }
                std::string plain;
                if (!deobfuscate_text(payload_text, current_password, plain)) {
                    res.status = 500;
                    res.set_content(R"({"error":"failed to decrypt existing payload"})", "application/json");
                    return;
                }
                payload_json = json::parse(plain);
            } else {
                payload_json = json::parse(payload_text);
            }

            for (auto it = body.begin(); it != body.end(); ++it) {
                if (it.key() == "current_password" || it.key() == "image_password") {
                    continue;
                }
                payload_json[it.key()] = it.value();
            }

            if (body_has_generation_condition_fields(body)) {
                json merged_source = body;
                if (payload_json.contains("generation_conditions")) {
                    merged_source["generation_conditions"] = payload_json["generation_conditions"];
                }
                payload_json["generation_conditions"] = merge_generation_conditions_from_body(merged_source);
            }

            std::vector<std::string> tags = collect_tags(payload_json);
            payload_json["tags"] = tags;
            const std::string prompt = payload_json.value("generation_conditions", json::object()).value("prompt", "");
            const std::string generation_json = payload_json.value("generation_conditions", json::object()).dump();

            bool next_encrypted = !new_password.empty();
            if (!next_encrypted && encrypted && !current_password.empty()) {
                next_encrypted = false;
            }
            std::string next_payload_text = payload_json.dump();
            std::string next_password_cipher;
            if (next_encrypted) {
                next_payload_text = obfuscate_text(next_payload_text, new_password);
                next_password_cipher = obfuscate_text(new_password, cfg.master_password);
            }

            const std::string update_sql =
                "UPDATE sdcpp_records SET "
                "rating = " + std::to_string(payload_json.value("rating", 0.0)) + ", "
                "tags = " + sql_quote(sql_text_array_literal(tags)) + "::text[], "
                "prompt = " + sql_quote(prompt) + ", "
                "generation_params_json = " + sql_quote(generation_json) + "::jsonb, "
                "payload_text = " + sql_quote(next_payload_text) + ", "
                "is_encrypted = " + (next_encrypted ? "TRUE" : "FALSE") + ", "
                "has_password = " + (next_encrypted ? "TRUE" : "FALSE") + ", "
                "password_cipher = " + (next_encrypted ? sql_quote(next_password_cipher) : "NULL")
                + " WHERE id = " + std::to_string(id);

            PGresult* updated = PQexec(holder.conn, update_sql.c_str());
            if (PQresultStatus(updated) != PGRES_COMMAND_OK) {
                error = PQerrorMessage(holder.conn);
                PQclear(updated);
                res.status = 500;
                res.set_content(json({{"error", "update failed"}, {"message", error}}).dump(), "application/json");
                return;
            }
            PQclear(updated);
            res.status = 200;
            res.set_content(json({{"ok", true}, {"id", id}}).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", "invalid request"}, {"message", e.what()}}).dump(), "application/json");
        }
    });

    svr.Delete(R"(/sdcpp/v1/postgres/records/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const int64_t id = std::stoll(req.matches[1]);
            PostgresConfig cfg = load_config();
            std::string error;
            PgConnHolder holder;
            if (!open_connection(cfg, holder, error)) {
                res.status = 400;
                res.set_content(json({{"error", error}}).dump(), "application/json");
                return;
            }
            const std::string sql = "DELETE FROM sdcpp_records WHERE id = " + std::to_string(id);
            PGresult* result = PQexec(holder.conn, sql.c_str());
            if (PQresultStatus(result) != PGRES_COMMAND_OK) {
                error = PQerrorMessage(holder.conn);
                PQclear(result);
                res.status = 500;
                res.set_content(json({{"error", "delete failed"}, {"message", error}}).dump(), "application/json");
                return;
            }
            const int affected = std::atoi(PQcmdTuples(result));
            PQclear(result);
            if (affected == 0) {
                res.status = 404;
                res.set_content(R"({"error":"record not found"})", "application/json");
                return;
            }
            res.status = 200;
            res.set_content(json({{"ok", true}, {"id", id}}).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", "invalid request"}, {"message", e.what()}}).dump(), "application/json");
        }
    });
#endif
}
