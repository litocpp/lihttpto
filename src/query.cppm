export module lihttpto:query;
export import :request_target;
using namespace rstd::prelude;
export namespace lihttpto
{
auto decode_form_query(slice<u8> input,
                       usize     byte_limit  = usize(8192),
                       usize     field_limit = usize(64)) -> Result<Vec<QueryField>, DecodeError> {
    if (input.len() > byte_limit) return Err(DecodeError::TooLarge);
    Vec<QueryField> fields;
    if (input.is_empty()) return Ok(rstd::move(fields));
    if (field_limit == usize()) return Err(DecodeError::TooLarge);
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(input) };
    QueryField              field;
    bool                    value = false;
    while (auto next = cursor.take()) {
        auto byte = next->get();
        if (byte == u8('&')) {
            fields.push(rstd::move(field));
            if (fields.len() >= field_limit) return Err(DecodeError::TooLarge);
            field = {};
            value = false;
            continue;
        }
        if (byte == u8('=') && ! value) {
            value = true;
            continue;
        }
        if (byte == u8('%')) {
            auto decoded = percent_byte(cursor);
            if (decoded.is_none()) return Err(DecodeError::InvalidTarget);
            byte = *decoded;
        } else if (byte == u8('+'))
            byte = u8(' ');
        (value ? field.value : field.name).push(u8(byte));
    }
    fields.push(rstd::move(field));
    return Ok(rstd::move(fields));
}
} // namespace lihttpto
