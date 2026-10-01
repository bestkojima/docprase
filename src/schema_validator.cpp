#include "schema_validator.hpp"
#include "document_schemas.hpp"
#include <array>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

namespace dococr {
namespace {
using Json = nlohmann::json;
void check_supported_schema(const Json& rule) {
    static const std::set<std::string> supported = {
        "$schema", "$id", "title", "$defs", "$ref", "type", "const", "enum",
        "properties", "required", "additionalProperties", "pattern", "minLength",
        "minimum", "maximum", "exclusiveMinimum", "minItems", "maxItems",
        "uniqueItems", "items", "allOf", "anyOf", "oneOf", "if", "then"};
    if (!rule.is_object()) throw std::logic_error("DocumentIR Schema 节点无效");
    for (const auto& item : rule.items()) {
        if (!supported.count(item.key()))
            throw std::logic_error("DocumentIR Schema 含不支持关键字：" + item.key());
        if (item.key() == "properties" || item.key() == "$defs") {
            for (const auto& property : item.value().items()) check_supported_schema(property.value());
        } else if (item.key() == "items" || item.key() == "if" || item.key() == "then")
            check_supported_schema(item.value());
        else if (item.key() == "allOf" || item.key() == "anyOf" || item.key() == "oneOf")
            for (const auto& child : item.value()) check_supported_schema(child);
    }
}

bool matches_type(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer() || value.is_number_unsigned();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return false;
}

bool schema_matches(const Json& value, const Json& rule, const Json& root,
                    const std::string& path, std::string& error) {
    auto fail = [&](const std::string& detail) { error = path + " " + detail; return false; };
    if (rule.contains("$ref")) {
        const std::string ref = rule.at("$ref").get<std::string>();
        if (ref.rfind("#/$defs/", 0) != 0) return fail("不支持的 Schema $ref");
        return schema_matches(value, root.at("$defs").at(ref.substr(8)), root, path, error);
    }
    if (rule.contains("type")) {
        const auto& types = rule.at("type");
        bool allowed = types.is_string() ? matches_type(value, types.get<std::string>()) : false;
        if (types.is_array()) for (const auto& type : types)
            allowed = allowed || matches_type(value, type.get<std::string>());
        if (!allowed) return fail("类型不符合 Schema");
    }
    if (rule.contains("const") && value != rule.at("const")) return fail("不符合 Schema const");
    if (rule.contains("enum")) {
        bool allowed = false;
        for (const auto& option : rule.at("enum")) if (value == option) allowed = true;
        if (!allowed) return fail("不符合 Schema enum");
    }
    if (value.is_string()) {
        const auto& string = value.get_ref<const std::string&>();
        if (rule.contains("minLength") && string.size() < rule.at("minLength").get<size_t>())
            return fail("长度不足");
        if (rule.contains("pattern") &&
            !std::regex_search(string, std::regex(rule.at("pattern").get<std::string>())))
            return fail("不符合 Schema pattern");
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) return fail("数值不有限");
        if (rule.contains("minimum") && number < rule.at("minimum").get<double>())
            return fail("低于 minimum");
        if (rule.contains("maximum") && number > rule.at("maximum").get<double>())
            return fail("高于 maximum");
        if (rule.contains("exclusiveMinimum") && number <= rule.at("exclusiveMinimum").get<double>())
            return fail("低于 exclusiveMinimum");
    }
    if (value.is_array()) {
        if (rule.contains("minItems") && value.size() < rule.at("minItems").get<size_t>())
            return fail("数组少于 minItems");
        if (rule.contains("maxItems") && value.size() > rule.at("maxItems").get<size_t>())
            return fail("数组多于 maxItems");
        if (rule.value("uniqueItems", false)) {
            for (size_t i = 0; i < value.size(); ++i)
                for (size_t j = i + 1; j < value.size(); ++j)
                    if (value[i] == value[j]) return fail("数组包含重复项");
        }
        if (rule.contains("items")) for (size_t i = 0; i < value.size(); ++i)
            if (!schema_matches(value[i], rule.at("items"), root,
                                path + "[" + std::to_string(i) + "]", error)) return false;
    }
    if (value.is_object()) {
        if (rule.contains("required")) for (const auto& name : rule.at("required")) {
            const std::string key = name.get<std::string>();
            if (!value.contains(key)) return fail("缺少 " + key);
        }
        const Json* properties = rule.contains("properties") ? &rule.at("properties") : nullptr;
        for (const auto& item : value.items()) {
            if (properties && properties->contains(item.key())) {
                if (!schema_matches(item.value(), properties->at(item.key()), root,
                                    path + "." + item.key(), error)) return false;
            } else if (rule.contains("additionalProperties") && rule.at("additionalProperties") == false)
                return fail("不允许字段 " + item.key());
        }
    }
    if (rule.contains("allOf")) for (const auto& child : rule.at("allOf"))
        if (!schema_matches(value, child, root, path, error)) return false;
    if (rule.contains("anyOf")) {
        bool any = false;
        for (const auto& child : rule.at("anyOf")) {
            std::string ignored;
            if (schema_matches(value, child, root, path, ignored)) any = true;
        }
        if (!any) return fail("不符合任一 anyOf 条件");
    }
    if (rule.contains("oneOf")) {
        size_t matches = 0;
        for (const auto& child : rule.at("oneOf")) {
            std::string ignored;
            if (schema_matches(value, child, root, path, ignored)) ++matches;
        }
        if (matches != 1) return fail("不符合 oneOf 条件");
    }
    if (rule.contains("if")) {
        std::string ignored;
        if (schema_matches(value, rule.at("if"), root, path, ignored) && rule.contains("then"))
            if (!schema_matches(value, rule.at("then"), root, path, error)) return false;
    }
    return true;
}
}

void validate_document_schema(const Json& document, const std::string& version) {
    static const std::array<Json, 15> schemas = {
        Json::parse(document_schema_1_0), Json::parse(document_schema_1_1),
        Json::parse(document_schema_1_2), Json::parse(document_schema_1_3),
        Json::parse(document_schema_1_4), Json::parse(document_schema_1_5_image),
        Json::parse(document_schema_1_5_pdf), Json::parse(document_schema_1_6_image),
        Json::parse(document_schema_1_6_pdf), Json::parse(document_schema_1_7_image),
        Json::parse(document_schema_1_7_pdf), Json::parse(document_schema_1_8_image),
        Json::parse(document_schema_1_8_pdf), Json::parse(document_schema_1_9_image),
        Json::parse(document_schema_1_9_pdf)};
    const size_t index = version == "1.0" ? 0 : version == "1.1" ? 1 :
                         version == "1.2" ? 2 : version == "1.3" ? 3 :
                         version == "1.4" ? 4 :
                         version == "1.5" ? (document.at("source").at("type") == "pdf" ? 6 : 5) :
                         version == "1.6" ? (document.at("source").at("type") == "pdf" ? 8 : 7) :
                         version == "1.7" ? (document.at("source").at("type") == "pdf" ? 10 : 9) :
                         version == "1.8" ? (document.at("source").at("type") == "pdf" ? 12 : 11) :
                         document.at("source").at("type") == "pdf" ? 14 : 13;
    static const bool checked = [] {
        for (const auto& schema : schemas) check_supported_schema(schema);
        return true;
    }();
    (void)checked;
    std::string error;
    if (!schema_matches(document, schemas[index], schemas[index], "document", error))
        throw std::invalid_argument("DocumentIR Schema " + version + " 无效：" + error);
}
}
