#include <rstd/test/gtest.hpp>
import lihttpto.cgi;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;
using Bytes = rstd::bytes::Bytes;

TEST(Cgi, HeadSupportsEverySplitAndPreservesBodyBoundary) {
    auto header =
        "Status: 403 Forbidden\r\nContent-Type: text/plain\r\nCache-Control: no-cache\r\n\r\n"_str;
    auto wire = rstd::format("{}body", header);
    for (usize split {}; split <= header.len(); ++split) {
        lihttpto::cgi::HeaderDecoder decoder;
        auto                         first = wire.as_str().get(usize(), split).unwrap();
        EXPECT_EQ(decoder.feed(first.as_bytes()).unwrap(), split);
        auto rest = wire.as_str().get(split, wire.len()).unwrap();
        EXPECT_EQ(decoder.feed(rest.as_bytes()).unwrap(), header.len() - split);
        ASSERT_TRUE(decoder.complete());
        auto parsed = decoder.take().unwrap();
        EXPECT_EQ(parsed.status, u16(403));
        EXPECT_EQ(parsed.headers.len(), usize(2));
    }
    for (auto wire : array<ref<str>, 4> { "Status: 200\nStatus: 404\n\n"_str,
                                          "Bad Header: x\n\n"_str,
                                          "Content-Length: 5\n\n"_str,
                                          "Status: bad\n\n"_str }) {
        lihttpto::cgi::HeaderDecoder decoder;
        EXPECT_TRUE(decoder.feed(wire.as_bytes()).is_err());
    }
}

TEST(Cgi, DecoderCompletionAndUnsupportedFraming) {
    cgi::HeaderDecoder early;
    EXPECT_TRUE(early.take().unwrap_err().is_State());
    early.feed("Status: 200\n"_str.as_bytes()).unwrap();
    EXPECT_TRUE(early.finish().unwrap_err().is_Truncated());
    EXPECT_TRUE(early.feed("\n"_str.as_bytes()).unwrap_err().is_State());
    for (auto wire :
         array<ref<str>, 2> { "Location: /next\n\n"_str, "Content-Length: 0\n\n"_str }) {
        cgi::HeaderDecoder decoder;
        EXPECT_TRUE(decoder.feed(wire.as_bytes()).unwrap_err().is_Unsupported());
        EXPECT_TRUE(decoder.feed("\n"_str.as_bytes()).unwrap_err().is_State());
    }
    cgi::HeaderDecoder complete;
    complete.feed("Content-Type: text/plain\n\n"_str.as_bytes()).unwrap();
    EXPECT_TRUE(complete.finish().is_ok());
    EXPECT_EQ(complete.take().unwrap().status, u16(200));
    EXPECT_TRUE(complete.take().unwrap_err().is_State());
}

TEST(Cgi, RejectsInvalidStatusDuplicateTypeAndOversizedHead) {
    for (auto wire : array<ref<str>, 5> { "Status: 199 No\n\n"_str,
                                          "Status: 600 No\n\n"_str,
                                          "Status: 200x\n\n"_str,
                                          "Content-Type: a\ncontent-type: b\n\n"_str,
                                          "Transfer-Encoding: chunked\n\n"_str }) {
        cgi::HeaderDecoder decoder;
        EXPECT_TRUE(decoder.feed(wire.as_bytes()).unwrap_err().is_Invalid());
    }
    Vec<u8> large;
    for (usize i {}; i < usize(65537); ++i) large.push(u8('x'));
    cgi::HeaderDecoder decoder;
    EXPECT_TRUE(decoder.feed(large.as_slice()).unwrap_err().is_TooLarge());
}

struct Chunks {
    using Error = i32;
    Vec<Bytes> chunks;
    usize      index {};
    bool       fail {};
    auto       next() -> rstd::async::coro<Result<Option<Bytes>, Error>> {
        if (index < chunks.len()) co_return Ok(Some(rstd::move(chunks[index++])));
        if (fail) co_return Err(i32(7));
        co_return Ok<Option<Bytes>>(None());
    }
};
TEST(Cgi, SourceReusesBoundaryBytesAndReportsTruncatedHead) {
    auto   runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    Chunks chunks;
    chunks.chunks.push(Bytes::copy_from_slice("Content-Type: text/plain\r\n\r"_str.as_bytes()));
    chunks.chunks.push(Bytes::copy_from_slice("\nfirst"_str.as_bytes()));
    chunks.chunks.push(Bytes::copy_from_slice("second"_str.as_bytes()));
    cgi::ResponseSource source(chunks);
    EXPECT_TRUE(runtime.block_on(source.next()).unwrap_err().is_Decode());
    EXPECT_EQ(runtime.block_on(source.read_head()).unwrap().status, u16(200));
    EXPECT_TRUE(runtime.block_on(source.read_head()).unwrap_err().is_Decode());
    auto first = runtime.block_on(source.next()).unwrap().unwrap();
    EXPECT_EQ(rstd::str_::from_utf8(first.as_slice()).unwrap(), "first"_str);
    EXPECT_EQ(chunks.index, usize(2));
    auto second = runtime.block_on(source.next()).unwrap().unwrap();
    EXPECT_EQ(rstd::str_::from_utf8(second.as_slice()).unwrap(), "second"_str);
    EXPECT_TRUE(runtime.block_on(source.next()).unwrap().is_none());
    EXPECT_TRUE(runtime.block_on(source.next()).unwrap().is_none());
    Chunks              empty;
    cgi::ResponseSource truncated(empty);
    auto                error = runtime.block_on(truncated.read_head()).unwrap_err();
    EXPECT_TRUE(error.as_Decode().error.is_Truncated());
    empty.fail = true;
    cgi::ResponseSource failed(empty);
    EXPECT_TRUE(runtime.block_on(failed.read_head()).unwrap_err().is_Source());
}
