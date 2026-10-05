module;
#include <rstd/enum.hpp>
module lihttpto;
import :message;
import lihttpto.parser.http1;

namespace lihttpto
{

using namespace rstd::prelude;
using rstd::string::String;

namespace
{

auto start_line_error(parser::ParseFailure failure) -> HttpParseError {
    return HttpParseError { HttpParseErrorKind::InvalidStartLine(), failure.offset() };
}

auto make_start_line(slice<u8> input, parser::http1::StartLine parsed)
    -> rstd::Result<StartLine, HttpParseError> {
    RSTD_MATCH(rstd::move(parsed)) {
        RSTD_CASE(Request, value) {
            auto method_text = rstd::str_::from_utf8_unchecked(slice<u8>::from_raw_parts(
                input.as_raw_ptr() + value.method.begin.to_primitive(), value.method.size()));
            auto method      = Method::parse(method_text);
            if (method.is_err()) return Err(rstd::move(method).unwrap_err());

            auto target_text = rstd::str_::from_utf8_unchecked(slice<u8>::from_raw_parts(
                input.as_raw_ptr() + value.target.begin.to_primitive(), value.target.size()));
            return Ok(StartLine::Request(
                MessageRequestLine { rstd::move(method).unwrap(),
                                     String::make(target_text),
                                     MessageVersion { value.major, value.minor } }));
        }
        RSTD_CASE(Response, value) {
            auto status = StatusCode::make(value.status);
            if (status.is_err()) return Err(rstd::move(status).unwrap_err());
            auto reason_bytes = slice<u8>::from_raw_parts(
                input.as_raw_ptr() + value.reason.begin.to_primitive(), value.reason.size());
            auto reason = HeaderValue::make(reason_bytes);
            if (reason.is_err()) {
                return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(),
                                            value.reason.begin + usize() });
            }
            return Ok(
                StartLine::Response(StatusLine { Some(MessageVersion { value.major, value.minor }),
                                                 rstd::move(status).unwrap(),
                                                 Some(rstd::move(reason).unwrap()) }));
        }
    }
    rstd::panic { "invalid HTTP start line" };
}

} // namespace

Method::Method(String value) noexcept: value_(rstd::move(value)) {
}

auto Method::parse(ref<str> input) -> rstd::Result<Method, HttpParseError> {
    if (input.size() == usize()) {
        return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), usize() });
    }
    for (usize offset {}; offset < input.size(); ++offset) {
        if (! parser::ascii::tchar(input[offset])) {
            return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), offset });
        }
    }
    return Ok(Method { String::make(input) });
}

auto Method::as_ref() const noexcept -> ref<str> {
    return value_.as_str();
}

auto Method::clone() const -> Method {
    return Method { value_.clone() };
}

auto MessageVersion::parse(ref<str> input) -> rstd::Result<MessageVersion, HttpParseError> {
    if (input.size() != usize(8) || input[usize()].to_primitive() != 'H' ||
        input[usize(1)].to_primitive() != 'T' || input[usize(2)].to_primitive() != 'T' ||
        input[usize(3)].to_primitive() != 'P' || input[usize(4)].to_primitive() != '/' ||
        ! rstd::ascii::is_digit(input[usize(5)]) || input[usize(6)].to_primitive() != '.' ||
        ! rstd::ascii::is_digit(input[usize(7)])) {
        return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), usize() });
    }
    return Ok(MessageVersion { u8(input[usize(5)].to_primitive() - '0'),
                               u8(input[usize(7)].to_primitive() - '0') });
}

auto StatusCode::make(u16 value) -> rstd::Result<StatusCode, HttpParseError> {
    if (value < u16(100) || value > u16(999)) {
        return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), usize() });
    }
    return Ok(StatusCode { value });
}

auto StatusCode::parse(ref<str> input) -> rstd::Result<StatusCode, HttpParseError> {
    if (input.size() != usize(3)) {
        return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), input.size() });
    }
    rstd::uint16_t value {};
    for (usize offset {}; offset < input.size(); ++offset) {
        auto digit = input[offset];
        if (! rstd::ascii::is_digit(digit)) {
            return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), offset });
        }
        value = static_cast<rstd::uint16_t>(value * 10 + digit.to_primitive() - '0');
    }
    return make(u16(value));
}

