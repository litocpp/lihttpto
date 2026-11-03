module;
#include <rstd/macro.hpp>
export module lihttpto:response;
export import :request_head;
export import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using IoError = rstd::io::error::Error;

export namespace lihttpto
{
struct Response {
    u16                status { 200 };
    Headers            headers;
    rstd::bytes::Bytes body;
};
struct StreamResponseHead {
    u16         status { 200 };
    Headers     headers;
    Option<u64> content_length;
};
enum class ResponseError
{
    InvalidStatus,
    InvalidHeader,
    ReservedHeader,
    InvalidBody,
    TooLarge,
    Io,
    Timeout,
    InvalidState,
    InvalidTrailer
};
struct ResponseWriteError {
    ResponseError   kind;
    Option<IoError> cause;
};
} // namespace lihttpto

namespace lihttpto
{
auto append_bytes(Vec<u8>& output, slice<u8> value, usize limit) -> bool {
    if (value.len() > limit - output.len()) return false;
    for (auto byte : value) output.push(u8(byte));
    return true;
}
auto append_decimal(Vec<u8>& output, u64 value, usize limit) -> bool {
    array<u8, 20> digits {};
    usize         count {};
    do {
        digits[count++] =
            u8(static_cast<rstd::uint8_t>((value % u64(10)).to_primitive())) + u8('0');
        value /= u64(10);
    } while (value != u64());
    if (count > limit - output.len()) return false;
    while (count != usize()) output.push(u8(digits[--count]));
    return true;
}
export auto omit_response_body(u16 status, ref<str> method) -> bool {
    return method == "HEAD"_str || status == u16(204) || status == u16(205) || status == u16(304);
}

} // namespace lihttpto

namespace lihttpto
{
auto validate_response_metadata(u16           status,
                                slice<Header> headers,
                                Option<u64>   length,
                                ref<str>      request_method) -> Result<empty, ResponseError> {
    if (status < u16(200) || status > u16(599) ||
        (request_method == "CONNECT"_str && status < u16(300)))
        return Err(ResponseError::InvalidStatus);
    if ((status == u16(204) || status == u16(205) || status == u16(304)) && length.is_some() &&
        *length != u64())
        return Err(ResponseError::InvalidBody);
    for (const auto& header : headers) {
        if (header.name.matches("content-length"_str) ||
            header.name.matches("transfer-encoding"_str) || header.name.matches("connection"_str) ||
            header.name.matches("trailer"_str))
            return Err(ResponseError::ReservedHeader);
    }
    return Ok(empty {});
}
} // namespace lihttpto

namespace lihttpto
{
auto encode_response_metadata(u16                      status,
                              slice<Header>            headers,
                              Option<u64>              length,
                              ref<str>                 request_method,
                              Version                  version,
                              bool                     keep_alive = false,
                              usize                    limit      = usize(65536),
                              bool                     chunked    = false,
                              Option<ref<HeaderValue>> reason     = {})
    -> Result<rstd::bytes::Bytes, ResponseError> {
    rstd_try(validate_response_metadata(status, headers, length, request_method));
    if (chunked && (version != Version::Http11 || length.is_some() ||
                    omit_response_body(status, request_method)))
        return Err(ResponseError::InvalidBody);
    Vec<u8> output;
    auto    append = [&](ref<str> value) {
        return append_bytes(output, value.as_bytes(), limit);
    };
    if (! append(version == Version::Http11 ? "HTTP/1.1 "_str : "HTTP/1.0 "_str) ||
        ! append_decimal(output, u64(status.to_primitive()), limit) || ! append(" "_str))
        return Err(ResponseError::TooLarge);
    if ((reason.is_some() && ! append_bytes(output, (*reason)->as_slice(), limit)) ||
        ! append("\r\n"_str))
        return Err(ResponseError::TooLarge);
    for (const auto& header : headers) {
        auto name = header.name.as_str();
        if (! append(name) || ! append(": "_str) ||
            ! append_bytes(output, header.value.as_slice(), limit) || ! append("\r\n"_str))
            return Err(ResponseError::TooLarge);
    }
    if (status != u16(204) && status != u16(304) && (length.is_some() || status == u16(205))) {
        if (! append("Content-Length: "_str) ||
            ! append_decimal(output, length.is_some() ? *length : u64(), limit) ||
            ! append("\r\n"_str))
            return Err(ResponseError::TooLarge);
    }
    if (chunked && ! append("Transfer-Encoding: chunked\r\n"_str))
        return Err(ResponseError::TooLarge);
    if (! append(keep_alive ? "Connection: keep-alive\r\n\r\n"_str
                            : "Connection: close\r\n\r\n"_str))
        return Err(ResponseError::TooLarge);
    return Ok(rstd::bytes::Bytes::copy_from_slice(output.as_slice()));
}
} // namespace lihttpto

export namespace lihttpto
{
auto validate_response_head(const StreamResponseHead& response, ref<str> request_method)
    -> Result<empty, ResponseError> {
    return validate_response_metadata(
        response.status, response.headers.as_slice(), response.content_length, request_method);
}

auto encode_response_head(const Response& response,
                          ref<str>        request_method,
                          Version         version,
                          bool            keep_alive = false,
                          usize limit = usize(65536)) -> Result<rstd::bytes::Bytes, ResponseError> {
    return encode_response_metadata(response.status,
                                    response.headers.as_slice(),
                                    Some(u64(response.body.len().to_primitive())),
                                    request_method,
                                    version,
                                    keep_alive,
                                    limit);
}

auto encode_response_head(const StreamResponseHead& response,
                          ref<str>                  request_method,
                          Version                   version,
                          usize limit = usize(65536)) -> Result<rstd::bytes::Bytes, ResponseError> {
    return encode_response_metadata(response.status,
                                    response.headers.as_slice(),
                                    response.content_length,
                                    request_method,
                                    version,
                                    false,
                                    limit);
}

} // namespace lihttpto

export namespace rstd
{
template<>
struct Impl<fmt::Display, lihttpto::ResponseError> : ImplBase<lihttpto::ResponseError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        switch (this->self()) {
        case lihttpto::ResponseError::InvalidStatus: return f.write_str("invalid HTTP status"_str);
        case lihttpto::ResponseError::InvalidHeader: return f.write_str("invalid HTTP field"_str);
        case lihttpto::ResponseError::ReservedHeader: return f.write_str("reserved HTTP field"_str);
        case lihttpto::ResponseError::InvalidBody: return f.write_str("invalid HTTP body"_str);
        case lihttpto::ResponseError::TooLarge:
            return f.write_str("HTTP encoding exceeds limit"_str);
        case lihttpto::ResponseError::Io: return f.write_str("HTTP transport failed"_str);
        case lihttpto::ResponseError::Timeout: return f.write_str("HTTP encoding timed out"_str);
        case lihttpto::ResponseError::InvalidState:
            return f.write_str("invalid HTTP encoder state"_str);
        case lihttpto::ResponseError::InvalidTrailer:
            return f.write_str("invalid HTTP trailer"_str);
        }
        return false;
    }
};
template<>
struct Impl<fmt::Debug, lihttpto::ResponseError> : Impl<fmt::Display, lihttpto::ResponseError> {};
template<>
struct Impl<error::Error, lihttpto::ResponseError>
    : DefaultInImpl<error::Error, lihttpto::ResponseError> {};
} // namespace rstd
