#include "config.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dococr {
namespace {
struct Json {
    enum Kind { Null, Bool, Number, String, Object, Array } kind = Null;
    std::string scalar;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    const Json& at(const std::string& key) const {
        auto it = object.find(key);
        if (it == object.end()) throw ConfigError("missing_field", key);
        return it->second;
    }
};
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    Json parse() { auto value = parse_value(); ws(); if (i_ != s_.size()) fail(); return value; }
private:
    const std::string& s_;
    size_t i_ = 0;
    unsigned depth_ = 0;
    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    [[noreturn]] void fail() const { throw ConfigError("invalid_json", "offset " + std::to_string(i_)); }
    char take() { if (i_ == s_.size()) fail(); return s_[i_++]; }
    unsigned hex4() {
        unsigned value = 0;
        for (int j = 0; j < 4; ++j) {
            char c = take();
            unsigned digit = c >= '0' && c <= '9' ? unsigned(c - '0') :
                c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10) :
                c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10) : 16;
            if (digit == 16) fail();
            value = value * 16 + digit;
        }
        return value;
    }
    void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
        } else {
            out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
            out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
        }
    }
    std::string string() {
        if (take() != '"') fail();
        std::string out;
        while (i_ < s_.size()) {
            char c = take();
            if (c == '"') {
                for (size_t j = 0; j < out.size();) {
                    unsigned char b = static_cast<unsigned char>(out[j]);
                    if (b < 0x80) { ++j; continue; }
                    int n = b >= 0xc2 && b <= 0xdf ? 2 : b >= 0xe0 && b <= 0xef ? 3 :
                            b >= 0xf0 && b <= 0xf4 ? 4 : 0;
                    if (!n || j + size_t(n) > out.size()) fail();
                    unsigned char next = static_cast<unsigned char>(out[j+1]);
                    if ((next & 0xc0) != 0x80 || (b == 0xe0 && next < 0xa0) ||
                        (b == 0xed && next >= 0xa0) || (b == 0xf0 && next < 0x90) ||
                        (b == 0xf4 && next >= 0x90)) fail();
                    for (int x = 2; x < n; ++x)
                        if ((static_cast<unsigned char>(out[j+x]) & 0xc0) != 0x80) fail();
                    j += size_t(n);
                }
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) fail();
            if (c == '\\') {
                c = take();
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (take() != '\\' || take() != 'u') fail();
                        unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail();
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) fail();
                    append_utf8(out, cp);
                    continue;
                }
                default: fail();
                }
            }
            out += c;
        }
        fail();
    }
    Json parse_value() {
        ws(); if (i_ == s_.size()) fail();
        Json v;
        if (s_[i_] == '{') {
            if (++depth_ > 64) throw ConfigError("json_too_deep", "nesting exceeds 64");
            v.kind = Json::Object; ++i_; ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; --depth_; return v; }
            for (;;) {
                ws(); if (i_ == s_.size() || s_[i_] != '"') fail();
                std::string key = string(); ws(); if (take() != ':') fail();
                if (!v.object.emplace(key, parse_value()).second) throw ConfigError("duplicate_field", key);
                ws(); char c = take(); if (c == '}') { --depth_; return v; } if (c != ',') fail();
            }
        }
        if (s_[i_] == '[') {
            if (++depth_ > 64) throw ConfigError("json_too_deep", "nesting exceeds 64");
            v.kind = Json::Array; ++i_; ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; --depth_; return v; }
            for (;;) { v.array.push_back(parse_value()); ws(); char c = take(); if (c == ']') { --depth_; return v; } if (c != ',') fail(); }
        }
        if (s_[i_] == '"') { v.kind = Json::String; v.scalar = string(); return v; }
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; v.kind = Json::Bool; v.scalar = "true"; return v; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.kind = Json::Bool; v.scalar = "false"; return v; }
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return v; }
        size_t start = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ == s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) fail();
        if (s_[i_] == '0') ++i_;
        else while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_; if (i_ == s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) fail();
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_; if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ == s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) fail();
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        v.kind = Json::Number; v.scalar = s_.substr(start, i_ - start); return v;
    }
};
void object(const Json& v, std::initializer_list<const char*> fields) {
    if (v.kind != Json::Object) throw ConfigError("type_error", "expected object");
    std::set<std::string> allowed(fields.begin(), fields.end());
    for (const auto& pair : v.object)
        if (!allowed.count(pair.first)) throw ConfigError("unknown_field", pair.first);
}
void array(const Json& v) { if (v.kind != Json::Array) throw ConfigError("type_error", "expected array"); }
std::string str(const Json& v) {
    if (v.kind != Json::String) throw ConfigError("type_error", "expected string");
    return v.scalar;
}
bool boolean(const Json& v) {
    if (v.kind != Json::Bool) throw ConfigError("type_error", "expected boolean");
    return v.scalar == "true";
}
uint64_t positive(const Json& v, const std::string& name) {
    if (v.kind != Json::Number || v.scalar.empty() || v.scalar[0] == '-' ||
        v.scalar.find_first_of(".eE") != std::string::npos)
        throw ConfigError("invalid_budget", name);
    uint64_t result;
    try { result = std::stoull(v.scalar); } catch (...) { throw ConfigError("invalid_budget", name); }
    if (result == 0) throw ConfigError("invalid_budget", name);
    return result;
}
std::string dump(const Json& v) {
    if (v.kind == Json::Null) return "null";
    if (v.kind == Json::Bool || v.kind == Json::Number) return v.scalar;
    if (v.kind == Json::String) return json_quote(v.scalar);
    std::string out = v.kind == Json::Object ? "{" : "[";
    bool first = true;
    if (v.kind == Json::Object) for (const auto& pair : v.object) {
        if (!first) out += ','; first = false;
        out += json_quote(pair.first) + ":" + dump(pair.second);
    }
    if (v.kind == Json::Array) for (const auto& item : v.array) {
        if (!first) out += ','; first = false; out += dump(item);
    }
    return out + (v.kind == Json::Object ? "}" : "]");
}
std::string artifact_path(const std::string& root, const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p = fs::u8path(path);
    if (path.empty() || path.find('\0') != std::string::npos || p.is_absolute())
        throw ConfigError("invalid_artifact_path", path);
    for (const auto& segment : p) if (segment == ".." || segment == ".")
        throw ConfigError("invalid_artifact_path", path);
    std::error_code ec;
    fs::path base = fs::canonical(fs::u8path(root), ec);
    if (ec) throw ConfigError("artifact_missing", root);
    fs::path actual = fs::canonical(base / p, ec);
    if (ec) throw ConfigError("artifact_missing", path);
    auto b = base.begin(), a = actual.begin();
    for (; b != base.end(); ++b, ++a)
        if (a == actual.end() || *a != *b) throw ConfigError("invalid_artifact_path", path);
    if (!fs::is_regular_file(actual)) throw ConfigError("artifact_missing", path);
    return actual.u8string();
}
const std::map<std::string, std::pair<std::string, bool>> processors = {
    {"decode", {"adapter", true}}, {"crop", {"adapter", true}},
    {"normalize", {"adapter", true}}, {"session_reset", {"runtime", true}},
    {"rgb_identity", {"none", true}}, {"deskew", {"none", false}}
};
const std::map<std::string, std::pair<std::string, std::string>> flow_types = {
    {"page_loader", {"source", "page"}},
    {"layout.detect", {"page", "layout_blocks"}},
    {"ocr.transcribe", {"layout_blocks", "block_results"}},
    {"document_assembler", {"block_results", "document"}},
    {"document_exporter", {"document", "output"}}
};
} // namespace

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}

