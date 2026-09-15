#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

TEST(RequestHead, EverySplitPreservesBodyAndFraming) {
    auto head =
        "POST / HTTP/1.1\r\nHost: example.test\r\ncOnTeNt-LeNgTh: 4\r\nX-A: one\r\nX-A: two\r\n\r\n"_str;
    for (usize split {}; split <= head.len(); ++split) {
        RequestHeadDecoder decoder;
        auto first = decoder.feed(slice<u8>::from_raw_parts(head.as_bytes().as_raw_ptr(), split));
        ASSERT_TRUE(first.is_ok());
        EXPECT_EQ(first->consumed, split);
        auto rest   = slice<u8>::from_raw_parts(head.as_bytes().as_raw_ptr() + split.to_primitive(),
                                                head.len() - split);
        auto second = decoder.feed(rest, true);
        ASSERT_TRUE(second.is_ok());
        EXPECT_EQ(second->status, DecodeStatus::Complete);
        auto body = decoder.feed("dataNEXT"_str.as_bytes());
        ASSERT_TRUE(body.is_ok());
        EXPECT_EQ(body->consumed, usize());
        auto result = decoder.take();
        ASSERT_TRUE(result.is_some());
        EXPECT_EQ(result->line.method.as_str(), "POST"_str);
        EXPECT_EQ(result->body.kind, BodyKind::FixedLength);
        EXPECT_EQ(result->body.length, u64(4));
        EXPECT_TRUE(result->keep_alive);
        EXPECT_EQ(result->headers.len(), usize(4));
        EXPECT_EQ(result->headers[usize(2)].name.as_str(), "X-A"_str);
        EXPECT_EQ(result->headers[usize(3)].name.as_str(), "X-A"_str);
    }
    RequestHeadDecoder decoder;
    auto result = decoder.feed("GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY"_str.as_bytes(), true);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result->consumed, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"_str.len());
}

TEST(RequestHead, BytewiseAndResultOwnership) {
    auto               input = "GET /first HTTP/1.1\r\nHost: x\r\nX: \tvalue \t\r\n\r\n"_str;
    RequestHeadDecoder decoder;
    for (auto byte : input.bytes()) {
        auto fragment = array<u8, 1> { byte };
        ASSERT_TRUE(decoder.feed({}).is_ok());
        auto progress = decoder.feed(fragment.as_slice());
        ASSERT_TRUE(progress.is_ok());
        EXPECT_EQ(progress->consumed, usize(1));
    }
    auto first = decoder.take();
    ASSERT_TRUE(first.is_some());
    EXPECT_TRUE(decoder.take().is_none());
    EXPECT_TRUE(decoder.feed({}).is_err());
    decoder.reset();
    ASSERT_TRUE(decoder.feed("GET /second HTTP/1.0\r\n\r\n"_str.as_bytes()).is_ok());
    EXPECT_EQ(first->line.target.as_str(), "/first"_str);
    auto value = rstd::str_::from_utf8(first->headers[usize(1)].value.as_slice());
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.unwrap(), "value"_str);
    EXPECT_EQ(decoder.take()->line.target.as_str(), "/second"_str);
}

TEST(RequestHead, VersionConnectionAndTransferRules) {
    auto inputs = array<ref<str>, 5> {
        "GET / HTTP/1.0\r\n\r\n"_str,
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"_str,
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade, CLOSE\r\nConnection: keep-alive\r\n\r\n"_str,
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ChUnKeD\r\n\r\n"_str,
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551615\r\n\r\n"_str
    };
    for (usize index {}; index < inputs.len(); ++index) {
        RequestHeadDecoder decoder;
        auto               progress = decoder.feed(inputs[index].as_bytes(), true);
        ASSERT_TRUE(progress.is_ok());
        auto head = decoder.take();
        ASSERT_TRUE(head.is_some());
        EXPECT_EQ(head->keep_alive, index != usize(0) && index != usize(2));
        if (index == usize(3))
            EXPECT_EQ(head->body.kind, BodyKind::Chunked);
        else if (index == usize(4))
            EXPECT_EQ(head->body.length, u64::MAX);
        else
            EXPECT_EQ(head->body.kind, BodyKind::None);
    }
}

