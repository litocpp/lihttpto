#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;
using rstd::bytes::Bytes;

TEST(ProxyCodec, AbsoluteTargetAndConnectAuthorityUseParsedUrl) {
    RequestParts parts { Url::parse_http("http://EXAMPLE.test:8080/a?q=%2F#ignored"_str).unwrap(),
                         Method::parse("GET"_str).unwrap(),
                         {} };
    auto         encoder =
        RequestEncoder::make(parts, Some(u64()), usize(65536), nullptr, TargetForm::Absolute)
            .unwrap();
    EXPECT_TRUE(rstd::str_::from_utf8(encoder.take_head().unwrap().as_slice())
                    .unwrap()
                    .starts_with("GET http://example.test:8080/a?q=%2F HTTP/1.1\r\n"_str));
    auto url = Url::parse_http("https://[::1]/a"_str).unwrap();
    EXPECT_EQ(url.host_name().unwrap(), "::1"_str);
    EXPECT_EQ(rstd::str_::from_utf8(connect_request(url).unwrap().as_slice()).unwrap(),
              "CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]:443\r\n\r\n"_str);
    EXPECT_TRUE(connect_request(url, usize(4)).is_err());
}

TEST(ProxyCodec, ConnectDecoderPreservesBytesAndRejectsUnsafeFailureFraming) {
    auto head =
        "HTTP/1.1 200 Connected\r\nContent-Length: 9\r\nTransfer-Encoding: chunked\r\n\r\n"_bytes;
    for (usize split; split < head.len(); ++split) {
        TunnelHeadDecoder decoder;
        EXPECT_TRUE(
            decoder.feed(slice<u8>::from_raw_parts(head.as_raw_ptr(), split)).unwrap().is_none());
        auto tail =
            slice<u8>::from_raw_parts(head.as_raw_ptr() + split.to_primitive(), head.len() - split);
        auto result = decoder.feed(tail).unwrap().unwrap();
        EXPECT_EQ(result.disposition, TunnelDisposition::Established);
        EXPECT_EQ(result.input_consumed, tail.len());
        EXPECT_TRUE(decoder.feed("extra"_bytes).is_err());
    }
    TunnelHeadDecoder decoder;
    EXPECT_EQ(decoder.feed("HTTP/1.1 100 Continue\r\n\r\n"_bytes).unwrap().unwrap().disposition,
              TunnelDisposition::Informational);
    auto established = decoder.feed("HTTP/1.1 200 Connected\r\n\r\nready"_bytes).unwrap().unwrap();
    EXPECT_EQ(established.input_consumed, usize(26));
    TunnelHeadDecoder rejected;
    auto failure = rejected.feed("HTTP/1.1 407 Auth\r\nContent-Length: 6\r\n\r\ndenied"_bytes)
                       .unwrap()
                       .unwrap();
    EXPECT_EQ(failure.disposition, TunnelDisposition::Rejected);
    EXPECT_EQ(failure.framing.length, u64(6));
    TunnelHeadDecoder bad;
    EXPECT_TRUE(
        bad.feed(
               "HTTP/1.1 407 Auth\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"_bytes)
            .is_err());
    TunnelHeadDecoder limited(usize(55));
    EXPECT_TRUE(limited.feed("HTTP/1.1 100 Continue\r\n\r\n"_bytes).is_ok());
    EXPECT_TRUE(limited.feed("HTTP/1.1 100 Continue\r\n\r\n"_bytes).is_ok());
    EXPECT_TRUE(limited.feed("HTTP/1.1 200 Connected\r\n\r\n"_bytes).is_err());
}

