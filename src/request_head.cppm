export module lihttpto:request_head;
export import :request_line;

import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
struct Header {
    String  name;
    Vec<u8> value;
};
enum class BodyKind
{
    None,
    FixedLength,
    Chunked
};
struct BodyFraming {
    BodyKind kind { BodyKind::None };
    u64      length {};
};
struct HeadLimits {
    usize request_line { 8192 };
    usize header_line { 8192 };
    usize total_bytes { 65536 };
    usize fields { 100 };
};
struct RequestHead {
    RequestLine       line;
    Vec<Header>       headers;
    BodyFraming       body;
    bool              keep_alive;
    Option<Authority> authority;
};
} // namespace lihttpto

namespace lihttpto
{
auto ows(u8 byte) -> bool {
    return byte == u8(' ') || byte == u8('\t');
}

auto parse_header(slice<u8> bytes) -> Result<Header, DecodeError> {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    auto                    name = rstd::parse::consume_while_one(cursor, token_byte);
    if (name.is_none() || rstd::parse::consume_literal(cursor, ":"_str).is_none())
        return Err(DecodeError::InvalidHeader);
    (void)rstd::parse::consume_while(cursor, ows);
    auto start = cursor.position();
    auto value = rstd::parse::consume_while(cursor, [](u8 byte) {
        return byte == u8('\t') || (byte >= u8(32) && byte != u8(127));
    });
    if (! cursor.is_eof()) return Err(DecodeError::InvalidHeader);
    auto end = value.end;
    while (end > start && ows(bytes[end - usize(1)])) --end;
    Vec<u8> owned;
    for (usize index = start; index < end; ++index) owned.push(u8(bytes[index]));
    return Ok(Header { String::make(cursor.text(*name).unwrap()), rstd::move(owned) });
}

auto parse_length(slice<u8> bytes) -> Result<u64, DecodeError> {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    auto digits = rstd::parse::consume_while_one(cursor, rstd::parse::ascii::digit);
    if (digits.is_none() || ! cursor.is_eof()) return Err(DecodeError::InvalidContentLength);
    u64 length {};
    for (auto byte : bytes) {
        auto digit = u64((byte - u8('0')).to_primitive());
        if (length > (u64::MAX - digit) / u64(10)) return Err(DecodeError::InvalidContentLength);
        length = length * u64(10) + digit;
    }
    return Ok(length);
}

auto parse_connection(slice<u8> bytes, bool& close, bool& persistent) -> bool {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    while (! cursor.is_eof()) {
        (void)rstd::parse::consume_while(cursor, ows);
        if (cursor.is_eof()) break;
        if (rstd::parse::consume_literal(cursor, ","_str).is_some()) continue;
        auto token = rstd::parse::consume_while_one(cursor, token_byte);
        if (token.is_none()) return false;
        auto value = cursor.view(*token);
        close      = close || ascii_equal(value, "close"_str);
        persistent = persistent || ascii_equal(value, "keep-alive"_str);
        (void)rstd::parse::consume_while(cursor, ows);
        if (! cursor.is_eof() && rstd::parse::consume_literal(cursor, ","_str).is_none())
            return false;
    }
    return true;
}

auto finish_head(RequestLine line, Vec<Header> headers) -> Result<RequestHead, DecodeError> {
    bool              host       = false;
    bool              transfer   = false;
    bool              close      = false;
    bool              persistent = line.version == Version::Http11;
    Option<u64>       length;
    Option<Authority> authority;
    for (const auto& header : headers) {
        auto name  = header.name.as_str().as_bytes();
        auto value = header.value.as_slice();
        if (ascii_equal(name, "host"_str)) {
            if (host || value.is_empty()) return Err(DecodeError::InvalidHost);
            auto parsed = parse_authority(value);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            authority = Some(rstd::move(parsed).unwrap());
            host      = true;
        } else if (ascii_equal(name, "content-length"_str)) {
            if (length.is_some()) return Err(DecodeError::AmbiguousFraming);
            auto parsed = parse_length(value);
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            length = Some(parsed.unwrap());
        } else if (ascii_equal(name, "transfer-encoding"_str)) {
            if (transfer || line.version != Version::Http11 || ! ascii_equal(value, "chunked"_str))
                return Err(DecodeError::UnsupportedTransferEncoding);
            transfer = true;
        } else if (ascii_equal(name, "connection"_str)) {
            if (! parse_connection(value, close, persistent))
                return Err(DecodeError::InvalidHeader);
        }
    }
    if (line.version == Version::Http11 && ! host) return Err(DecodeError::InvalidHost);
    if (transfer && length.is_some()) return Err(DecodeError::AmbiguousFraming);
    BodyFraming body;
    if (transfer)
        body.kind = BodyKind::Chunked;
    else if (length.is_some())
        body = BodyFraming { BodyKind::FixedLength, *length };
    if (line.resource.authority.is_some()) {
        const auto& selected = *line.resource.authority;
        authority = Some(Authority { selected.kind, selected.name.clone(), selected.port });
    }
    return Ok(RequestHead { rstd::move(line),
                            rstd::move(headers),
                            body,
                            persistent && ! close,
                            rstd::move(authority) });
}
} // namespace lihttpto

