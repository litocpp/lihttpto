module;
#include <rstd/macro.hpp>
export module lihttpto:cache;
export import :header;
export import :message;
export import :date;
export import :validator;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
auto delta_seconds(ref<str> text) -> Option<i64> {
    if (text.is_empty()) return None();
    i64 value {};
    for (auto byte : text.as_bytes()) {
        if (byte < u8('0') || byte > u8('9')) return None();
        value =
            rstd::min(i64(2147483648ll), value * i64(10) + i64((byte - u8('0')).to_primitive()));
    }
    return Some(value);
}
auto header_text(const Headers& headers, ref<str> name) -> Option<ref<str>> {
    auto value = headers.get_unique_text(name);
    return value.is_ok() ? *value : None<ref<str>>();
}
struct CacheControl {
    bool no_store {}, no_cache {}, must_revalidate {}, private_response {}, public_response {},
        invalid {};
    Option<i64> max_age, min_fresh;
    static auto parse(const Headers& headers) -> CacheControl {
        CacheControl result;
        for (auto field : headers.get_all("cache-control"_str)) {
            auto text = field->to_str();
            if (text.is_err()) {
                result.invalid = true;
                continue;
            }
            auto  bytes = text->as_bytes();
            usize start {};
            while (start < bytes.len()) {
                usize end = start;
                bool  quoted {}, escaped {};
                while (end < bytes.len()) {
                    auto byte = bytes[end];
                    if (! escaped && byte == u8('"')) quoted = ! quoted;
                    if (! quoted && byte == u8(',')) break;
                    escaped = quoted && ! escaped && byte == u8('\\');
                    ++end;
                }
                if (quoted) result.invalid = true;
                auto  directive = text->get(start, end).unwrap().trim_ascii();
                usize equal {};
                while (equal < directive.len() && directive.as_bytes()[equal] != u8('=')) ++equal;
                auto name = String::make(directive.get(usize(), equal).unwrap().trim_ascii());
                name.as_mut_str().make_ascii_lowercase();
                auto value =
                    equal < directive.len()
                        ? directive.get(equal + usize(1), directive.len()).unwrap().trim_ascii()
                        : ""_str;
                if (value.len() >= usize(2) && value.as_bytes()[usize()] == u8('"') &&
                    value.as_bytes()[value.len() - usize(1)] == u8('"'))
                    value = value.get(usize(1), value.len() - usize(1)).unwrap();
                if (name == "no-store"_str)
                    result.no_store = true;
                else if (name == "no-cache"_str)
                    result.no_cache = true;
                else if (name == "must-revalidate"_str)
                    result.must_revalidate = true;
                else if (name == "private"_str)
                    result.private_response = true;
                else if (name == "public"_str)
                    result.public_response = true;
                else if (name == "max-age"_str || name == "min-fresh"_str) {
                    auto  parsed = delta_seconds(value);
                    auto& target = name == "max-age"_str ? result.max_age : result.min_fresh;
                    if (target.is_some() || parsed.is_none())
                        result.invalid = true;
                    else
                        target = parsed;
                }
                start = end + usize(1);
            }
        }
        if (! headers.contains("cache-control"_str))
            for (auto field : headers.get_all("pragma"_str)) {
                auto tokens = field->tokens();
                if (tokens.is_ok())
                    for (auto& token : *tokens) {
                        token.as_mut_str().make_ascii_lowercase();
                        if (token == "no-cache"_str) result.no_cache = true;
                    }
            }
        return result;
    }
};
struct CacheFields {
    CacheControl   control;
    Option<i64>    date, expires, last_modified;
    i64            age {};
    Option<String> etag;
    Vec<String>    vary;
    bool           reusable { true };
    auto           clone() const -> CacheFields {
        Vec<String> names;
        for (const auto& name : vary) names.push(String::make(name.as_str()));
        return { control, date,         expires,           last_modified,
                 age,     etag.clone(), rstd::move(names), reusable };
    }
    static auto parse(const Headers& headers) -> CacheFields {
        CacheFields result;
        result.control = CacheControl::parse(headers);
        for (auto name : array<ref<str>, 3> { "date"_str, "expires"_str, "last-modified"_str }) {
            auto value  = header_text(headers, name);
            auto parsed = value.is_some() ? http_date(*value) : None<i64>();
            if (name == "date"_str)
                result.date = parsed;
            else if (name == "expires"_str)
                result.expires = parsed;
            else
                result.last_modified = parsed;
        }
        if (auto value = header_text(headers, "age"_str); value.is_some()) {
            auto parsed = delta_seconds(value->trim_ascii());
            if (parsed.is_none())
                result.reusable = false;
            else
                result.age = *parsed;
        }
        if (auto value = header_text(headers, "etag"_str); value.is_some()) {
            auto tag = value->trim_ascii();
            if (EntityTag::parse(tag.as_bytes()).is_ok()) result.etag = Some(String::make(tag));
        }
        for (auto field : headers.get_all("vary"_str)) {
            auto tokens = field->tokens();
            if (tokens.is_err()) {
                result.reusable = false;
                continue;
            }
            for (auto& token : *tokens) {
                token.as_mut_str().make_ascii_lowercase();
                if (token == "*"_str) result.reusable = false;
                result.vary.push(rstd::move(token));
            }
        }
        if (headers.contains("set-cookie"_str) || result.control.no_store || result.control.invalid)
            result.reusable = false;
        return result;
    }
};
auto merge_not_modified(const Headers& stored, const Headers& update) -> Headers {
    auto result = stored.clone();
    for (const auto& field : update) {
        auto name = field.name.as_str();
        if (field.name.matches("content-length"_str) ||
            field.name.matches("content-encoding"_str) || field.name.matches("content-range"_str) ||
            field.name.matches("transfer-encoding"_str) || field.name.matches("connection"_str))
            continue;
        result.remove(name);
        for (auto value : update.get_all(name))
            result.push(Header::make(name, value->as_slice()).unwrap());
    }
    return result;
}
} // namespace lihttpto
