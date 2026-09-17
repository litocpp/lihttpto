#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

namespace
{
auto header(ref<str> name, ref<str> value) -> Header {
    Vec<u8> bytes;
    for (auto byte : value.bytes()) bytes.push(u8(byte));
    return Header { String::make(name), rstd::move(bytes) };
}
auto make_response() -> Response {
    Response response;
    response.body = rstd::bytes::Bytes::copy_from_slice("hello"_str.as_bytes());
    response.headers.push(header("Content-Type"_str, "text/plain"_str));
    return response;
}

auto serve(rstd::net::TcpStream stream) -> rstd::async::coro<bool> {
    RequestHeadDecoder decoder;
    auto               buffer = rstd::bytes::BytesMut::with_capacity(usize(16));
    for (;;) {
        buffer.clear();
        auto read = co_await rstd::async::io::read(stream, buffer);
        if (read.is_err() || *read == usize()) co_return false;
        auto progress = decoder.feed(buffer.as_slice());
        if (progress.is_err()) co_return false;
        if (progress->status == DecodeStatus::Complete) break;
    }
    auto request = decoder.take();
    if (request.is_none() || request->body.kind != BodyKind::None) co_return false;
    auto response = make_response();
    auto written  = co_await write_response(
        stream, response, request->line.method.as_str(), request->line.version);
    if (written.is_err()) co_return false;
    co_return stream.shutdown().is_ok();
}

auto round_trip(rstd::net::TcpListener& listener, rstd::net::SocketAddr address, ref<str> method)
    -> rstd::async::coro<bool> {
    auto connection = co_await rstd::net::TcpStream::connect(address);
    if (connection.is_err()) co_return false;
    auto accepted = co_await listener.accept();
    if (accepted.is_err()) co_return false;
    auto pair   = rstd::move(accepted).unwrap();
    auto task   = rstd::async::spawn(serve(rstd::move(pair.template get<0>())));
    auto client = rstd::move(connection).unwrap();
    auto prefix = rstd::bytes::Bytes::copy_from_slice(method.as_bytes());
    auto suffix = rstd::bytes::Bytes::copy_from_slice(
        " / HTTP/1.1\r\nHost: localhost\r\n\r\n"_str.as_bytes());
    if ((co_await rstd::async::io::write_all(client, prefix)).is_err()) co_return false;
    if ((co_await rstd::async::io::write_all(client, suffix)).is_err()) co_return false;
    Vec<u8> output;
    auto    buffer = rstd::bytes::BytesMut::with_capacity(usize(7));
    for (;;) {
        buffer.clear();
        auto read = co_await rstd::async::io::read(client, buffer);
        if (read.is_err()) co_return false;
        if (*read == usize()) break;
        for (auto byte : buffer.as_slice()) output.push(u8(byte));
        if (output.len() > usize(4096)) co_return false;
    }
    auto joined = co_await rstd::move(task);
    if (joined.is_err() || ! *joined) co_return false;
    auto response = rstd::str_::from_utf8(output.as_slice());
    if (response.is_err()) co_return false;
    auto expected =
        method == "HEAD"_str
            ? "HTTP/1.1 200 \r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\n"_str
            : "HTTP/1.1 200 \r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"_str;
    co_return *response == expected;
}
} // namespace

TEST(Response, EncodesManagedFramingAndHeadMetadata) {
    auto response = make_response();
    auto encoded  = encode_response_head(response, "HEAD"_str, Version::Http10, true);
    ASSERT_TRUE(encoded.is_ok());
    auto text = rstd::str_::from_utf8(encoded->as_slice()).unwrap();
    EXPECT_EQ(
        text,
        "HTTP/1.0 200 \r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: keep-alive\r\n\r\n"_str);
    EXPECT_EQ(
        encode_response_head(response, "GET"_str, Version::Http11, false, encoded->len() - usize(6))
            .unwrap_err(),
        ResponseError::TooLarge);
}