std::shared_ptr<const ExecutionPlan> build_plan(const std::string& text, bool fixture_build) {
    Json config = Parser(text).parse();
    object(config, {"schema_version", "mode", "backend", "models", "skills", "flow", "processing", "platform", "execution"});
    if (str(config.at("schema_version")) != "1.0") throw ConfigError("unsupported_schema", "schema_version");
    auto plan = std::make_shared<ExecutionPlan>();
    plan->mode = str(config.at("mode"));
    if (plan->mode != "development" && plan->mode != "production") throw ConfigError("invalid_mode", plan->mode);
    plan->backend = str(config.at("backend"));
    const bool real_pair = plan->backend == "mnn:pp-doclayout-v3+ovisocr2";
    plan->layout_only = plan->backend == "mnn:pp-doclayout-v3" ||
                        plan->backend == "fixture:layout_dedup" ||
                        plan->backend == "fixture:layout_dedup_edges" ||
                        plan->backend == "fixture:layout_geometry" ||
                        plan->backend == "fixture:layout_smartresize" ||
                        plan->backend == "fixture:layout_tuning" ||
                        plan->backend == "fixture:layout_contract" ||
                        plan->backend == "fixture:layout_table" ||
                        plan->backend == "fixture:layout_inline_formula" ||
                        plan->backend == "fixture:layout_empty" ||
                        plan->backend == "fixture:layout_infer_failure";
    if (plan->backend.rfind("fixture:", 0) == 0 && !fixture_build)
        throw ConfigError("unsupported_backend", plan->backend);
    if (plan->backend != "none" && plan->backend.rfind("fixture:", 0) != 0 && !plan->layout_only && !real_pair)
        throw ConfigError("unsupported_backend", plan->backend);
    if (plan->backend == "none") throw ConfigError("missing_capability", "backend has no inference capabilities");
    if (plan->mode == "production" && plan->backend.rfind("fixture:", 0) == 0)
        throw ConfigError("contract_unverified", "fixture is never a production backend");
    object(config.at("models"), {"layout", "recognition"});
    if (plan->layout_only && config.at("models").object.count("recognition"))
        throw ConfigError("unsupported_model", "recognition is not available in layout-only plan");
    for (const auto& name : {"layout", "recognition"}) {
        if (plan->layout_only && std::string(name) == "recognition") continue;
        const Json& model = config.at("models").at(name);
        object(model, {"contract_status", "root", "artifacts"});
        std::string status = str(model.at("contract_status"));
        if (status != "verified_fixture" && status != "pending_probe" && status != "contract_verified")
            throw ConfigError("invalid_contract_status", name);
        if (plan->mode == "production" && (status != "contract_verified" || plan->backend == "none"))
            throw ConfigError("contract_unverified", name);
        if (plan->backend.rfind("fixture:", 0) == 0 && status != "verified_fixture")
            throw ConfigError("contract_unverified", name);
        if ((plan->backend == "mnn:pp-doclayout-v3" || real_pair) && status != "contract_verified")
            throw ConfigError("contract_unverified", name);
        if (status == "verified_fixture" && plan->backend.rfind("fixture:", 0) != 0)
            throw ConfigError("contract_unverified", name);
        array(model.at("artifacts"));
        if (model.at("artifacts").array.empty()) throw ConfigError("artifact_missing", name);
        std::string root = str(model.at("root"));
        for (const Json& a : model.at("artifacts").array) {
            object(a, {"path", "sha256"});
            std::string path = str(a.at("path"));
            std::string expected = str(a.at("sha256"));
            std::string resolved = artifact_path(root, path);
            std::string actual = sha256_file(resolved);
            if (expected != actual) throw ConfigError("artifact_hash_mismatch", path);
            plan->artifacts.push_back({name, resolved, actual, status});
        }
    }
    object(config.at("skills"), {"layout.detect", "ocr.transcribe"});
    if (str(config.at("skills").at("layout.detect")) != "layout" ||
        (!plan->layout_only && str(config.at("skills").at("ocr.transcribe")) != "recognition") ||
        (plan->layout_only && config.at("skills").object.count("ocr.transcribe")))
        throw ConfigError("missing_capability", "skill binding");
    array(config.at("flow"));
    std::map<std::string, std::string> outputs;
    std::set<std::string> ids;
    std::string final_output;
    std::string resolved_flow = "[";
    for (const auto& node : config.at("flow").array) {
        object(node, {"id", "processor", "skill", "depends_on"});
        std::string id = str(node.at("id"));
        if (!ids.insert(id).second) throw ConfigError("duplicate_node", id);
        bool has_processor = node.object.count("processor") != 0;
        if (has_processor == (node.object.count("skill") != 0)) throw ConfigError("invalid_node", id);
        std::string type = str(node.at(has_processor ? "processor" : "skill"));
        auto contract = flow_types.find(type);
        if (contract == flow_types.end()) throw ConfigError("unknown_processor", type);
        if (!has_processor && !config.at("skills").object.count(type))
            throw ConfigError("missing_capability", type);
        array(node.at("depends_on"));
        std::string input = "source";
        if (node.at("depends_on").array.size() > 1) throw ConfigError("type_disconnected", id);
        if (!node.at("depends_on").array.empty()) {
            std::string dependency = str(node.at("depends_on").array[0]);
            if (dependency == id || !outputs.count(dependency))
                throw ConfigError("cycle_or_missing_dependency", id);
            input = outputs.at(dependency);
        }
        if (input != contract->second.first &&
            !(plan->layout_only && type == "document_assembler" && input == "layout_blocks"))
            throw ConfigError("type_disconnected", id);
        outputs[id] = final_output = contract->second.second;
        if (resolved_flow.size() > 1) resolved_flow += ',';
        resolved_flow += "{\"id\":" + json_quote(id) + ",\"binding\":" + json_quote(type) +
            ",\"input_kind\":" + json_quote(input) + ",\"output_kind\":" +
            json_quote(final_output) + "}";
    }
    if (config.at("flow").array.size() != flow_types.size() - (plan->layout_only ? 1 : 0) ||
        outputs.empty() || final_output != "output")
        throw ConfigError("missing_capability", "required flow");
    resolved_flow += ']';
    array(config.at("processing"));
    std::set<std::string> steps;
    for (const auto& item : config.at("processing").array) {
        object(item, {"id", "owner", "enabled"});
        std::string id = str(item.at("id")), owner = str(item.at("owner"));
        auto registered = processors.find(id);
        if (registered == processors.end()) throw ConfigError("unknown_processor", id);
        if (!steps.insert(id).second) throw ConfigError("duplicate_processing", id);
        bool enabled = boolean(item.at("enabled"));
        std::string expected_owner = registered->second.first;
        if (id == "session_reset" && plan->layout_only) expected_owner = "none";
        if (id == "normalize" && real_pair) expected_owner = "adapter";
        if (id == "normalize" && plan->backend == "fixture:runtime") expected_owner = "runtime";
        if (id == "normalize" && plan->backend == "fixture:graph") expected_owner = "graph";
        if (owner != expected_owner) throw ConfigError("processing_owner_mismatch", id);
        if (registered->second.second && !enabled && !(id == "session_reset" && plan->layout_only))
            throw ConfigError("required_processing_disabled", id);
        if (id == "session_reset" && plan->layout_only && enabled)
            throw ConfigError("unsupported_processing", id);
        if (!registered->second.second && enabled) throw ConfigError("unsupported_processing", id);
        plan->processing.push_back({id, owner, enabled});
    }
    if (steps.size() != processors.size()) throw ConfigError("missing_required_processing", "processing chain");
    const std::vector<std::string> processing_order = {
        "decode", "rgb_identity", "normalize", "crop", "session_reset", "deskew"};
    std::sort(plan->processing.begin(), plan->processing.end(), [&](const ProcessingStep& a, const ProcessingStep& b) {
        auto position = [&](const std::string& id) {
            return std::find(processing_order.begin(), processing_order.end(), id);
        };
        return position(a.id) < position(b.id);
    });
    object(config.at("platform"), {"device", "threads"});
    plan->device = str(config.at("platform").at("device"));
    if (plan->device != "cpu") throw ConfigError("unsupported_device", plan->device);
    uint64_t threads = positive(config.at("platform").at("threads"), "threads");
    if (threads != 1) throw ConfigError("unsupported_parameter", "threads: only 1 is implemented");
    plan->threads = 1;
    object(config.at("execution"), {"max_page_pixels", "max_output_bytes", "max_new_tokens", "generation_timeout_ms",
                                   "layout_preprocess", "layout_score_threshold"});
    const auto& execution = config.at("execution").object;
    if (execution.count("layout_preprocess")) {
        plan->layout_preprocess = str(execution.at("layout_preprocess"));
        if (!plan->uses_doclayout() || (plan->layout_preprocess != "auto" &&
            plan->layout_preprocess != "reference" && plan->layout_preprocess != "smartresize_bilinear" &&
            plan->layout_preprocess != "smartresize_area" && plan->layout_preprocess != "smartresize_lanczos"))
            throw ConfigError("invalid_layout_parameter", "layout_preprocess");
    }
    if (execution.count("layout_score_threshold")) {
        const auto& value = execution.at("layout_score_threshold");
        if (value.kind != Json::Number || !plan->uses_doclayout())
            throw ConfigError("invalid_layout_parameter", "layout_score_threshold");
        std::istringstream threshold(value.scalar);
        threshold.imbue(std::locale::classic());
        if (!(threshold >> plan->layout_score_threshold) || threshold.peek() != EOF)
            throw ConfigError("invalid_layout_parameter", "layout_score_threshold");
        if (!std::isfinite(plan->layout_score_threshold) || plan->layout_score_threshold < 0 ||
            plan->layout_score_threshold > 1)
            throw ConfigError("invalid_layout_parameter", "layout_score_threshold");
    }
    plan->max_page_pixels = positive(config.at("execution").at("max_page_pixels"), "max_page_pixels");
    if (plan->max_page_pixels > (plan->uses_doclayout() ? max_layout_source_pixels : 16000000))
        throw ConfigError("unsupported_parameter", "max_page_pixels exceeds decoder limit");
    plan->max_output_bytes = positive(config.at("execution").at("max_output_bytes"), "max_output_bytes");
    if (plan->max_output_bytes > 67108864) throw ConfigError("unsupported_parameter", "max_output_bytes exceeds decoder limit");
    plan->max_new_tokens = positive(config.at("execution").at("max_new_tokens"), "max_new_tokens");
    if (!plan->layout_only && plan->uses_doclayout() && plan->max_new_tokens > 4096)
        throw ConfigError("unsupported_parameter", "max_new_tokens exceeds Ovis runtime limit");
    if (config.at("execution").object.count("generation_timeout_ms")) {
        plan->generation_timeout_ms = positive(config.at("execution").at("generation_timeout_ms"), "generation_timeout_ms");
        if (plan->layout_only || !plan->uses_doclayout() || plan->generation_timeout_ms > 120000)
            throw ConfigError("unsupported_parameter", "generation_timeout_ms exceeds region generation limit");
    }
    plan->config_hash = sha256(dump(config));
    std::string resolved_processing = "[";
    for (const auto& step : plan->processing) {
        if (resolved_processing.size() > 1) resolved_processing += ',';
        resolved_processing += "{\"id\":" + json_quote(step.id) + ",\"owner\":" +
            json_quote(step.owner) + ",\"enabled\":" + (step.enabled ? "true" : "false") + "}";
    }
    resolved_processing += ']';
    plan->json = "{\"schema_version\":\"1.0\",\"config_hash\":" + json_quote(plan->config_hash) +
        ",\"effective_config\":" + dump(config) +
        ",\"resolved_flow\":" + resolved_flow +
        ",\"resolved_processing\":" + resolved_processing + "}";
    return plan;
}
} // namespace dococr
