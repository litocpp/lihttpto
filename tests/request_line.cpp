#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

TEST(RequestLine, EverySplitPreservesFieldsAndRemainder) {
    auto line = "POST /api/v1/documents?q=1 HTTP/1.1\r\n"_str;
    for (usize split {}; split <= line.len(); ++split) {
        RequestLineDecoder decoder;
        auto               bytes = line.as_bytes();
        auto first = decoder.feed(slice<u8>::from_raw_parts(bytes.as_raw_ptr(), split));
        ASSERT_TRUE(first.is_ok());
        EXPECT_EQ(first->consumed, split);
        auto rest   = slice<u8>::from_raw_parts(bytes.as_raw_ptr() + split.to_primitive(),
                                                bytes.len() - split);
        auto second = decoder.feed(rest, true);
        ASSERT_TRUE(second.is_ok());
        EXPECT_EQ(second->status, DecodeStatus::Complete);
        EXPECT_EQ(second->consumed, rest.len());
        auto result = decoder.take();
        ASSERT_TRUE(result.is_some());
        EXPECT_EQ(result->method.as_str(), "POST"_str);
        EXPECT_EQ(result->target.as_str(), "/api/v1/documents?q=1"_str);
        EXPECT_EQ(result->version, Version::Http11);
    }
}

TEST(RequestLine, BytewiseAndEmptyFeeds) {
    RequestLineDecoder decoder;
    auto               input = "GET / HTTP/1.0\r\n"_str;
    for (auto byte : input.bytes()) {
        ASSERT_TRUE(decoder.feed({}).is_ok());
        auto fragment = array<u8, 1> { byte };
        auto result   = decoder.feed(fragment.as_slice());
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result->consumed, usize(1));
    }
    EXPECT_EQ(decoder.take()->version, Version::Http10);
}

TEST(RequestLine, StopsBeforeHeadersAndOwnsResult) {
    RequestLineDecoder decoder;
    auto result = decoder.feed("GET /one HTTP/1.1\r\nHost: localhost\r\n"_str.as_bytes());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result->consumed, usize(19));
    auto line = decoder.take();
    ASSERT_TRUE(line.is_some());
    EXPECT_TRUE(decoder.take().is_none());
    EXPECT_TRUE(decoder.feed({}).is_err());
    decoder.reset();
    ASSERT_TRUE(decoder.feed("POST /two HTTP/1.1\r\n"_str.as_bytes()).is_ok());
    EXPECT_EQ(line->target.as_str(), "/one"_str);
    EXPECT_EQ(decoder.take()->target.as_str(), "/two"_str);
}

TEST(RequestLine, RejectsMalformedAndKeepsFailure) {
    auto inputs = array<ref<str>, 7> { "GET / HTTP/1.1\n"_str,    "GET / HTTP/1.1\rx"_str,
                                       "G(ET / HTTP/1.1\r\n"_str, "GET  / HTTP/1.1\r\n"_str,
                                       "GET / HTTP/2.0\r\n"_str,  "GET / HTTP/1.1 extra\r\n"_str,
                                       "GET /\t HTTP/1.1\r\n"_str };
    for (auto input : inputs) {
        RequestLineDecoder decoder;
        auto               result = decoder.feed(input.as_bytes(), true);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(decoder.feed({}).unwrap_err(), result.unwrap_err());
        EXPECT_TRUE(decoder.take().is_none());
    }
}

TEST(RequestLine, EveryTruncatedPrefixFailsAtFinalInput) {
    auto line = "GET / HTTP/1.1\r\n"_str;
    for (usize size {}; size < line.len(); ++size) {
        RequestLineDecoder decoder;
        auto               input  = slice<u8>::from_raw_parts(line.as_bytes().as_raw_ptr(), size);
        auto               result = decoder.feed(input, true);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err(), DecodeError::Truncated);
    }
}

TEST(RequestLine, LimitIncludesTerminator) {
    auto               input = "GET / HTTP/1.1\r\n"_str;
    RequestLineDecoder exact(input.len());
    EXPECT_TRUE(exact.feed(input.as_bytes()).is_ok());
    RequestLineDecoder short_limit(input.len() - usize(1));
    EXPECT_EQ(short_limit.feed(input.as_bytes()).unwrap_err(), DecodeError::TooLarge);
    RequestLineDecoder zero { usize() };
    EXPECT_EQ(zero.feed(input.as_bytes()).unwrap_err(), DecodeError::TooLarge);
}
