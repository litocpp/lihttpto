#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

TEST(Header, TokenListParsingUsesHttpGrammar) {
    auto tokens = HeaderValue::make(" , one,\tTwo ,, three, "_str.as_bytes()).unwrap().tokens();
    ASSERT_TRUE(tokens.is_ok());
    ASSERT_TRUE(tokens->len() == usize(3));
    EXPECT_TRUE((*tokens)[usize(0)].as_str() == "one"_str);
    EXPECT_TRUE((*tokens)[usize(1)].as_str() == "Two"_str);
    for (auto invalid : array<ref<str>, 4> { ""_str, ", ,"_str, "one two"_str, "one, \"two\""_str })
        EXPECT_TRUE(HeaderValue::make(invalid.as_bytes()).unwrap().tokens().is_err());
}

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

TEST(Header, ReplacementIsAtomicAndCloneOwnsFields) {
    Headers headers;
    headers.add("X-Test"_str, "first"_str).unwrap();
    headers.add("x-test"_str, "second"_str).unwrap();
    auto copy = headers.clone();
    EXPECT_TRUE(headers.set("X-Test"_str, "bad\r\nvalue"_str).is_err());
    EXPECT_EQ(headers.get_all("x-test"_str).len(), usize(2));
    headers.set("x-test"_str, "replacement"_str).unwrap();
    EXPECT_EQ(headers.get_unique_text("X-Test"_str).unwrap().unwrap(), "replacement"_str);
    EXPECT_EQ(copy.get_all("x-test"_str).len(), usize(2));
    EXPECT_EQ(copy.remove("X-Test"_str), usize(2));
    EXPECT_TRUE(copy.is_empty());
    EXPECT_TRUE(headers.contains("X-Test"_str));
}

TEST(Header, TransportMetadataDoesNotRelaxHttp1WireParsing) {
    for (auto line : array<ref<str>, 2> { "HTTP/2 200\r\n\r\n"_str, "HTTP/3 204\r\n\r\n"_str }) {
        Http1HeadParser wire;
        EXPECT_TRUE(wire.push(line.as_bytes()).is_err());
        Http1HeadParser metadata { true };
        auto            parsed = metadata.push(line.as_bytes()).unwrap();
        EXPECT_TRUE(parsed.is_Complete());
        EXPECT_TRUE(parsed.as_Complete().head.status_code().is_some());
    }
}

TEST(Header, ResponseHeadKeepsFieldsAndTrailersSeparate) {
    Http1HeadParser decoder;
    EXPECT_TRUE(decoder.push("HTTP/1.1 200 OK\r\nSet-Coo"_bytes).unwrap().is_NeedMore());
    auto result = decoder.push("kie: a=1\r\nset-cookie: b=2\r\n\r\nbody"_bytes).unwrap();
    ASSERT_TRUE(result.is_Complete());
    auto& head = result.as_Complete().head;
    EXPECT_EQ(head.status_code().unwrap(), u16(200));
    EXPECT_EQ(head.headers().get_all("Set-Cookie"_str).len(), usize(2));
    Http1FieldSectionParser trailers;
    auto                    trailer = trailers.push("X-End: ok\r\n\r\n"_bytes).unwrap();
    EXPECT_TRUE(trailer.is_Complete());
    EXPECT_FALSE(head.headers().contains("X-End"_str));
    auto response = rstd::move(head).into_response().unwrap();
    EXPECT_EQ(response.status.value(), u16(200));
    EXPECT_EQ(response.headers.get_all("Set-Cookie"_str).len(), usize(2));
    EXPECT_EQ(response.version.unwrap().major(), u8(1));
}

TEST(Header, FailedAndFinishedDecodersCannotResume) {
    Http1HeadParser invalid;
    EXPECT_TRUE(invalid.push("bad\r\n"_bytes).is_err());
    EXPECT_TRUE(invalid.push("HTTP/1.1 200 OK\r\n\r\n"_bytes).is_err());
    Http1HeadParser truncated;
    EXPECT_TRUE(truncated.push("HTTP/1.1 200"_bytes).unwrap().is_NeedMore());
    EXPECT_TRUE(truncated.finish().is_err());
    EXPECT_TRUE(truncated.push(" OK\r\n\r\n"_bytes).is_err());
    Http1FieldSectionParser fields;
    EXPECT_TRUE(fields.push("bad field\r\n"_bytes).is_err());
    EXPECT_TRUE(fields.push("X: good\r\n\r\n"_bytes).is_err());
}