TEST(RequestEncoder, OriginHostFramingAndExactLength) {
    RequestParts parts { Url::parse_http("http://example.test:8080/a?q=1#ignored"_str).unwrap(),
                         Method::parse("POST"_str).unwrap(),
                         {} };
    auto         encoder = RequestEncoder::make(parts, Some(u64(3))).unwrap();
    auto         head    = encoder.take_head().unwrap();
    EXPECT_EQ(rstd::str_::from_utf8(head.as_slice()).unwrap(),
              "POST /a?q=1 HTTP/1.1\r\nHost: example.test:8080\r\nContent-Length: 3\r\n\r\n"_str);
    EXPECT_TRUE(
        encoder.encode(BodyFrame::Data(Bytes::copy_from_slice("abc"_str.as_bytes()))).is_ok());
    EXPECT_TRUE(encoder.finish().is_ok());
    parts.headers.add("Host"_str, "attacker.test"_str).unwrap();
    EXPECT_TRUE(RequestEncoder::make(parts).is_err());
    parts.headers.remove("host"_str);
    parts.headers.add("Content-Length"_str, "3"_str).unwrap();
    parts.headers.add("Transfer-Encoding"_str, "chunked"_str).unwrap();
    EXPECT_TRUE(RequestEncoder::make(parts, Some(u64(3))).is_err());
}
TEST(RequestEncoder, ChunkedTrailersAndRejectsUnsafeTargets) {
    RequestParts parts { Url::parse_http("https://[::1]/"_str).unwrap(),
                         Method::parse("POST"_str).unwrap(),
                         {} };
    auto         encoder = RequestEncoder::make(parts, {}).unwrap();
    EXPECT_TRUE(rstd::str_::from_utf8(encoder.take_head().unwrap().as_slice())
                    .unwrap()
                    .contains("Host: [::1]\r\n"_str));
    EXPECT_EQ(rstd::str_::from_utf8(
                  encoder.encode(BodyFrame::Data(Bytes::copy_from_slice("abc"_str.as_bytes())))
                      .unwrap()
                      .as_slice())
                  .unwrap(),
              "3\r\nabc\r\n"_str);
    Headers trailers;
    trailers.add("X-End"_str, "yes"_str).unwrap();
    EXPECT_TRUE(encoder.encode(BodyFrame::Trailers(rstd::move(trailers))).is_ok());
    EXPECT_TRUE(encoder.finish().unwrap().is_empty());
    parts.url = Url::parse_http("http://user:pass@example.test/"_str).unwrap();
    EXPECT_TRUE(RequestEncoder::make(parts).is_err());
    parts.url    = Url::parse_http("http://example.test/"_str).unwrap();
    parts.method = Method::parse("CONNECT"_str).unwrap();
    EXPECT_TRUE(RequestEncoder::make(parts).is_err());
}
TEST(ResponseHeadDecoder, EverySplitPreservesFinalHeadAndRemainder) {
    auto wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nX-Bin: \xff\r\nX-Dup: a\r\nX-Dup: b\r\n\r\nabc"_bytes;
    for (usize split; split < wire.len() - usize(3); ++split) {
        ResponseHeadDecoder decoder(Method::parse("GET"_str).unwrap());
        auto first = decoder.feed(slice<u8>::from_raw_parts(wire.as_raw_ptr(), split)).unwrap();
        EXPECT_TRUE(first.is_none());
        auto rest =
            slice<u8>::from_raw_parts(wire.as_raw_ptr() + split.to_primitive(), wire.len() - split);
        auto last = decoder.feed(rest).unwrap().unwrap();
        EXPECT_EQ(last.framing.length, u64(3));
        EXPECT_TRUE(last.keep_alive);
        EXPECT_EQ(rest.len() - last.input_consumed, usize(3));
        EXPECT_EQ(last.head.headers.get_all("x-dup"_str).len(), usize(2));
        EXPECT_EQ((*last.head.headers.get("x-bin"_str))->as_slice()[usize()], u8(255));
    }
}
TEST(ResponseHeadDecoder, InformationalBudgetAndStrictFraming) {
    ResponseHeadDecoder decoder(Method::parse("GET"_str).unwrap(), usize(65536), usize(1));
    auto info = decoder.feed("HTTP/1.1 103 Early Hints\r\n\r\n"_bytes).unwrap().unwrap();
    EXPECT_TRUE(info.informational);
    EXPECT_TRUE(decoder.feed("HTTP/1.1 100 Continue\r\n\r\n"_bytes).is_err());
    for (auto wire : array<slice<u8>, 4> {
             "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n"_bytes,
             "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n"_bytes,
             "HTTP/1.1 101 Switching Protocols\r\n\r\n"_bytes,
             "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"_bytes }) {
        ResponseHeadDecoder bad(Method::parse("GET"_str).unwrap());
        EXPECT_TRUE(bad.feed(wire).is_err());
    }
    ResponseHeadDecoder head(Method::parse("HEAD"_str).unwrap());
    EXPECT_EQ(
        head.feed("HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n"_bytes).unwrap()->framing.kind,
        BodyKind::None);
    ResponseHeadDecoder old(Method::parse("GET"_str).unwrap());
    EXPECT_FALSE(
        old.feed("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"_bytes).unwrap()->keep_alive);
    ResponseHeadDecoder partial(Method::parse("GET"_str).unwrap());
    EXPECT_TRUE(partial.feed("HTTP/1.1 2"_bytes).unwrap().is_none());
    EXPECT_TRUE(partial.finish().is_err());
}
TEST(HttpValues, NumericUrlAddressUsesParsedHost) {
    EXPECT_TRUE(
        Url::parse_http("http://127.1:8080/"_str).unwrap().numeric_address()->is_loopback());
    EXPECT_EQ(Url::parse_http("http://[::1]:8080/"_str).unwrap().numeric_address()->port(),
              u16(8080));
    EXPECT_TRUE(Url::parse_http("http://example.test/"_str).unwrap().numeric_address().is_none());
}