MessageRequestLine::MessageRequestLine(Method         method,
                                       String         target,
                                       MessageVersion version) noexcept
    : method_(rstd::move(method)), target_(rstd::move(target)), version_(version) {
}

auto MessageRequestLine::method() const noexcept -> const Method& {
    return method_;
}

auto MessageRequestLine::target() const noexcept -> ref<str> {
    return target_.as_str();
}

auto MessageRequestLine::version() const noexcept -> MessageVersion {
    return version_;
}

auto MessageRequestLine::clone() const -> MessageRequestLine {
    return MessageRequestLine { method_.clone(), target_.clone(), version_ };
}

StatusLine::StatusLine(Option<MessageVersion> version,
                       StatusCode             status,
                       Option<HeaderValue>    reason) noexcept
    : version_(rstd::move(version)), status_(status), reason_(rstd::move(reason)) {
}

auto StatusLine::version() const noexcept -> Option<MessageVersion> {
    return version_.clone();
}

auto StatusLine::status() const noexcept -> StatusCode {
    return status_;
}

auto StatusLine::reason() const noexcept -> Option<ref<HeaderValue>> {
    if (reason_.is_none()) return None();
    return Some(ref<HeaderValue>::from_raw_parts(&*reason_));
}

auto StatusLine::clone() const -> StatusLine {
    return StatusLine { version_.clone(), status_, reason_.clone() };
}

auto StartLine::clone() const -> StartLine {
    RSTD_MATCH(*this) {
        RSTD_CASE(Request, value) {
            return StartLine::Request(value.clone());
        }
        RSTD_CASE(Response, value) {
            return StartLine::Response(value.clone());
        }
    }
    rstd::panic { "invalid HTTP start line" };
}

MessageHead::MessageHead(StartLine start, Headers headers) noexcept
    : start_(rstd::move(start)), headers_(rstd::move(headers)) {
}

auto MessageHead::parse(slice<u8> input) -> rstd::Result<MessageHead, HttpParseError> {
    auto parser = Http1HeadParser {};
    auto parsed = parser.push(input);
    if (parsed.is_err()) return Err(rstd::move(parsed).unwrap_err());
    auto event = rstd::move(parsed).unwrap();
    if (event.is_Complete()) {
        auto complete = rstd::move(event).as_Complete();
        if (complete.consumed != input.len()) {
            return Err(HttpParseError { HttpParseErrorKind::InvalidSyntax(), complete.consumed });
        }
        return Ok(rstd::move(complete.head));
    }
    return parser.finish();
}

auto MessageHead::start() const noexcept -> const StartLine& {
    return start_;
}

auto MessageHead::headers() const noexcept -> const Headers& {
    return headers_;
}

auto MessageHead::status_code() const noexcept -> Option<u16> {
    if (! start_.is_Response()) return None();
    return Some(start_.as_Response().value.status().value());
}

auto MessageHead::has_field(ref<str> name) const noexcept -> bool {
    return headers_.contains(name);
}

auto MessageHead::clone() const -> MessageHead {
    return MessageHead { start_.clone(), headers_.clone() };
}

auto MessageHead::into_response() && -> Result<ResponseHead, HttpParseError> {
    if (! start_.is_Response())
        return Err(HttpParseError { HttpParseErrorKind::InvalidStartLine(), usize() });
    const auto&         status = start_.as_Response().value;
    auto                reason = status.reason();
    Option<HeaderValue> owned_reason;
    if (reason.is_some()) owned_reason = Some((**reason).clone());
    return Ok(ResponseHead {
        status.status(), rstd::move(headers_), status.version(), rstd::move(owned_reason) });
}

