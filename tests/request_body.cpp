#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

namespace
{
auto consume(RequestBodyDecoder& decoder, slice<u8> input, Vec<u8>& output)
    -> Result<usize, DecodeError> {
    usize used {};
    while (used < input.len()) {
        auto fragment =
            slice<u8>::from_raw_parts(input.as_raw_ptr() + used.to_primitive(), input.len() - used);
        auto progress = decoder.feed(fragment);
        if (progress.is_err()) return Err(progress.unwrap_err());
        for (auto byte : progress->data) output.push(u8(byte));
        used += progress->consumed;
        if (progress->status == DecodeStatus::Complete) break;
        if (progress->consumed == usize()) return Err(DecodeError::InvalidState);
    }
    return Ok(used);
}
} // namespace

TEST(RequestBody, FixedLengthBorrowsInputAndStopsBeforeNextRequest) {
    auto               input = "dataGET /next HTTP/1.1\r\n"_str;
    RequestBodyDecoder decoder(BodyFraming { BodyKind::FixedLength, u64(4) });
    auto               result = decoder.feed(input.as_bytes());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result->consumed, usize(4));
    EXPECT_EQ(result->data.as_raw_ptr(), input.as_bytes().as_raw_ptr());
    EXPECT_EQ(result->status, DecodeStatus::Complete);
    EXPECT_TRUE(decoder.finish().is_ok());
    EXPECT_EQ(decoder.feed(input.as_bytes())->consumed, usize());
    for (usize split {}; split <= usize(4); ++split) {
        RequestBodyDecoder parts(BodyFraming { BodyKind::FixedLength, u64(4) });
        Vec<u8>            output;
        ASSERT_TRUE(
            consume(parts, slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr(), split), output)
                .is_ok());
        auto rest = slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr() + split.to_primitive(),
                                              input.len() - split);
        EXPECT_EQ(consume(parts, rest, output).unwrap(), usize(4) - split);
        EXPECT_EQ(rstd::str_::from_utf8(output.as_slice()).unwrap(), "data"_str);
        EXPECT_TRUE(parts.finish().is_ok());
    }
}

TEST(RequestBody, ChunkedEverySplitWithExtensionsAndTrailers) {
    auto input = "2;foo=\"a\\\"b\"\r\nhe\r\n3 ; flag\r\nllo\r\n0\r\nX-Sum: ok\r\n\r\n"_str;
    for (usize split {}; split <= input.len(); ++split) {
        RequestBodyDecoder decoder(BodyFraming { BodyKind::Chunked });
        Vec<u8>            output;
        auto               prefix = slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr(), split);
        ASSERT_TRUE(consume(decoder, prefix, output).is_ok());
        auto suffix = slice<u8>::from_raw_parts(
            input.as_bytes().as_raw_ptr() + split.to_primitive(), input.len() - split);
        ASSERT_TRUE(consume(decoder, suffix, output).is_ok());
        EXPECT_TRUE(decoder.finish().is_ok());
        EXPECT_EQ(rstd::str_::from_utf8(output.as_slice()).unwrap(), "hello"_str);
        auto trailers = decoder.take_trailers();
        ASSERT_TRUE(trailers.is_some());
        EXPECT_EQ(trailers->len(), usize(1));
        EXPECT_EQ((*trailers)[usize()].name.as_str(), "X-Sum"_str);
        EXPECT_TRUE(decoder.take_trailers().is_none());
    }
    RequestBodyDecoder decoder(BodyFraming { BodyKind::Chunked });
    Vec<u8>            output;
    auto               used = consume(decoder, "1\r\na\r\n0\r\n\r\nNEXT"_str.as_bytes(), output);
    ASSERT_TRUE(used.is_ok());
    EXPECT_EQ(*used, "1\r\na\r\n0\r\n\r\n"_str.len());
}

TEST(RequestBody, BytewiseAndEmptyBody) {
    RequestBodyDecoder decoder(BodyFraming { BodyKind::Chunked });
    Vec<u8>            output;
    for (auto byte : "1\r\nx\r\n0\r\n\r\n"_str.bytes()) {
        ASSERT_TRUE(decoder.feed({}).is_ok());
        auto one = array<u8, 1> { byte };
        EXPECT_EQ(consume(decoder, one.as_slice(), output).unwrap(), usize(1));
    }
    EXPECT_TRUE(decoder.finish().is_ok());
    EXPECT_EQ(output.len(), usize(1));
    RequestBodyDecoder none(BodyFraming {});
    EXPECT_TRUE(none.finish().is_ok());
    EXPECT_EQ(none.feed("NEXT"_str.as_bytes())->consumed, usize());
    RequestBodyDecoder zero(BodyFraming { BodyKind::FixedLength, u64() });
    EXPECT_TRUE(zero.finish().is_ok());
}

