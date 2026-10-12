module lihttpto;
import :validator;
using namespace rstd::prelude;
using namespace rstd::literals;

auto tag_space(u8 byte) -> bool {
    return byte == u8(' ') || byte == u8('\t');
}
auto tag_view(slice<u8> bytes, usize first, usize last) -> slice<u8> {
    return slice<u8>::from_raw_parts(bytes.as_raw_ptr() + first.to_primitive(), last - first);
}
auto trim_tag(slice<u8> bytes) -> slice<u8> {
    usize start {}, end = bytes.len();
    while (start < end && tag_space(bytes[start])) ++start;
    while (start < end && tag_space(bytes[end - usize(1)])) --end;
    return tag_view(bytes, start, end);
}
auto tag_end(slice<u8> bytes) -> Option<usize> {
    usize start {};
    if (bytes.len() >= usize(2) && bytes[usize()] == u8('W') && bytes[usize(1)] == u8('/'))
        start = usize(2);
    if (start >= bytes.len() || bytes[start] != u8('"')) return None();
    for (usize index = start + usize(1); index < bytes.len(); ++index) {
        auto byte = bytes[index];
        if (byte == u8('"')) return Some(index + usize(1));
        if (! (byte == u8(33) || (byte >= u8(35) && byte != u8(127)))) return None();
    }
    return None();
}
namespace lihttpto
{
auto EntityTag::parse(slice<u8> bytes) -> Result<EntityTag, ConditionError> {
    bytes    = trim_tag(bytes);
    auto end = tag_end(bytes);
    if (end.is_none() || *end != bytes.len()) return Err(ConditionError::InvalidEntityTag);
    return Ok(EntityTag { Vec<u8>::from(bytes), bytes[usize()] == u8('W') });
}
auto EntityTag::weak_eq(const EntityTag& other) const -> bool {
    return tag_view(as_bytes(), usize(weak_ ? 2 : 0), wire_.len()) ==
           tag_view(other.as_bytes(), usize(other.weak_ ? 2 : 0), other.wire_.len());
}
auto EntityTagCondition::parse(const Headers& headers, ref<str> name)
    -> Result<Option<EntityTagCondition>, ConditionError> {
    auto fields = headers.get_all(name);
    if (fields.is_empty()) return Ok<Option<EntityTagCondition>>(None());
    EntityTagCondition result;
    for (auto field : fields) {
        auto bytes = trim_tag(field->as_slice());
        if (bytes == "*"_bytes) {
            if (fields.len() != usize(1)) return Err(ConditionError::InvalidEntityTag);
            result.any_ = true;
            return Ok(Some(rstd::move(result)));
        }
        usize start {};
        while (start < bytes.len()) {
            while (start < bytes.len() && (tag_space(bytes[start]) || bytes[start] == u8(',')))
                ++start;
            if (start == bytes.len()) break;
            auto tail = tag_view(bytes, start, bytes.len());
            auto end  = tag_end(tail);
            if (end.is_none()) return Err(ConditionError::InvalidEntityTag);
            auto tag = EntityTag::parse(tag_view(tail, usize(), *end));
            result.tags_.push(rstd::move(tag).unwrap());
            start += *end;
            while (start < bytes.len() && tag_space(bytes[start])) ++start;
            if (start < bytes.len() && bytes[start] != u8(','))
                return Err(ConditionError::InvalidEntityTag);
        }
    }
    if (result.tags_.is_empty()) return Err(ConditionError::InvalidEntityTag);
    return Ok(Some(rstd::move(result)));
}
auto EntityTagCondition::matches(bool exists, const Option<EntityTag>& current, bool strong) const
    -> bool {
    if (! exists) return false;
    if (any_) return true;
    if (current.is_none()) return false;
    for (const auto& tag : tags_)
        if (strong ? tag.strong_eq(*current) : tag.weak_eq(*current)) return true;
    return false;
}
auto entity_tag_matches(ref<str> condition, ref<str> current) -> bool {
    Headers headers;
    if (headers.add("if-none-match"_str, condition).is_err()) return false;
    auto parsed = EntityTagCondition::parse(headers, "if-none-match"_str);
    auto tag    = EntityTag::parse(current.as_bytes());
    if (parsed.is_err() || parsed->is_none() || tag.is_err()) return false;
    return (**parsed).matches(true, Some(rstd::move(tag).unwrap()), false);
}
} // namespace lihttpto
