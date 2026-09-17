#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

TEST(Header, ConstructionValidatesNamesAndPreservesRawValues) {
    auto name = HeaderName::make("X-Custom"_str).unwrap();
    EXPECT_TRUE(name.matches("x-CUSTOM"_str));
    EXPECT_EQ(name.as_str(), "X-Custom"_str);
    for (auto invalid :
         array<ref<str>, 5> { ""_str, "bad name"_str, "bad:name"_str, "x\n"_str, "名称"_str })
        EXPECT_TRUE(HeaderName::make(invalid).unwrap_err().is_InvalidName());
    auto raw   = array<u8, 3> { u8('\t'), u8(128), u8(255) };
    auto value = HeaderValue::make(raw.as_slice()).unwrap();
    EXPECT_EQ(value.as_slice().len(), usize(3));
    EXPECT_EQ(value[usize(2)], u8(255));
    EXPECT_TRUE(value.to_str().unwrap_err().is_InvalidText());
    for (auto invalid : array<ref<str>, 4> { "x\r\ny"_str, "x\0y"_str, "x\ny"_str, "x\x7fy"_str })
        EXPECT_TRUE(HeaderValue::make(invalid.as_bytes()).unwrap_err().is_InvalidValue());
}

TEST(Header, LineParsingOwnsFieldsAndTrimsOnlySurroundingWhitespace) {
    auto header = Header::parse_line("X-Test:\t a\tb \t"_str.as_bytes()).unwrap();
    EXPECT_EQ(header.name.as_str(), "X-Test"_str);
    EXPECT_EQ(header.value.to_str().unwrap(), "a\tb"_str);
    EXPECT_TRUE(Header::parse_line("X-Test : value"_str.as_bytes()).is_err());
    EXPECT_TRUE(Header::parse_line("X-Test value"_str.as_bytes()).is_err());
    EXPECT_TRUE(Header::parse_line("X-Test: a\rb"_str.as_bytes()).is_err());
    EXPECT_TRUE(Header::parse_line("X-Test:"_str.as_bytes()).is_ok());
}

TEST(Header, CollectionPreservesDuplicatesAndSeparatesTextConversion) {
    Headers headers;
    headers.push(Header::make("Set-Cookie"_str, "a=1"_str.as_bytes()).unwrap());
    headers.push(Header::make("set-cookie"_str, "b=2"_str.as_bytes()).unwrap());
    auto all = headers.get_all("SET-COOKIE"_str);
    ASSERT_EQ(all.len(), usize(2));
    EXPECT_EQ(all[usize()]->to_str().unwrap(), "a=1"_str);
    EXPECT_EQ(all[usize(1)]->to_str().unwrap(), "b=2"_str);
    EXPECT_TRUE(headers.get_unique("Set-Cookie"_str).unwrap_err().is_Duplicate());
    EXPECT_TRUE(headers.get_unique("missing"_str).unwrap().is_none());
    auto raw = array<u8, 1> { u8(255) };
    headers.push(Header::make("X-Raw"_str, raw.as_slice()).unwrap());
    EXPECT_TRUE(headers.get_unique("x-raw"_str).is_ok());
    EXPECT_TRUE(headers.get_unique_text("x-raw"_str).unwrap_err().is_InvalidText());
}

TEST(Header, RequestFieldsMoveDirectlyIntoResponse) {
    RequestHeadDecoder decoder;
    decoder
        .feed("GET / HTTP/1.1\r\nHost: x\r\nX-Test: first\r\nX-Test: second\r\n\r\n"_str.as_bytes())
        .unwrap();
    auto request = decoder.take().unwrap();
    decoder.reset();
    EXPECT_EQ(request.headers.get_unique_text("HOST"_str).unwrap().unwrap(), "x"_str);
    Response response;
    response.headers = rstd::move(request.headers);
    auto encoded     = encode_response_head(response, "GET"_str, Version::Http11).unwrap();
    auto text        = rstd::str_::from_utf8(encoded.as_slice()).unwrap();
    EXPECT_TRUE(text.contains("X-Test: first\r\nX-Test: second\r\n"_str));
    StreamResponseHead streamed;
    streamed.headers.push(Header::make("Content-Length"_str, "0"_str.as_bytes()).unwrap());
    EXPECT_EQ(validate_response_head(streamed, "GET"_str).unwrap_err(),
              ResponseError::ReservedHeader);
}