TEST(RequestBody, TruncatedPrefixFailsAndStaysFailed) {
    auto input = "1\r\nx\r\n0\r\nX: y\r\n\r\n"_str;
    for (usize size {}; size < input.len(); ++size) {
        RequestBodyDecoder decoder(BodyFraming { BodyKind::Chunked });
        Vec<u8>            output;
        auto               prefix = slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr(), size);
        ASSERT_TRUE(consume(decoder, prefix, output).is_ok());
        EXPECT_TRUE(decoder.take_trailers().is_none());
        EXPECT_EQ(decoder.finish().unwrap_err(), DecodeError::Truncated);
        EXPECT_EQ(decoder.feed({}).unwrap_err(), DecodeError::Truncated);
    }
    RequestBodyDecoder fixed(BodyFraming { BodyKind::FixedLength, u64(4) });
    ASSERT_TRUE(fixed.feed("abc"_str.as_bytes()).is_ok());
    EXPECT_EQ(fixed.finish().unwrap_err(), DecodeError::Truncated);
}

TEST(RequestBody, RejectsMalformedChunksAndForbiddenTrailers) {
    auto inputs = array<ref<str>, 9> { "g\r\n"_str,
                                       "1\n"_str,
                                       "1\r\nx!\n"_str,
                                       "10000000000000000\r\n"_str,
                                       "1;foo=\"bad\r\n"_str,
                                       "1;=x\r\n"_str,
                                       "0\r\nContent-Length: 1\r\n\r\n"_str,
                                       "0\r\nAuthorization: x\r\n\r\n"_str,
                                       "0\r\n Bad: x\r\n\r\n"_str };
    for (auto input : inputs) {
        RequestBodyDecoder decoder(BodyFraming { BodyKind::Chunked });
        Vec<u8>            output;
        auto               result = consume(decoder, input.as_bytes(), output);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(decoder.finish().unwrap_err(), result.unwrap_err());
        EXPECT_TRUE(decoder.take_trailers().is_none());
    }
}

TEST(RequestBody, DataMetadataAndTrailerLimits) {
    BodyLimits limits;
    limits.data_bytes = u64(2);
    RequestBodyDecoder fixed(BodyFraming { BodyKind::FixedLength, u64(3) }, limits);
    EXPECT_EQ(fixed.feed({}).unwrap_err(), DecodeError::TooLarge);
    RequestBodyDecoder chunked(BodyFraming { BodyKind::Chunked }, limits);
    Vec<u8>            output;
    EXPECT_EQ(
        consume(chunked, "2\r\nab\r\n1\r\nc\r\n0\r\n\r\n"_str.as_bytes(), output).unwrap_err(),
        DecodeError::TooLarge);
    limits                = BodyLimits {};
    limits.metadata_bytes = usize(4);
    RequestBodyDecoder metadata(BodyFraming { BodyKind::Chunked }, limits);
    EXPECT_EQ(consume(metadata, "0\r\n\r\n"_str.as_bytes(), output).unwrap_err(),
              DecodeError::TooLarge);
    limits.metadata_bytes = usize(5);
    RequestBodyDecoder exact(BodyFraming { BodyKind::Chunked }, limits);
    EXPECT_TRUE(consume(exact, "0\r\n\r\n"_str.as_bytes(), output).is_ok());
    EXPECT_TRUE(exact.finish().is_ok());
    limits                = BodyLimits {};
    limits.trailer_fields = usize();
    RequestBodyDecoder fields(BodyFraming { BodyKind::Chunked }, limits);
    EXPECT_EQ(consume(fields, "0\r\nX: y\r\n\r\n"_str.as_bytes(), output).unwrap_err(),
              DecodeError::TooLarge);
}

TEST(RequestBody, UsesDecodedHeadAndPreservesPipelinedRequest) {
    auto request =
        "POST /articles HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nmdx\r\n0\r\n\r\nGET /next HTTP/1.1\r\nHost: x\r\n\r\n"_str;
    RequestHeadDecoder heads;
    auto               head_progress = heads.feed(request.as_bytes());
    ASSERT_TRUE(head_progress.is_ok());
    auto head = heads.take();
    ASSERT_TRUE(head.is_some());
    RequestBodyDecoder body(head->body);
    Vec<u8>            output;
    auto remaining = slice<u8>::from_raw_parts(request.as_bytes().as_raw_ptr() +
                                                   head_progress->consumed.to_primitive(),
                                               request.len() - head_progress->consumed);
    auto used      = consume(body, remaining, output);
    ASSERT_TRUE(used.is_ok());
    ASSERT_TRUE(body.finish().is_ok());
    EXPECT_EQ(rstd::str_::from_utf8(output.as_slice()).unwrap(), "mdx"_str);
    heads.reset();
    auto next = slice<u8>::from_raw_parts(remaining.as_raw_ptr() + used->to_primitive(),
                                          remaining.len() - *used);
    ASSERT_TRUE(heads.feed(next, true).is_ok());
    auto next_head = heads.take();
    ASSERT_TRUE(next_head.is_some());
    EXPECT_EQ(next_head->line.target.as_str(), "/next"_str);
    EXPECT_EQ(next_head->body.kind, BodyKind::None);
}
