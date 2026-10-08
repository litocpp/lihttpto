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
    usize offset_ {};

public:
    auto offset() const -> usize { return offset_; }
    auto at(usize offset) && -> HeaderError {
        offset_ = offset;
        return rstd::move(*this);
    }
};
class HeaderName : public DefaultInClass<HeaderName, Clone> {
    friend struct Header;
    String text_;
    explicit HeaderName(String text): text_(rstd::move(text)) {}

public:
    static auto make(ref<str> text) -> Result<HeaderName, HeaderError> {
        rstd::parse::TextCursor cursor(rstd::parse::text_input(text));
        if (rstd::parse::consume_while_one(cursor, token_byte).is_none() || ! cursor.is_eof())
            return Err(HeaderError::InvalidName().at(cursor.position()));
        return Ok(HeaderName { String::make(text) });
    }
    auto as_str() const -> ref<str> { return text_.as_str(); }
    auto clone() const -> HeaderName { return HeaderName { text_.clone() }; }
    auto matches(ref<str> name) const -> bool {
        return ascii_equal(text_.as_str().as_bytes(), name);
    }
};
class HeaderValue : public DefaultInClass<HeaderValue, Clone> {
    Vec<u8> bytes_;
    explicit HeaderValue(Vec<u8> bytes): bytes_(rstd::move(bytes)) {}

public:
    static auto make(slice<u8> bytes) -> Result<HeaderValue, HeaderError> {
        usize offset {};
        for (auto byte : bytes) {
            if (! (byte == u8('\t') || (byte >= u8(32) && byte != u8(127))))
                return Err(HeaderError::InvalidValue().at(offset));
            ++offset;
        }
        return Ok(HeaderValue { Vec<u8>::from(bytes) });
    }
    auto as_slice() const -> slice<u8> { return bytes_.as_slice(); }
    auto clone() const -> HeaderValue { return HeaderValue { bytes_.clone() }; }
    auto operator[](usize index) const -> u8 { return u8(bytes_[index]); }
    auto to_str() const -> Result<ref<str>, HeaderError> {
        auto text = rstd::str_::from_utf8(bytes_.as_slice());
        if (text.is_err()) return Err(HeaderError::InvalidText());
        return Ok(*text);
    }
    auto tokens() const -> Result<Vec<String>, HeaderError> {
        rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes_.as_slice()) };
        Vec<String>             result;
        auto                    whitespace = [](u8 byte) {
            return byte == u8(' ') || byte == u8('\t');
        };
        for (;;) {
            (void)rstd::parse::consume_while(cursor, whitespace);
            if (cursor.is_eof()) break;
            if (rstd::parse::consume_literal(cursor, ","_str).is_some()) continue;
            auto token = rstd::parse::consume_while_one(cursor, token_byte);
            if (token.is_none()) return Err(HeaderError::InvalidValue().at(cursor.position()));
            result.push(String::make(rstd::str_::from_utf8(cursor.view(*token)).unwrap()));
            (void)rstd::parse::consume_while(cursor, whitespace);
            if (cursor.is_eof()) break;
            if (rstd::parse::consume_literal(cursor, ","_str).is_none())
                return Err(HeaderError::InvalidValue().at(cursor.position()));
        }
        if (result.is_empty()) return Err(HeaderError::InvalidValue());
        return Ok(rstd::move(result));
    }
};
struct Header : public DefaultInClass<Header, Clone> {
    HeaderName  name;
    HeaderValue value;
    Header(HeaderName name, HeaderValue value): name(rstd::move(name)), value(rstd::move(value)) {}
    auto        clone() const -> Header { return Header { name.clone(), value.clone() }; }
    static auto make(ref<str> name, slice<u8> value) -> Result<Header, HeaderError> {
        auto parsed_name  = rstd_try(HeaderName::make(name));
        auto parsed_value = rstd_try(HeaderValue::make(value));
        return Ok(Header { rstd::move(parsed_name), rstd::move(parsed_value) });
    }
    // The caller owns line framing; this consumes a field without CR/LF.
    static auto parse_line(slice<u8> bytes) -> Result<Header, HeaderError> {
        rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
        auto                    name = rstd::parse::consume_while_one(cursor, token_byte);
        if (name.is_none() || ! rstd::parse::consume_literal(cursor, ":"_str))
            return Err(HeaderError::InvalidName().at(cursor.position()));
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
        auto parsed_name  = HeaderName { String::make(*text) };
        auto parsed_value = HeaderValue::make(value);
        if (parsed_value.is_err()) {
            auto error  = rstd::move(parsed_value).unwrap_err();
            auto offset = cursor.position() + error.offset();
            return Err(rstd::move(error).at(offset));
        }
        return Ok(Header { rstd::move(parsed_name), rstd::move(parsed_value).unwrap() });
    }
};
class Headers : public DefaultInClass<Headers, Clone> {
    Vec<Header> fields_;

public:
    void push(Header header) { fields_.push(rstd::move(header)); }
    auto add(ref<str> name, HeaderValue value) -> Result<empty, HeaderError> {
        auto parsed = rstd_try(HeaderName::make(name));
        push(Header { rstd::move(parsed), rstd::move(value) });
        return Ok(empty {});
    }
    auto add(ref<str> name, ref<str> value) -> Result<empty, HeaderError> {
        push(rstd_try(Header::make(name, value.as_bytes())));
        return Ok(empty {});
    }
    auto set(ref<str> name, HeaderValue value) -> Result<empty, HeaderError> {
        auto parsed = rstd_try(HeaderName::make(name));
        remove(name);
        push(Header { rstd::move(parsed), rstd::move(value) });
        return Ok(empty {});
    }
    auto set(ref<str> name, ref<str> value) -> Result<empty, HeaderError> {
        auto field = rstd_try(Header::make(name, value.as_bytes()));
        remove(name);
        push(rstd::move(field));
        return Ok(empty {});
    }
    auto remove(ref<str> name) -> usize {
        Vec<Header> kept;
        usize       count {};
        for (auto& field : fields_) {
            if (field.name.matches(name))
                ++count;
            else
                kept.push(rstd::move(field));
        }
        fields_ = rstd::move(kept);
        return count;
    }
    auto contains(ref<str> name) const -> bool {
        for (const auto& field : fields_)
            if (field.name.matches(name)) return true;
        return false;
    }
    auto get(ref<str> name) const -> Option<ref<HeaderValue>> {
        for (const auto& field : fields_)
            if (field.name.matches(name))
                return Some(ref<HeaderValue>::from_raw_parts(rstd::addressof(field.value)));
        return None();
    }
    auto clone() const -> Headers {
        Headers result;
        for (const auto& field : fields_) result.push(field.clone());
        return result;
    }
    auto is_empty() const -> bool { return fields_.is_empty(); }
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

namespace rstd
{
export template<>
struct Impl<fmt::Display, lihttpto::HeaderError> : ImplBase<lihttpto::HeaderError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& error = this->self();
        auto        text  = error.is_InvalidName()    ? "invalid HTTP field name"_str
                            : error.is_InvalidValue() ? "invalid HTTP field value"_str
                            : error.is_InvalidText()  ? "HTTP field is not UTF-8"_str
                                                      : "duplicate HTTP field"_str;
        return formatter.write_raw(text.data(), text.len().to_primitive());
    }
};
export template<>
struct Impl<fmt::Debug, lihttpto::HeaderError> : ImplBase<lihttpto::HeaderError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
export template<>
struct Impl<error::Error, lihttpto::HeaderError>
    : DefaultInImpl<error::Error, lihttpto::HeaderError> {};
} // namespace rstd
