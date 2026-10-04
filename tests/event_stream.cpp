#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

static auto consume(EventStreamDecoder& decoder, slice<u8> input, Vec<ServerEvent>& events)
    -> bool {
    usize offset {};
    while (offset < input.len()) {
        auto part   = slice<u8>::from_raw_parts(input.as_raw_ptr() + offset.to_primitive(),
                                                input.len() - offset);
        auto result = decoder.feed(part);
        if (result.is_err() || result->consumed == usize(0)) return false;
        offset += result->consumed;
        if (result->event.is_some()) events.push(rstd::move(result->event).unwrap());
    }
    return true;
}

TEST(EventStream, EverySplitAndBytewise) {
    auto input =
        "\xEF\xBB\xBF:keepalive\r\nid: 7\revent: delta\ndata: \xE4\xB8\xAD\r\ndata: two\r\n\r\ndata:\n\ndata: last\n\n"_str;
    for (usize split {}; split <= input.len(); ++split) {
        EventStreamDecoder decoder;
        Vec<ServerEvent>   events;
        ASSERT_TRUE(consume(
            decoder, slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr(), split), events));
        ASSERT_TRUE(
            consume(decoder,
                    slice<u8>::from_raw_parts(input.as_bytes().as_raw_ptr() + split.to_primitive(),
                                              input.len() - split),
                    events));
        ASSERT_TRUE(decoder.finish().is_ok());
        ASSERT_EQ(events.len(), usize(3));
        EXPECT_EQ(events[usize(0)].data.as_str(), "\xE4\xB8\xAD\ntwo"_str);
        EXPECT_EQ(events[usize(0)].type.as_str(), "delta"_str);
        EXPECT_EQ(events[usize(1)].type.as_str(), "message"_str);
        EXPECT_TRUE(events[usize(1)].data.is_empty());
        EXPECT_EQ(events[usize(2)].id.as_str(), "7"_str);
    }
    EventStreamDecoder decoder;
    Vec<ServerEvent>   events;
    for (auto byte : input.bytes()) {
        auto fragment = array<u8, 1> { byte };
        ASSERT_TRUE(consume(decoder, fragment.as_slice(), events));
    }
    EXPECT_EQ(events.len(), usize(3));
}

TEST(EventStream, FieldsAndEof) {
    EventStreamDecoder decoder;
    Vec<ServerEvent>   events;
    ASSERT_TRUE(consume(
        decoder,
        "id: old\nretry: 1200\nevent: discarded\n\nid: bad\0id\nretry: -1\nData: ignored\ndata:  space\n\nid\ndata: ok\n\ndata: unfinished\n"_str
            .as_bytes(),
        events));
    ASSERT_EQ(events.len(), usize(2));
    EXPECT_EQ(events[usize(0)].data.as_str(), " space"_str);
    EXPECT_EQ(events[usize(0)].id.as_str(), "old"_str);
    EXPECT_EQ(events[usize(0)].type.as_str(), "message"_str);
    EXPECT_TRUE(events[usize(1)].id.is_empty());
    EXPECT_EQ(decoder.retry_ms().unwrap(), u64(1200));
    EXPECT_TRUE(decoder.finish().is_ok());
    EXPECT_TRUE(decoder.feed({}).is_err());
}

TEST(EventStream, LimitsAndInvalidUtf8) {
    EventStreamDecoder line({ usize(3), usize(100) });
    EXPECT_EQ(line.feed("abcd"_str.as_bytes()).unwrap_err(), EventStreamError::LimitExceeded);
    EXPECT_EQ(line.feed({}).unwrap_err(), EventStreamError::Closed);
    EventStreamDecoder data({ usize(100), usize(4) });
    EXPECT_TRUE(data.feed("data:a\n"_str.as_bytes()).is_ok());
    EXPECT_EQ(data.feed("data:bc\n"_str.as_bytes()).unwrap_err(), EventStreamError::LimitExceeded);
    EventStreamDecoder invalid;
    auto               bad = array<u8, 2> { u8(0xff), u8('\n') };
    EXPECT_EQ(invalid.feed(bad.as_slice()).unwrap_err(), EventStreamError::InvalidUtf8);
    EventStreamDecoder incomplete;
    auto               prefix = array<u8, 1> { u8(0xe4) };
    EXPECT_TRUE(incomplete.feed(prefix.as_slice()).is_ok());
    EXPECT_EQ(incomplete.finish().unwrap_err(), EventStreamError::InvalidUtf8);
    EventStreamDecoder retry;
    EXPECT_EQ(retry.feed("retry: 18446744073709551616\n"_str.as_bytes()).unwrap_err(),
              EventStreamError::LimitExceeded);
}
