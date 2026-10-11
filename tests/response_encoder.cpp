#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;
using rstd::bytes::Bytes;

auto encoder(u16         status  = u16(200),
             ref<str>    method  = "GET"_str,
             Version     version = Version::Http11,
             Option<u64> length  = {}) -> ResponseEncoder {
    ResponseHead head { StatusCode::make(status).unwrap(), {}, {}, {} };
    return ResponseEncoder::make(head, Method::parse(method).unwrap(), version, length).unwrap();
}
auto text(const Bytes& bytes) -> ref<str> {
    return rstd::str_::from_utf8(bytes.as_slice()).unwrap();
}
auto data(ref<str> value) -> BodyFrame {
    return BodyFrame::Data(Bytes::copy_from_slice(value.as_bytes()));
}

TEST(ResponseEncoder, UnknownLengthHttp11UsesChunkedAndKeepsAlive) {
    auto response = encoder();
    EXPECT_TRUE(response.keep_alive());
    auto head = response.take_head().unwrap();
    EXPECT_TRUE(text(head).contains("Transfer-Encoding: chunked\r\n"_str));
    EXPECT_FALSE(text(head).contains("Content-Length:"_str));
    EXPECT_EQ(text(response.encode(data("hello"_str)).unwrap()), "5\r\nhello\r\n"_str);
    EXPECT_TRUE(response.encode(data(""_str)).unwrap().is_empty());
    EXPECT_EQ(text(response.finish().unwrap()), "0\r\n\r\n"_str);
    EXPECT_EQ(response.finish().unwrap_err(), ResponseError::InvalidState);
}
TEST(ResponseEncoder, TrailersAreTerminalAndRoundtripThroughBodyDecoder) {
    auto    response = encoder();
    auto    head     = response.take_head().unwrap();
    auto    first    = response.encode(data("payload"_str)).unwrap();
    Headers fields;
    fields.add("X-End"_str, "yes"_str).unwrap();
    auto last = response.encode(BodyFrame::Trailers(rstd::move(fields))).unwrap();
    EXPECT_EQ(text(last), "0\r\nX-End: yes\r\n\r\n"_str);
    EXPECT_TRUE(response.finish().unwrap().is_empty());
    BodyDecoder decoder({ BodyKind::Chunked });
    auto        chunk = decoder.feed(first.as_slice()).unwrap();
    EXPECT_EQ(rstd::str_::from_utf8(chunk.data).unwrap(), "payload"_str);
    first.advance(chunk.consumed);
    EXPECT_TRUE(decoder.feed(first.as_slice()).is_ok());
    EXPECT_EQ(decoder.feed(last.as_slice()).unwrap().status, DecodeStatus::Complete);
    auto trailers = decoder.take_trailers().unwrap();
    EXPECT_EQ(*trailers.get_unique_text("x-end"_str).unwrap(), "yes"_str);
    EXPECT_EQ(response.encode(data("late"_str)).unwrap_err(), ResponseError::InvalidState);
}
TEST(ResponseEncoder, ValidatesForbiddenAndDuplicateTrailers) {
    auto response = encoder();
    response.take_head().unwrap();
    Headers fields;
    fields.add("Content-Length"_str, "1"_str).unwrap();
    EXPECT_EQ(response.encode(BodyFrame::Trailers(rstd::move(fields))).unwrap_err(),
              ResponseError::InvalidTrailer);
    auto duplicate = encoder();
    duplicate.take_head().unwrap();
    EXPECT_TRUE(duplicate.encode(BodyFrame::Trailers(Headers {})).is_ok());
    EXPECT_EQ(duplicate.encode(BodyFrame::Trailers(Headers {})).unwrap_err(),
              ResponseError::InvalidState);
}
TEST(ResponseEncoder, FixedLengthChecksOverflowUnderflowAndTrailerFraming) {
    auto short_body = encoder(u16(200), "GET"_str, Version::Http11, Some(u64(3)));
    EXPECT_TRUE(text(short_body.take_head().unwrap()).contains("Content-Length: 3\r\n"_str));
    EXPECT_TRUE(short_body.encode(data("a"_str)).is_ok());
    EXPECT_EQ(short_body.finish().unwrap_err(), ResponseError::InvalidBody);
    auto long_body = encoder(u16(200), "GET"_str, Version::Http11, Some(u64(1)));
    long_body.take_head().unwrap();
    EXPECT_EQ(long_body.encode(data("ab"_str)).unwrap_err(), ResponseError::InvalidBody);
    auto exact = encoder(u16(200), "GET"_str, Version::Http11, Some(u64(2)));
    exact.take_head().unwrap();
    EXPECT_EQ(text(exact.encode(data("ab"_str)).unwrap()), "ab"_str);
    EXPECT_TRUE(exact.finish().unwrap().is_empty());
    auto fixed = encoder(u16(200), "GET"_str, Version::Http11, Some(u64()));
    fixed.take_head().unwrap();
    EXPECT_EQ(fixed.encode(BodyFrame::Trailers(Headers {})).unwrap_err(),
              ResponseError::InvalidTrailer);
}
TEST(ResponseEncoder, Http10UnknownLengthClosesAndHeadDoesNotEmitBody) {
    auto old = encoder(u16(200), "GET"_str, Version::Http10);
    EXPECT_FALSE(old.keep_alive());
    auto head = old.take_head().unwrap();
    EXPECT_FALSE(text(head).contains("Transfer-Encoding:"_str));
    EXPECT_EQ(text(old.encode(data("raw"_str)).unwrap()), "raw"_str);
    EXPECT_TRUE(old.finish().unwrap().is_empty());
    auto omitted = encoder(u16(200), "HEAD"_str, Version::Http11, Some(u64(100)));
    EXPECT_TRUE(text(omitted.take_head().unwrap()).contains("Content-Length: 100\r\n"_str));
    EXPECT_TRUE(omitted.finish().unwrap().is_empty());
    for (auto status : array<u16, 3> { u16(204), u16(205), u16(304) }) {
        auto no_body = encoder(status);
        no_body.take_head().unwrap();
        EXPECT_EQ(no_body.encode(data("x"_str)).unwrap_err(), ResponseError::InvalidBody);
    }
    auto not_started = encoder();
    EXPECT_EQ(not_started.encode(data("x"_str)).unwrap_err(), ResponseError::InvalidState);
}
TEST(ResponseEncoder, MetadataLimitsAndReservedFieldsRemainOwnedByHttp) {
    ResponseHead head { StatusCode::make(u16(200)).unwrap(), {}, {}, {} };
    auto         method = Method::parse("GET"_str).unwrap();
    EXPECT_EQ(ResponseEncoder::make(head, method, Version::Http11, {}, true, usize(8)).unwrap_err(),
              ResponseError::TooLarge);
    head.headers.add("Transfer-Encoding"_str, "chunked"_str).unwrap();
    EXPECT_EQ(ResponseEncoder::make(head, method, Version::Http11).unwrap_err(),
              ResponseError::ReservedHeader);
}
