export module lihttpto:request_target;
export import :syntax;

import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
enum class TargetForm
{
    Origin,
    Absolute,
    Authority,
    Asterisk
};
enum class TargetScheme
{
    Http,
    Https
};
enum class HostKind
{
    Name,
    Ipv4,
    Ipv6,
    Future
};
struct Authority {
    HostKind    kind { HostKind::Name };
    String      name;
    Option<u16> port;
};
struct RequestTarget {
    TargetForm           form { TargetForm::Origin };
    Option<TargetScheme> scheme;
    Option<Authority>    authority;
    String               escaped_path;
    // Split before decoding; a decoded slash remains inside its original segment.
    Vec<Vec<u8>>   segments;
    Option<String> query;
};
struct QueryField {
    Vec<u8> name, value;
};
} // namespace lihttpto

namespace lihttpto
{
auto unreserved(u8 byte) -> bool {
    return rstd::ascii::is_alnum(byte) ||
           rstd::parse::one_of(u8('-'), u8('.'), u8('_'), u8('~'))(byte);
}
auto sub_delimiter(u8 byte) -> bool {
    return rstd::parse::one_of(u8('!'),
                               u8('$'),
                               u8('&'),
                               u8('\''),
                               u8('('),
                               u8(')'),
                               u8('*'),
                               u8('+'),
                               u8(','),
                               u8(';'),
                               u8('='))(byte);
}
auto path_byte(u8 byte) -> bool {
    return unreserved(byte) || sub_delimiter(byte) || byte == u8(':') || byte == u8('@');
}
auto percent_byte(rstd::parse::TextCursor& cursor) -> Option<u8> {
    auto first  = cursor.take();
    auto second = cursor.take();
    if (first.is_none() || second.is_none()) return None();
    auto high = rstd::ascii::digit_value(first->get(), u8(16));
    auto low  = rstd::ascii::digit_value(second->get(), u8(16));
    if (high.is_none() || low.is_none()) return None();
    return Some(u8(high->to_primitive() * 16 + low->to_primitive()));
}
auto valid_ipv4(slice<u8> bytes) -> bool {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    for (int part = 0; part < 4; ++part) {
        auto digits = rstd::parse::consume_while_one(cursor, rstd::parse::ascii::digit);
        if (digits.is_none()) return false;
        auto text = cursor.view(*digits);
        if (text.len() > usize(3) || (text.len() > usize(1) && text[usize()] == u8('0')))
            return false;
        unsigned value = 0;
        for (auto byte : text) value = value * 10 + (byte - u8('0')).to_primitive();
        if (value > 255) return false;
        if (part != 3 && rstd::parse::consume_literal(cursor, "."_str).is_none()) return false;
    }
    return cursor.is_eof();
}
auto valid_ipv6(slice<u8> bytes) -> bool {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    bool                    compressed = rstd::parse::consume_literal(cursor, "::"_str).is_some();
    unsigned                groups     = 0;
    if (compressed && cursor.is_eof()) return true;
    while (! cursor.is_eof()) {
        auto segment = rstd::parse::consume_while_one(cursor, [](u8 byte) {
            return byte != u8(':');
        });
        if (segment.is_none()) return false;
        auto text   = cursor.view(*segment);
        bool dotted = false;
        for (auto byte : text) dotted = dotted || byte == u8('.');
        if (dotted) {
            if (! cursor.is_eof() || ! valid_ipv4(text)) return false;
            groups += 2;
        } else {
            if (text.len() > usize(4)) return false;
            for (auto byte : text)
                if (! rstd::parse::ascii::hex_digit(byte)) return false;
            ++groups;
        }
        if (groups > 8) return false;
        if (cursor.is_eof()) break;
        if (rstd::parse::consume_literal(cursor, "::"_str).is_some()) {
            if (compressed) return false;
            compressed = true;
            if (cursor.is_eof()) break;
        } else {
            (void)cursor.advance(usize(1));
            if (cursor.is_eof()) return false;
        }
    }
    return compressed ? groups < 8 : groups == 8;
}
auto valid_future(slice<u8> bytes) -> bool {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    auto                    prefix = cursor.take();
    if (prefix.is_none() || ascii_lower(prefix->get()) != u8('v')) return false;
    if (rstd::parse::consume_while_one(cursor, rstd::parse::ascii::hex_digit).is_none() ||
        rstd::parse::consume_literal(cursor, "."_str).is_none())
        return false;
    auto value = rstd::parse::consume_while_one(cursor, [](u8 byte) {
        return unreserved(byte) || sub_delimiter(byte) || byte == u8(':');
    });
    return value.is_some() && cursor.is_eof();
}
auto parse_authority(slice<u8> bytes, bool require_port = false) -> Result<Authority, DecodeError> {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    Authority               result;
    slice<u8>               host;
    if (rstd::parse::consume_literal(cursor, "["_str).is_some()) {
        auto literal = rstd::parse::consume_while_one(cursor, [](u8 byte) {
            return byte != u8(']');
        });
        if (literal.is_none() || rstd::parse::consume_literal(cursor, "]"_str).is_none())
            return Err(DecodeError::InvalidHost);
        host = cursor.view(*literal);
        if (valid_future(host))
            result.kind = HostKind::Future;
        else if (valid_ipv6(host))
            result.kind = HostKind::Ipv6;
        else
            return Err(DecodeError::InvalidHost);
    } else {
        auto begin = cursor.checkpoint();
        while (! cursor.is_eof()) {
            auto byte = cursor.peek()->get();
            if (byte == u8(':')) break;
            (void)cursor.advance(usize(1));
            if (byte == u8('%')) {
                if (percent_byte(cursor).is_none()) return Err(DecodeError::InvalidHost);
            } else if (! unreserved(byte) && ! sub_delimiter(byte))
                return Err(DecodeError::InvalidHost);
        }
        host = cursor.consumed(begin);
        if (host.is_empty()) return Err(DecodeError::InvalidHost);
        if (valid_ipv4(host)) result.kind = HostKind::Ipv4;
    }
    result.name = String::make(rstd::str_::from_utf8(host).unwrap());
    if (! cursor.is_eof()) {
        if (rstd::parse::consume_literal(cursor, ":"_str).is_none())
            return Err(DecodeError::InvalidHost);
        auto digits = rstd::parse::consume_while_one(cursor, rstd::parse::ascii::digit);
        if (! cursor.is_eof()) return Err(DecodeError::InvalidHost);
        if (digits.is_some()) {
            unsigned port = 0;
            for (auto byte : cursor.view(*digits)) {
                auto digit = (byte - u8('0')).to_primitive();
                if (port > (65535u - digit) / 10u) return Err(DecodeError::InvalidHost);
                port = port * 10 + digit;
            }
            result.port = Some(u16(port));
        }
    }
    if (require_port && result.port.is_none()) return Err(DecodeError::InvalidHost);
    return Ok(rstd::move(result));
}
auto parse_request_target(ref<str> method, slice<u8> bytes) -> Result<RequestTarget, DecodeError> {
    RequestTarget           result;
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    if (method == "CONNECT"_str) {
        auto authority = parse_authority(bytes, true);
        if (authority.is_err()) return Err(DecodeError::InvalidTarget);
        result.form      = TargetForm::Authority;
        result.authority = Some(rstd::move(authority).unwrap());
        return Ok(rstd::move(result));
    }
    if (bytes.len() == usize(1) && bytes[usize()] == u8('*')) {
        if (method != "OPTIONS"_str) return Err(DecodeError::InvalidTarget);
        result.form = TargetForm::Asterisk;
        return Ok(rstd::move(result));
    }
    if (bytes.is_empty()) return Err(DecodeError::InvalidTarget);
    if (bytes[usize()] != u8('/')) {
        auto scheme = rstd::parse::consume_while_one(cursor, [](u8 byte) {
            return byte != u8(':');
        });
        if (scheme.is_none() || rstd::parse::consume_literal(cursor, "://"_str).is_none())
            return Err(DecodeError::InvalidTarget);
        if (ascii_equal(cursor.view(*scheme), "http"_str))
            result.scheme = Some(TargetScheme::Http);
        else if (ascii_equal(cursor.view(*scheme), "https"_str))
            result.scheme = Some(TargetScheme::Https);
        else
            return Err(DecodeError::UnsupportedTarget);
        auto authority_text = rstd::parse::consume_while_one(cursor, [](u8 byte) {
            return byte != u8('/') && byte != u8('?');
        });
        if (authority_text.is_none()) return Err(DecodeError::InvalidTarget);
        auto authority = parse_authority(cursor.view(*authority_text));
        if (authority.is_err()) return Err(DecodeError::InvalidTarget);
        result.authority = Some(rstd::move(authority).unwrap());
        result.form      = TargetForm::Absolute;
    }
    auto path_start = cursor.checkpoint();
    bool slash      = rstd::parse::consume_literal(cursor, "/"_str).is_some();
    if (! slash && result.form == TargetForm::Origin) return Err(DecodeError::InvalidTarget);
    Vec<u8> segment;
    while (! cursor.is_eof() && cursor.peek()->get() != u8('?')) {
        auto byte = cursor.take()->get();
        if (byte == u8('/')) {
            result.segments.push(rstd::move(segment));
            segment = {};
        } else if (byte == u8('%')) {
            auto decoded = percent_byte(cursor);
            if (decoded.is_none()) return Err(DecodeError::InvalidTarget);
            segment.push(u8(*decoded));
        } else {
            if (! path_byte(byte)) return Err(DecodeError::InvalidTarget);
            segment.push(u8(byte));
        }
    }
    result.segments.push(rstd::move(segment));
    auto path           = cursor.consumed(path_start);
    result.escaped_path = path.is_empty() ? String::make("/"_str)
                                          : String::make(rstd::str_::from_utf8(path).unwrap());
    if (rstd::parse::consume_literal(cursor, "?"_str).is_some()) {
        auto begin = cursor.checkpoint();
        while (auto next = cursor.take()) {
            auto byte = next->get();
            if (byte == u8('%')) {
                if (percent_byte(cursor).is_none()) return Err(DecodeError::InvalidTarget);
            } else if (! path_byte(byte) && byte != u8('/') && byte != u8('?'))
                return Err(DecodeError::InvalidTarget);
        }
        result.query = Some(String::make(rstd::str_::from_utf8(cursor.consumed(begin)).unwrap()));
    }
    return Ok(rstd::move(result));
}
} // namespace lihttpto