TEST(Response, RejectsInjectionAndUserFraming) {
    auto names = array<ref<str>, 4> {
        "Content-Length"_str, "transfer-encoding"_str, "Connection"_str, "Trailer"_str
    };
    for (auto name : names) {
        auto response = make_response();
        response.headers.push(header(name, "x"_str));
        EXPECT_EQ(encode_response_head(response, "GET"_str, Version::Http11).unwrap_err(),
                  ResponseError::ReservedHeader);
    }
    auto response = make_response();
    response.headers.push(header("X"_str, "safe\r\nBad: injected"_str));
    EXPECT_EQ(encode_response_head(response, "GET"_str, Version::Http11).unwrap_err(),
              ResponseError::InvalidHeader);
}

TEST(Response, SpecialStatusBodyRules) {
    for (auto status : array<u16, 3> { u16(204), u16(205), u16(304) }) {
        auto invalid   = make_response();
        invalid.status = status;
        EXPECT_EQ(encode_response_head(invalid, "GET"_str, Version::Http11).unwrap_err(),
                  ResponseError::InvalidBody);
        Response empty_response;
        empty_response.status = status;
        auto encoded          = encode_response_head(empty_response, "GET"_str, Version::Http11);
        ASSERT_TRUE(encoded.is_ok());
        auto text = rstd::str_::from_utf8(encoded->as_slice()).unwrap();
        EXPECT_EQ(text.contains("Content-Length:"_str), status == u16(205));
    }
    Response response;
    response.status = u16(101);
    EXPECT_EQ(encode_response_head(response, "GET"_str, Version::Http11).unwrap_err(),
              ResponseError::InvalidStatus);
    response.status = u16(200);
    EXPECT_EQ(encode_response_head(response, "CONNECT"_str, Version::Http11).unwrap_err(),
              ResponseError::InvalidStatus);
}

TEST(Response, TcpLoopbackGetAndHead) {
    auto listener = rstd::net::TcpListener::bind(rstd::net::SocketAddr::ipv4_loopback(u16()));
    ASSERT_TRUE(listener.is_ok());
    auto tcp_listener = rstd::move(listener).unwrap();
    auto address      = tcp_listener.local_addr();
    ASSERT_TRUE(address.is_ok());
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    for (auto method : array<ref<str>, 2> { "GET"_str, "HEAD"_str }) {
        auto pending = rstd::async::AbortOnDropHandle { runtime.spawn(
            round_trip(tcp_listener, *address, method)) };
        auto result  = runtime.block_on(
            rstd::async::timeout(rstd::move(pending), rstd::time::Duration::from_secs(u64(5))));
        ASSERT_TRUE(result.is_ok());
        ASSERT_TRUE(result->is_ok());
        EXPECT_TRUE(**result);
    }
}

TEST(Response, StreamingUsesSharedValidationAndCloseDelimitedFraming) {
    StreamResponseHead response;
    response.headers.push(header("Content-Type"_str, "application/octet-stream"_str));
    for (auto version : array<Version, 2> { Version::Http10, Version::Http11 }) {
        auto encoded = encode_response_head(response, "GET"_str, version);
        ASSERT_TRUE(encoded.is_ok());
        auto text = rstd::str_::from_utf8(encoded->as_slice()).unwrap();
        EXPECT_TRUE(text.contains("Connection: close\r\n\r\n"_str));
        EXPECT_FALSE(text.contains("Content-Length:"_str));
        EXPECT_FALSE(text.contains("Transfer-Encoding:"_str));
    }
    response.headers.push(header("Content-Length"_str, "123"_str));
    EXPECT_EQ(encode_response_head(response, "GET"_str, Version::Http11).unwrap_err(),
              ResponseError::ReservedHeader);
    StreamResponseHead invalid;
    invalid.headers.push(header("X-Test"_str, "x\r\ninjected"_str));
    EXPECT_EQ(encode_response_head(invalid, "GET"_str, Version::Http11).unwrap_err(),
              ResponseError::InvalidHeader);
    StreamResponseHead reset;
    reset.status = u16(205);
    auto encoded = encode_response_head(reset, "GET"_str, Version::Http11).unwrap();
    EXPECT_TRUE(
        rstd::str_::from_utf8(encoded.as_slice()).unwrap().contains("Content-Length: 0"_str));
}