auto Http1HeadParser::push(slice<u8> input) -> rstd::Result<Http1HeadEvent, HttpParseError> {
    if (complete_ || failed_) {
        return Err(HttpParseError { HttpParseErrorKind::InvalidSyntax(), line_start_ });
    }
    failed_             = true;
    auto previous_bytes = buffer_.len();
    auto available      = max_header_bytes_ - buffer_.len();
    auto count          = input.len() < available ? input.len() : available;
    buffer_.extend_from_slice(slice<u8>::from_raw_parts(input.as_raw_ptr(), count));

    while (scan_ + usize(1) < buffer_.len()) {
        if (static_cast<u8>(buffer_[scan_]).to_primitive() != '\r' ||
            static_cast<u8>(buffer_[scan_ + usize(1)]).to_primitive() != '\n') {
            ++scan_;
            continue;
        }

        auto line_end = scan_ + usize(2);
        if (line_end > max_header_bytes_) {
            return Err(HttpParseError { HttpParseErrorKind::HeaderTooLarge(), max_header_bytes_ });
        }
        auto line = slice<u8>::from_raw_parts(
            buffer_.as_slice().as_raw_ptr() + line_start_.to_primitive(), line_end - line_start_);
        auto base   = line_start_;
        line_start_ = line_end;
        scan_       = line_end;

        if (start_.is_none()) {
            auto parsed = parser::http1::parse_start_line(line, transport_metadata_);
            if (parsed.is_err()) return Err(start_line_error(rstd::move(parsed).unwrap_err()));
            auto start = make_start_line(line, rstd::move(parsed).unwrap());
            if (start.is_err()) return Err(rstd::move(start).unwrap_err());
            start_ = Some(rstd::move(start).unwrap());
            continue;
        }

        if (line.len() == usize(2)) {
            complete_ = true;
            return Ok(Http1HeadEvent::Complete(
                MessageHead { rstd::move(start_).unwrap(), rstd::move(headers_) },
                line_start_,
                line_start_ - previous_bytes));
        }

        auto content = slice<u8>::from_raw_parts(line.as_raw_ptr(), line.len() - usize(2));
        auto field   = Header::parse_line(content);
        if (field.is_err())
            return Err(HttpParseError { HttpParseErrorKind::InvalidHeaderLine(),
                                        base + field.unwrap_err().offset() });
        headers_.push(rstd::move(field).unwrap());
    }
    if (input.len() > count) {
        return Err(HttpParseError { HttpParseErrorKind::HeaderTooLarge(), max_header_bytes_ });
    }
    failed_ = false;
    return Ok(Http1HeadEvent::NeedMore());
}

auto Http1HeadParser::finish() -> rstd::Result<MessageHead, HttpParseError> {
    failed_ = true;
    return Err(HttpParseError { HttpParseErrorKind::UnexpectedEof(), buffer_.len() });
}

auto Http1FieldSectionParser::push(slice<u8> input)
    -> rstd::Result<Http1FieldSectionEvent, HttpParseError> {
    if (complete_ || failed_) {
        return Err(HttpParseError { HttpParseErrorKind::InvalidSyntax(), line_start_ });
    }
    failed_        = true;
    auto available = max_header_bytes_ - buffer_.len();
    auto count     = input.len() < available ? input.len() : available;
    buffer_.extend_from_slice(slice<u8>::from_raw_parts(input.as_raw_ptr(), count));

    while (scan_ + usize(1) < buffer_.len()) {
        if (static_cast<u8>(buffer_[scan_]).to_primitive() != '\r' ||
            static_cast<u8>(buffer_[scan_ + usize(1)]).to_primitive() != '\n') {
            ++scan_;
            continue;
        }

        auto line_end = scan_ + usize(2);
        if (line_end > max_header_bytes_) {
            return Err(HttpParseError { HttpParseErrorKind::HeaderTooLarge(), max_header_bytes_ });
        }
        auto line = slice<u8>::from_raw_parts(
            buffer_.as_slice().as_raw_ptr() + line_start_.to_primitive(), line_end - line_start_);
        auto base   = line_start_;
        line_start_ = line_end;
        scan_       = line_end;

        if (line.len() == usize(2)) {
            complete_ = true;
            return Ok(Http1FieldSectionEvent::Complete(rstd::move(fields_), line_start_));
        }

        auto content = slice<u8>::from_raw_parts(line.as_raw_ptr(), line.len() - usize(2));
        auto field   = Header::parse_line(content);
        if (field.is_err())
            return Err(HttpParseError { HttpParseErrorKind::InvalidHeaderLine(),
                                        base + field.unwrap_err().offset() });
        fields_.push(rstd::move(field).unwrap());
    }
    if (input.len() > count) {
        return Err(HttpParseError { HttpParseErrorKind::HeaderTooLarge(), max_header_bytes_ });
    }
    failed_ = false;
    return Ok(Http1FieldSectionEvent::NeedMore());
}

auto Http1FieldSectionParser::finish() -> rstd::Result<Headers, HttpParseError> {
    failed_ = true;
    return Err(HttpParseError { HttpParseErrorKind::UnexpectedEof(), buffer_.len() });
}

} // namespace lihttpto
