module;
#include <rstd/macro.hpp>
module lihttpto;
import :conditions;
using namespace rstd::prelude;
using namespace rstd::literals;

auto condition_date(const lihttpto::Headers& headers, ref<str> name) -> Option<i64> {
    auto field = headers.get_unique_text(name);
    return field.is_ok() && field->is_some() ? lihttpto::http_date((**field).trim_ascii())
                                             : None<i64>();
}
auto range_number(ref<str> text) -> Option<u64> {
    if (text.is_empty()) return None();
    u64 result {};
    for (auto byte : text.as_bytes()) {
        if (! rstd::ascii::is_digit(byte)) return None();
        auto digit      = u64((byte - u8('0')).to_primitive());
        auto multiplied = result.checked_mul(u64(10));
        if (multiplied.is_none()) return None();
        auto added = multiplied->checked_add(digit);
        if (added.is_none()) return None();
        result = *added;
    }
    return Some(result);
}
namespace lihttpto
{
auto RequestConditions::parse(const Headers& headers) -> Result<RequestConditions, ConditionError> {
    RequestConditions result;
    result.if_match_      = rstd_try(EntityTagCondition::parse(headers, "if-match"_str));
    result.if_none_match_ = rstd_try(EntityTagCondition::parse(headers, "if-none-match"_str));
    if (result.if_match_.is_none())
        result.if_unmodified_since_ = condition_date(headers, "if-unmodified-since"_str);
    if (result.if_none_match_.is_none())
        result.if_modified_since_ = condition_date(headers, "if-modified-since"_str);
    result.if_range_present_ = headers.contains("if-range"_str);
    auto validator           = headers.get_unique("if-range"_str);
    if (validator.is_ok() && validator->is_some()) {
        auto tag = EntityTag::parse((**validator)->as_slice());
        if (tag.is_ok())
            result.if_range_tag_ = Some(rstd::move(tag).unwrap());
        else
            result.if_range_date_ = condition_date(headers, "if-range"_str);
    }
    auto range = headers.get_unique_text("range"_str);
    if (range.is_err() || range->is_none()) return Ok(rstd::move(result));
    auto  text = (**range).trim_ascii();
    usize equal {};
    while (equal < text.len() && text.as_bytes()[equal] != u8('=')) ++equal;
    if (equal == text.len() ||
        ! ascii_equal(text.get(usize(), equal).unwrap().as_bytes(), "bytes"_str))
        return Ok(rstd::move(result));
    auto  value = text.get(equal + usize(1), text.len()).unwrap().trim_ascii();
    usize dash {};
    while (dash < value.len() && value.as_bytes()[dash] != u8('-')) ++dash;
    if (dash == value.len()) return Ok(rstd::move(result));
    auto first = value.get(usize(), dash).unwrap();
    auto last  = value.get(dash + usize(1), value.len()).unwrap();
    if (first.is_empty()) {
        auto suffix = range_number(last);
        if (suffix.is_none()) return Ok(rstd::move(result));
        result.suffix_ = true;
        result.first_  = *suffix;
    } else {
        auto start = range_number(first);
        if (start.is_none()) return Ok(rstd::move(result));
        result.first_ = *start;
        if (! last.is_empty()) {
            result.last_ = range_number(last);
            if (result.last_.is_none() || *result.last_ < *start) return Ok(rstd::move(result));
        }
    }
    result.range_valid_ = true;
    return Ok(rstd::move(result));
}
auto RequestConditions::evaluate(ref<str>                      method,
                                 const RepresentationMetadata& representation) const
    -> RepresentationDecision {
    auto terminal = [](RepresentationKind kind) {
        return RepresentationDecision { kind, {} };
    };
    const auto& meta      = representation;
    bool        retrieval = method == "GET"_str || method == "HEAD"_str;
    if (method == "CONNECT"_str || method == "OPTIONS"_str || method == "TRACE"_str)
        return { RepresentationKind::Full, { u64(), meta.length } };
    if (if_match_.is_some()) {
        if (! if_match_->matches(meta.exists, meta.etag, true))
            return terminal(RepresentationKind::PreconditionFailed);
    } else if (meta.exists && if_unmodified_since_.is_some() && meta.last_modified.is_some() &&
               *meta.last_modified > *if_unmodified_since_)
        return terminal(RepresentationKind::PreconditionFailed);
    if (if_none_match_.is_some()) {
        if (if_none_match_->matches(meta.exists, meta.etag, false))
            return terminal(retrieval ? RepresentationKind::NotModified
                                      : RepresentationKind::PreconditionFailed);
    } else if (retrieval && meta.exists && if_modified_since_.is_some() &&
               meta.last_modified.is_some() && *meta.last_modified <= *if_modified_since_)
        return terminal(RepresentationKind::NotModified);
    RepresentationDecision full { RepresentationKind::Full, { u64(), meta.length } };
    if (method != "GET"_str || ! meta.exists || ! meta.accept_ranges || ! range_valid_ ||
        meta.length == u64())
        return full;
    if (if_range_present_) {
        bool matches =
            if_range_tag_.is_some() && meta.etag.is_some() && if_range_tag_->strong_eq(*meta.etag);
        if (if_range_date_.is_some())
            matches = meta.last_modified_strong && meta.last_modified.is_some() &&
                      *if_range_date_ == *meta.last_modified;
        if (! matches) return full;
    }
    if (suffix_) {
        if (first_ == u64()) return terminal(RepresentationKind::Unsatisfiable);
        auto length = rstd::min(first_, meta.length);
        return { RepresentationKind::Partial, { meta.length - length, length } };
    }
    if (first_ >= meta.length) return terminal(RepresentationKind::Unsatisfiable);
    auto end = last_.is_some() ? rstd::min(*last_, meta.length - u64(1)) : meta.length - u64(1);
    return { RepresentationKind::Partial, { first_, end - first_ + u64(1) } };
}
auto RepresentationDecision::status() const -> StatusCode {
    unsigned code = 200;
    switch (kind) {
    case RepresentationKind::Full: break;
    case RepresentationKind::Partial: code = 206; break;
    case RepresentationKind::NotModified: code = 304; break;
    case RepresentationKind::PreconditionFailed: code = 412; break;
    case RepresentationKind::Unsatisfiable: code = 416; break;
    }
    return StatusCode::make(u16(code)).unwrap();
}
auto RepresentationDecision::content_range(u64 total_length) const -> Option<String> {
    if (kind == RepresentationKind::Unsatisfiable)
        return Some(rstd::format("bytes */{}", total_length));
    if (kind == RepresentationKind::Partial)
        return Some(rstd::format(
            "bytes {}-{}/{}", range.offset, range.offset + range.length - u64(1), total_length));
    return None();
}
} // namespace lihttpto