TEST(RequestEncoder, PreparedKnownLengthTrailersUseChunkingAndFreshEncodingState) {
    RequestParts parts { Url::parse_http("http://example.test/trailers"_str).unwrap(),
                         Method::parse("POST"_str).unwrap(),
                         {} };
    Headers      trailers;
    trailers.add("X-End"_str, "yes"_str).unwrap();
    auto prepared =
        PreparedRequest::make(parts.clone(), Some(u64(3)), usize(65536), &trailers).unwrap();
    for (int i = 0; i < 2; ++i) {
        auto encoder = prepared.encoder();
        auto head    = encoder.take_head().unwrap();
        auto text    = rstd::str_::from_utf8(head.as_slice()).unwrap();
        EXPECT_TRUE(text.contains("Transfer-Encoding: chunked\r\n"_str));
        EXPECT_FALSE(text.contains("Content-Length"_str));
        EXPECT_TRUE(
            encoder.encode(BodyFrame::Data(Bytes::copy_from_slice("abc"_str.as_bytes()))).is_ok());
        EXPECT_TRUE(encoder.encode(BodyFrame::Trailers(trailers.clone())).is_ok());
        EXPECT_TRUE(encoder.finish().is_ok());
    }
    auto short_body = prepared.encoder();
    (void)short_body.take_head();
    EXPECT_TRUE(
        short_body.encode(BodyFrame::Data(Bytes::copy_from_slice("ab"_str.as_bytes()))).is_ok());
    EXPECT_TRUE(short_body.encode(BodyFrame::Trailers(trailers.clone())).is_err());
    auto long_body = prepared.encoder();
    (void)long_body.take_head();
    EXPECT_TRUE(
        long_body.encode(BodyFrame::Data(Bytes::copy_from_slice("abcd"_str.as_bytes()))).is_err());
    parts.headers.set("Content-Length"_str, "3"_str).unwrap();
    EXPECT_TRUE(
        PreparedRequest::make(rstd::move(parts), Some(u64(3)), usize(65536), &trailers).is_err());
}
TEST(ClientFields, RedirectAndRetryAfterUseProtocolParsing) {
    auto         base = Url::parse_http("http://example.test/a/path"_str).unwrap();
    ResponseHead head { StatusCode::make(u16(307)).unwrap(), {}, {}, {} };
    head.headers.add("Location"_str, "../next"_str).unwrap();
    auto target = redirect_target(head, base).unwrap().unwrap();
    EXPECT_EQ(target.as_ref(), "http://example.test/next"_str);
    head.headers.add("Location"_str, "/other"_str).unwrap();
    EXPECT_TRUE(redirect_target(head, base).is_err());
    Headers fields;
    fields.add("Retry-After"_str, "10"_str).unwrap();
    EXPECT_EQ(retry_after(fields).unwrap()->delay(i64(100)), i64(10));
    fields.set("Retry-After"_str, "Sun, 06 Nov 1994 08:49:37 GMT"_str).unwrap();
    auto date = http_date("Sun, 06 Nov 1994 08:49:37 GMT"_str).unwrap();
    EXPECT_EQ(retry_after(fields).unwrap()->delay(date - i64(2)), i64(2));
    fields.set("Retry-After"_str, "invalid"_str).unwrap();
    EXPECT_TRUE(retry_after(fields).is_err());
}
