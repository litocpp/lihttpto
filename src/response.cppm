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
    Vec<Header>        headers;
    rstd::bytes::Bytes body;
};
enum class ResponseError
{
    InvalidStatus,
    InvalidHeader,
    ReservedHeader,
    InvalidBody,
    TooLarge,
    Io,
    Timeout
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
auto omit_response_body(u16 status, ref<str> method) -> bool {
    return method == "HEAD"_str || status == u16(204) || status == u16(205) || status == u16(304);
}

auto write_with_deadline(rstd::net::TcpStream&     stream,
                         const rstd::bytes::Bytes& bytes,
                         rstd::time::Instant       started,
                         rstd::time::Duration      budget)
    -> rstd::async::coro<Result<empty, ResponseWriteError>> {
    auto elapsed = started.elapsed();
    if (elapsed >= budget) co_return Err(ResponseWriteError { ResponseError::Timeout, None() });
    auto timed =
        co_await rstd::async::timeout(rstd::async::io::write_all(stream, bytes), budget - elapsed);
    if (timed.is_err()) co_return Err(ResponseWriteError { ResponseError::Timeout, None() });
    auto written = rstd::move(timed).unwrap();
    if (written.is_err())
        co_return Err(
            ResponseWriteError { ResponseError::Io, Some(rstd::move(written).unwrap_err()) });
    co_return Ok(empty {});
}
} // namespace lihttpto

export namespace lihttpto
{
auto encode_response_head(const Response& response,
                          ref<str>        request_method,
                          Version         version,
                          bool            keep_alive = false,
                          usize limit = usize(65536)) -> Result<rstd::bytes::Bytes, ResponseError> {
    if (response.status < u16(200) || response.status > u16(599) ||
        (request_method == "CONNECT"_str && response.status < u16(300)))
        return Err(ResponseError::InvalidStatus);
    if ((response.status == u16(204) || response.status == u16(205) ||
         response.status == u16(304)) &&
        ! response.body.is_empty())
        return Err(ResponseError::InvalidBody);
    Vec<u8> output;
    auto    append = [&](ref<str> value) {
        return append_bytes(output, value.as_bytes(), limit);
    };
    if (! append(version == Version::Http11 ? "HTTP/1.1 "_str : "HTTP/1.0 "_str) ||
        ! append_decimal(output, u64(response.status.to_primitive()), limit) ||
        ! append(" \r\n"_str))
        return Err(ResponseError::TooLarge);
    for (const auto& header : response.headers) {
        auto                    name = header.name.as_str();
        rstd::parse::TextCursor cursor(rstd::parse::text_input(name));
        if (rstd::parse::consume_while_one(cursor, token_byte).is_none() || ! cursor.is_eof())
            return Err(ResponseError::InvalidHeader);
        if (ascii_equal(name.as_bytes(), "content-length"_str) ||
            ascii_equal(name.as_bytes(), "transfer-encoding"_str) ||
            ascii_equal(name.as_bytes(), "connection"_str) ||
            ascii_equal(name.as_bytes(), "trailer"_str))
            return Err(ResponseError::ReservedHeader);
        for (auto byte : header.value.as_slice())
            if (! (byte == u8('\t') || (byte >= u8(32) && byte != u8(127))))
                return Err(ResponseError::InvalidHeader);
        if (! append(name) || ! append(": "_str) ||
            ! append_bytes(output, header.value.as_slice(), limit) || ! append("\r\n"_str))
            return Err(ResponseError::TooLarge);
    }
    if (response.status != u16(204) && response.status != u16(304)) {
        if (! append("Content-Length: "_str) ||
            ! append_decimal(output, u64(response.body.len().to_primitive()), limit) ||
            ! append("\r\n"_str))
            return Err(ResponseError::TooLarge);
    }
    if (! append(keep_alive ? "Connection: keep-alive\r\n\r\n"_str
                            : "Connection: close\r\n\r\n"_str))
        return Err(ResponseError::TooLarge);
    return Ok(rstd::bytes::Bytes::copy_from_slice(output.as_slice()));
}

auto write_response(rstd::net::TcpStream& stream,
                    const Response&       response,
                    ref<str>              request_method,
                    Version               version,
                    bool                  keep_alive = false,
                    rstd::time::Duration  budget     = rstd::time::Duration::from_secs(u64(30)))
    -> rstd::async::coro<Result<empty, ResponseWriteError>> {
    auto head = encode_response_head(response, request_method, version, keep_alive);
    if (head.is_err()) co_return Err(ResponseWriteError { head.unwrap_err(), None() });
    auto bytes   = rstd::move(head).unwrap();
    auto started = rstd::time::Instant::now();
    rstd_co_try(co_await write_with_deadline(stream, bytes, started, budget));
    if (! omit_response_body(response.status, request_method) && ! response.body.is_empty()) {
        rstd_co_try(co_await write_with_deadline(stream, response.body, started, budget));
    }
    co_return Ok(empty {});
}
} // namespace lihttpto