export namespace lihttpto
{
class RequestHeadDecoder {
    enum class State
    {
        RequestLine,
        Headers,
        Complete,
        Failed,
        Taken
    };
    State               state_ { State::RequestLine };
    HeadLimits          limits_;
    usize               total_ {};
    RequestLineDecoder  request_line_;
    CrlfLineReader      header_line_;
    Option<RequestLine> line_;
    Vec<Header>         headers_;
    Option<RequestHead> head_;
    DecodeError         error_ { DecodeError::InvalidState };

    auto fail(DecodeError error) -> Result<DecodeProgress, DecodeError> {
        state_ = State::Failed;
        error_ = error;
        return Err(error);
    }

public:
    explicit RequestHeadDecoder(HeadLimits limits = {})
        : limits_(limits), request_line_(limits.request_line), header_line_(limits.header_line) {}

    auto feed(slice<u8> input, bool final = false) -> Result<DecodeProgress, DecodeError> {
        if (state_ == State::Failed) return Err(error_);
        if (state_ == State::Taken) return Err(DecodeError::InvalidState);
        if (state_ == State::Complete)
            return Ok(DecodeProgress { usize(), DecodeStatus::Complete });
        rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(input) };
        while (! cursor.is_eof()) {
            if (total_ >= limits_.total_bytes) return fail(DecodeError::TooLarge);
            auto count     = cursor.remaining();
            auto available = limits_.total_bytes - total_;
            if (count > available) count = available;
            auto remaining = cursor.remaining_input();
            auto fragment  = slice<u8>::from_raw_parts(remaining.as_raw_ptr(), count);
            auto progress  = state_ == State::RequestLine ? request_line_.feed(fragment)
                                                          : header_line_.feed(fragment);
            if (progress.is_err()) return fail(progress.unwrap_err());
            (void)cursor.advance(progress->consumed);
            total_ += progress->consumed;
            if (progress->status == DecodeStatus::NeedMore) continue;
            if (state_ == State::RequestLine) {
                line_  = request_line_.take();
                state_ = State::Headers;
                continue;
            }
            auto bytes = header_line_.line();
            if (bytes.is_empty()) {
                auto parsed = finish_head(line_.take().unwrap(), rstd::move(headers_));
                if (parsed.is_err()) return fail(parsed.unwrap_err());
                head_  = Some(rstd::move(parsed).unwrap());
                state_ = State::Complete;
                return Ok(DecodeProgress { cursor.position(), DecodeStatus::Complete });
            }
            if (headers_.len() >= limits_.fields) return fail(DecodeError::TooLarge);
            auto header = parse_header(bytes);
            if (header.is_err()) return fail(header.unwrap_err());
            headers_.push(rstd::move(header).unwrap());
            header_line_.reset();
        }
        if (final) return fail(DecodeError::Truncated);
        return Ok(DecodeProgress { cursor.position(), DecodeStatus::NeedMore });
    }

    auto take() -> Option<RequestHead> {
        if (state_ != State::Complete) return None();
        state_ = State::Taken;
        return head_.take();
    }
    void reset() {
        request_line_.reset();
        header_line_.reset();
        line_ = None();
        head_ = None();
        headers_.clear();
        total_ = usize();
        state_ = State::RequestLine;
    }
};
} // namespace lihttpto
