module;
#include <rstd/macro.hpp>
#include <time.h>
export module lihttpto:cache;
export import :header;
export import :message;
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
auto http_date(ref<str> text) -> Option<i64> {
    if (text.len() > usize(128)) return None();
    array<char, 129> input {};
    for (usize i {}; i < text.len(); ++i) input[i] = char(text.as_bytes()[i].to_primitive());
    for (auto format : array<const char*, 3> {
             "%a, %d %b %Y %H:%M:%S GMT", "%A, %d-%b-%y %H:%M:%S GMT", "%a %b %e %H:%M:%S %Y" }) {
        tm   parsed {};
        auto end = strptime(input.data(), format, &parsed);
        if (! end || *end || parsed.tm_sec > 59) continue;
        auto original = parsed;
        auto result   = timegm(&parsed);
        if (parsed.tm_year != original.tm_year || parsed.tm_mon != original.tm_mon ||
            parsed.tm_mday != original.tm_mday || parsed.tm_hour != original.tm_hour ||
            parsed.tm_min != original.tm_min || parsed.tm_sec != original.tm_sec)
            continue;
        return Some(i64(result));
    }
    return None();
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
            auto tag    = value->trim_ascii();
            auto opaque = tag.starts_with("W/"_str) ? tag.get(usize(2), tag.len()).unwrap() : tag;
            bool valid  = opaque.len() >= usize(2) && opaque.as_bytes()[usize()] == u8('"') &&
                          opaque.as_bytes()[opaque.len() - usize(1)] == u8('"');
            for (usize i(1); valid && i + usize(1) < opaque.len(); ++i) {
                auto byte = opaque.as_bytes()[i];
                valid     = byte == u8(33) || (byte >= u8(35) && byte != u8(127));
            }
            if (valid) result.etag = Some(String::make(tag));
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
auto entity_tag_matches(ref<str> condition, ref<str> current) -> bool {
    auto weak = [](ref<str> value) {
        value = value.trim_ascii();
        return value.starts_with("W/"_str) ? value.get(usize(2), value.len()).unwrap() : value;
    };
    auto  bytes = condition.as_bytes();
    usize start {};
    while (start < bytes.len()) {
        usize end = start;
        bool  quoted {};
        while (end < bytes.len()) {
            if (bytes[end] == u8('"')) quoted = ! quoted;
            if (! quoted && bytes[end] == u8(',')) break;
            ++end;
        }
        auto value = condition.get(start, end).unwrap().trim_ascii();
        if (value == "*"_str || weak(value) == weak(current)) return true;
        start = end + usize(1);
    }
    return false;
}
} // namespace lihttpto