TEST(RequestHead, RejectsAmbiguousAndMalformedFields) {
    auto headers = array<ref<str>, 16> {
        "Host: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n"_str,
        "Host: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n"_str,
        "Host: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n"_str,
        "Host: x\r\nContent-Length: -1\r\n"_str,
        "Host: x\r\nContent-Length: 18446744073709551616\r\n"_str,
        "Host: x\r\nContent-Length: 1, 1\r\n"_str,
        "Host: x\r\nContent-Length:\r\n"_str,
        "Host: x\r\nTransfer-Encoding: gzip, chunked\r\n"_str,
        "Host: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n"_str,
        "Host: x\r\nHost: x\r\n"_str,
        "Host:\r\n"_str,
        "Host: x y\r\n"_str,
        "Host: x\r\nName : value\r\n"_str,
        "Host: x\r\n folded\r\n"_str,
        "Host: x\r\nConnection: close keep-alive\r\n"_str,
        "X: missing-host\r\n"_str
    };
    for (auto fields : headers) {
        RequestHeadDecoder decoder;
        ASSERT_TRUE(decoder.feed("GET / HTTP/1.1\r\n"_str.as_bytes()).is_ok());
        auto partial = decoder.feed(fields.as_bytes());
        auto result =
            partial.is_err() ? rstd::move(partial) : decoder.feed("\r\n"_str.as_bytes(), true);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(decoder.feed({}).unwrap_err(), result.unwrap_err());
        EXPECT_TRUE(decoder.take().is_none());
        decoder.reset();
        EXPECT_TRUE(decoder.feed("GET / HTTP/1.0\r\n\r\n"_str.as_bytes(), true).is_ok());
    }
    RequestHeadDecoder old;
    EXPECT_TRUE(
        old.feed("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"_str.as_bytes(), true)
            .is_err());
}

TEST(RequestHead, PreservesOpaqueBytesButRejectsControls) {
    RequestHeadDecoder decoder;
    ASSERT_TRUE(decoder.feed("GET / HTTP/1.1\r\nHost: x\r\nX: "_str.as_bytes()).is_ok());
    auto opaque = array<u8, 2> { u8(255), u8(128) };
    ASSERT_TRUE(decoder.feed(opaque.as_slice()).is_ok());
    ASSERT_TRUE(decoder.feed("\r\n\r\n"_str.as_bytes(), true).is_ok());
    auto head = decoder.take();
    ASSERT_TRUE(head.is_some());
    EXPECT_EQ(head->headers[usize(1)].value[usize()], u8(255));
    EXPECT_EQ(head->headers[usize(1)].value[usize(1)], u8(128));
    for (auto byte : array<u8, 3> { u8(0), u8(31), u8(127) }) {
        RequestHeadDecoder invalid;
        ASSERT_TRUE(invalid.feed("GET / HTTP/1.1\r\nHost: x\r\nX: "_str.as_bytes()).is_ok());
        auto fragment = array<u8, 1> { byte };
        ASSERT_TRUE(invalid.feed(fragment.as_slice()).is_ok());
        EXPECT_TRUE(invalid.feed("\r\n\r\n"_str.as_bytes(), true).is_err());
    }
}

TEST(RequestHead, LimitsAndTruncatedPrefixes) {
    auto input = "GET / HTTP/1.1\r\nHost: x\r\n\r\n"_str;
    for (usize size {}; size < input.len(); ++size) {
        RequestHeadDecoder decoder;
        auto               result =
            decoder.feed(slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr(), size), true);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err(), DecodeError::Truncated);
    }
    HeadLimits limits;
    limits.total_bytes = input.len();
    RequestHeadDecoder exact(limits);
    EXPECT_TRUE(exact.feed(input.as_bytes(), true).is_ok());
    limits.total_bytes = input.len() - usize(1);
    RequestHeadDecoder total(limits);
    EXPECT_EQ(total.feed(input.as_bytes()).unwrap_err(), DecodeError::TooLarge);
    limits        = HeadLimits {};
    limits.fields = usize();
    RequestHeadDecoder count(limits);
    EXPECT_EQ(count.feed(input.as_bytes()).unwrap_err(), DecodeError::TooLarge);
    limits             = HeadLimits {};
    limits.header_line = usize(8);
    RequestHeadDecoder line(limits);
    EXPECT_EQ(line.feed(input.as_bytes()).unwrap_err(), DecodeError::TooLarge);
}
