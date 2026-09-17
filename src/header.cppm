module;
#include <rstd/enum.hpp>
#include <rstd/macro.hpp>
export module lihttpto:header;
export import :syntax;
import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
class HeaderError {
    RSTD_ENUM(HeaderError, (InvalidName), (InvalidValue), (InvalidText), (Duplicate))
};
class HeaderName {
    String text_;
    explicit HeaderName(String text): text_(rstd::move(text)) {}

public:
    static auto make(ref<str> text) -> Result<HeaderName, HeaderError> {
        rstd::parse::TextCursor cursor(rstd::parse::text_input(text));
        if (rstd::parse::consume_while_one(cursor, token_byte).is_none() || ! cursor.is_eof())
            return Err(HeaderError::InvalidName());
        return Ok(HeaderName { String::make(text) });
    }
    auto as_str() const -> ref<str> { return text_.as_str(); }
    auto matches(ref<str> name) const -> bool {
        return ascii_equal(text_.as_str().as_bytes(), name);
    }
};
class HeaderValue {
    Vec<u8> bytes_;
    explicit HeaderValue(Vec<u8> bytes): bytes_(rstd::move(bytes)) {}

public:
    static auto make(slice<u8> bytes) -> Result<HeaderValue, HeaderError> {
        for (auto byte : bytes)
            if (! (byte == u8('\t') || (byte >= u8(32) && byte != u8(127))))
                return Err(HeaderError::InvalidValue());
        return Ok(HeaderValue { Vec<u8>::from(bytes) });
    }
    auto as_slice() const -> slice<u8> { return bytes_.as_slice(); }
    auto operator[](usize index) const -> u8 { return u8(bytes_[index]); }
    auto to_str() const -> Result<ref<str>, HeaderError> {
        auto text = rstd::str_::from_utf8(bytes_.as_slice());
        if (text.is_err()) return Err(HeaderError::InvalidText());
        return Ok(*text);
    }
};
struct Header {
    HeaderName  name;
    HeaderValue value;
    static auto make(ref<str> name, slice<u8> value) -> Result<Header, HeaderError> {
        auto parsed_name  = rstd_try(HeaderName::make(name));
        auto parsed_value = rstd_try(HeaderValue::make(value));
        return Ok(Header { rstd::move(parsed_name), rstd::move(parsed_value) });
    }
    // The caller owns line framing; this consumes a field without CR/LF.
    static auto parse_line(slice<u8> bytes) -> Result<Header, HeaderError> {
        rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
        auto                    name = rstd::parse::consume_while_one(cursor, [](u8 byte) {
            return byte != u8(':');
        });
        if (name.is_none() || ! rstd::parse::consume_literal(cursor, ":"_str))
            return Err(HeaderError::InvalidName());
        auto text = cursor.text(*name);
        if (text.is_err()) return Err(HeaderError::InvalidName());
        auto ows = [](u8 byte) {
            return byte == u8(' ') || byte == u8('\t');
        };
        rstd::parse::consume_while(cursor, ows);
        auto end = bytes.len();
        while (end > cursor.position() && ows(bytes[end - usize(1)])) --end;
        auto value = slice<u8>::from_raw_parts(
            bytes.as_raw_ptr() + cursor.position().to_primitive(), end - cursor.position());
        return make(*text, value);
    }
};
class Headers {
    Vec<Header> fields_;

public:
    void push(Header header) { fields_.push(rstd::move(header)); }
    void clear() { fields_.clear(); }
    auto len() const -> usize { return fields_.len(); }
    auto as_slice() const -> slice<Header> { return fields_.as_slice(); }
    auto begin() const { return fields_.begin(); }
    auto end() const { return fields_.end(); }
    auto operator[](usize index) const -> const Header& { return fields_[index]; }
    // Returned views borrow this collection and are invalidated by mutation.
    auto get_all(ref<str> name) const -> Vec<ref<HeaderValue>> {
        Vec<ref<HeaderValue>> result;
        for (const auto& field : fields_)
            if (field.name.matches(name))
                result.push(ref<HeaderValue>::from_raw_parts(rstd::addressof(field.value)));
        return result;
    }
    auto get_unique(ref<str> name) const -> Result<Option<ref<HeaderValue>>, HeaderError> {
        Option<ref<HeaderValue>> result;
        for (const auto& field : fields_) {
            if (! field.name.matches(name)) continue;
            if (result.is_some()) return Err(HeaderError::Duplicate());
            result = Some(ref<HeaderValue>::from_raw_parts(rstd::addressof(field.value)));
        }
        return Ok(result);
    }
    auto get_unique_text(ref<str> name) const -> Result<Option<ref<str>>, HeaderError> {
        auto field = rstd_try(get_unique(name));
        if (field.is_none()) return Ok<Option<ref<str>>>(None());
        return Ok(Some(rstd_try((*field)->to_str())));
    }
};
} // namespace lihttpto
