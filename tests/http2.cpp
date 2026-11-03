#include <rstd/test/gtest.hpp>
import rstd;
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
namespace h2 = lihttpto::h2;
auto h2_hex(ref<str> text) -> Bytes {
    Vec<u8> bytes;
    auto    input = text.as_bytes();
    auto    digit = [](u8 b) -> unsigned {
        auto n = b.to_primitive();
        return n >= 'a' ? n - 'a' + 10 : n - '0';
    };
    for (usize i; i < input.len(); i += usize(2))
        bytes.push(u8((digit(input[i]) << 4) | digit(input[i + usize(1)])));
    return Bytes::from_vec(rstd::move(bytes));
}
auto h2_request() -> h2::Request {
    lihttpto::RequestParts parts {
        lihttpto::Url::parse_http("https://example.com/test?q=1"_str).unwrap(),
        lihttpto::Method::parse("GET"_str).unwrap(),
        {}
    };
    return lihttpto::PreparedRequest::make(rstd::move(parts)).unwrap().http2_request().unwrap();
}
auto h2_headers(u32 stream, ref<str> block = "88"_str, unsigned flags = 4) -> Bytes {
    return h2::frame(u8(1), u8(flags), stream, h2_hex(block));
}
TEST(Hpack, RfcHuffmanDynamicExamplesShareOneTable) {
    h2::HpackDecoder decoder;
    auto first = decoder.decode(h2_hex("828684418cf1e3c2e5f23a6ba0ab90f4ff"_str)).unwrap();
    EXPECT_EQ(first.len(), usize(4));
    EXPECT_EQ(first[usize(3)].name.as_str(), ":authority"_str);
    EXPECT_EQ(first[usize(3)].value.as_slice(), "www.example.com"_bytes);
    auto second = decoder.decode(h2_hex("828684be5886a8eb10649cbf"_str)).unwrap();
    EXPECT_EQ(second[usize(3)].value.as_slice(), "www.example.com"_bytes);
    EXPECT_EQ(second[usize(4)].value.as_slice(), "no-cache"_bytes);
    auto third =
        decoder.decode(h2_hex("828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf"_str)).unwrap();
    EXPECT_EQ(third[usize(4)].name.as_str(), "custom-key"_str);
    EXPECT_EQ(third[usize(4)].value.as_slice(), "custom-value"_bytes);
}
TEST(Hpack, RejectsEosPaddingIndexesTableUpdatesAndExpansion) {
    for (auto text : array<ref<str>, 7> { "00"_str,
                                          "80"_str,
                                          "be"_str,
                                          "828020"_str,
                                          "0081ff00"_str,
                                          "00810000"_str,
                                          "3fe21f"_str }) {
        h2::HpackDecoder decoder;
        EXPECT_TRUE(decoder.decode(h2_hex(text)).is_err());
    }
    h2::HpackDecoder decoder;
    EXPECT_TRUE(
        decoder.decode(h2_hex("828684418cf1e3c2e5f23a6ba0ab90f4ff"_str), usize(16)).is_err());
    auto fields  = h2_request().fields;
    auto encoded = h2::encode_fields(fields.as_slice()).unwrap();
    auto decoded = decoder.decode(rstd::move(encoded)).unwrap();
    EXPECT_EQ(decoded.len(), fields.len());
    for (usize i; i < fields.len(); ++i) {
        EXPECT_EQ(decoded[i].name, fields[i].name);
        EXPECT_EQ(decoded[i].value.as_slice(), fields[i].value.as_slice());
    }
}
TEST(Http2, IncrementalFramesRequireSettingsAndExclusiveContinuation) {
    h2::FrameDecoder decoder;
    auto             bytes = h2_headers(u32(1));
    EXPECT_TRUE(decoder.feed(bytes.slice(usize(), usize(5)))->is_empty());
    EXPECT_EQ(decoder.feed(bytes.slice(usize(5), bytes.len()))->len(), usize(1));
    h2::ClientConnection invalid;
    EXPECT_TRUE(invalid.feed(h2_headers(u32(1))).is_err());
    h2::ClientConnection connection;
    EXPECT_TRUE(connection.feed(h2::frame(u8(4), u8(), u32(), {})).is_ok());
    auto id = connection.open(h2_request()).unwrap();
    EXPECT_TRUE(connection.feed(h2_headers(id, ""_str, 0)).is_ok());
    EXPECT_TRUE(
        connection.feed(h2::frame(u8(6), u8(), u32(), Bytes::copy_from_slice("12345678"_bytes)))
            .is_err());
}
TEST(Http2, StreamsPreserveHeadersDataTrailersAndLength) {
    h2::ClientConnection connection;
    connection.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    auto one = connection.open(h2_request()).unwrap(), two = connection.open(h2_request()).unwrap();
    EXPECT_EQ(one, u32(1));
    EXPECT_EQ(two, u32(3));
    connection.feed(h2_headers(one, "880f0d0133"_str)).unwrap();
    connection.feed(h2_headers(two)).unwrap();
    connection.feed(h2::frame(u8(), u8(), one, Bytes::copy_from_slice("abc"_bytes))).unwrap();
    connection.feed(h2::frame(u8(), u8(1), two, {})).unwrap();
    connection.feed(h2_headers(one, "0001780179"_str, 5)).unwrap();
    auto events = connection.take_events();
    EXPECT_EQ(events.len(), usize(6));
    EXPECT_TRUE(events[usize()].is_Head());
    EXPECT_EQ(events[usize()].as_Head().head.version->major(), u8(2));
    EXPECT_TRUE(events[usize(2)].is_Data());
    EXPECT_TRUE(events[usize(3)].is_End());
    EXPECT_TRUE(events[usize(4)].is_Trailers());
}
TEST(Http2, MalformedResponseResetsOnlyItsStream) {
    for (auto block : array<ref<str>, 8> { "890f0d0130"_str,
                                           "880002746508747261696c657273"_str,
                                           "8800017803206120"_str,
                                           "880001580179"_str,
                                           "8288"_str,
                                           "8888"_str,
                                           "88000a436f6e6e656374696f6e05636c6f7365"_str,
                                           "880f0d0131"_str }) {
        h2::ClientConnection c;
        c.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
        auto id     = c.open(h2_request()).unwrap();
        auto other  = c.open(h2_request()).unwrap();
        auto result = c.feed(h2_headers(id, block, 5));
        EXPECT_TRUE(result.is_ok());
        auto rejected = c.take_events();
        bool reset    = false;
        for (const auto& event : rejected)
            if (event.is_Reset()) reset = true;
        EXPECT_TRUE(reset);
        c.feed(h2_headers(other, "88"_str, 5)).unwrap();
        auto events = c.take_events();
        bool ended  = false;
        for (const auto& e : events)
            if (e.is_End()) ended = true;
        EXPECT_TRUE(ended);
    }
}
TEST(Http2, WindowsSettingsResetAndGoawayRemainBounded) {
    h2::Limits limits;
    limits.receive_window  = usize(16);
    limits.maximum_streams = usize(2);
    auto c                 = h2::ClientConnection::make(limits).unwrap();
    c.feed(h2::frame(u8(4), u8(), u32(), h2_hex("000400000000"_str))).unwrap();
    auto id = c.open(h2_request()).unwrap();
    EXPECT_EQ(c.writable(id), usize());
    c.feed(h2::frame(u8(8), u8(), id, h2_hex("0000000a"_str))).unwrap();
    EXPECT_EQ(c.writable(id), usize(10));
    c.send(id, Bytes::copy_from_slice("abc"_bytes)).unwrap();
    EXPECT_EQ(c.writable(id), usize(7));
    c.feed(h2_headers(id)).unwrap();
    c.feed(h2::frame(u8(), u8(), id, Bytes::copy_from_slice("12345678901234567"_bytes))).unwrap();
    auto events = c.take_events();
    EXPECT_TRUE(events[events.len() - usize(1)].is_Reset());
    auto second = c.open(h2_request()).unwrap();
    c.feed(h2::frame(u8(7), u8(), u32(), h2_hex("0000000100000000"_str))).unwrap();
    EXPECT_FALSE(c.accepting());
    events = c.take_events();
    EXPECT_TRUE(events[usize()].is_Goaway());
    EXPECT_EQ(events[usize(1)].as_Reset().stream, second);
    EXPECT_TRUE(c.feed(h2::frame(u8(8), u8(), u32(), h2_hex("00000000"_str))).is_err());
}

TEST(Http2, ServerCanDisablePushButCannotEnableIt) {
    h2::ClientConnection allowed;
    EXPECT_TRUE(allowed.feed(h2::frame(u8(4), u8(), u32(), h2_hex("000200000000"_str))).is_ok());
    h2::ClientConnection rejected;
    EXPECT_TRUE(rejected.feed(h2::frame(u8(4), u8(), u32(), h2_hex("000200000001"_str))).is_err());
}

TEST(Http2, LimitsAndResetPreserveQueuedFraming) {
    h2::Limits invalid;
    invalid.receive_window = usize(0x80000000);
    EXPECT_TRUE(h2::ClientConnection::make(invalid).is_err());
    h2::ClientConnection c;
    c.take_output();
    c.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    c.take_output();
    auto id = c.open(h2_request()).unwrap();
    c.send(id, Bytes::copy_from_slice("abc"_bytes)).unwrap();
    c.reset(id);
    auto             output = c.take_output();
    h2::FrameDecoder decoder;
    Vec<h2::Frame>   frames;
    for (auto& item : output) {
        auto next = decoder.feed(rstd::move(item.bytes)).unwrap();
        for (auto& value : next) frames.push(rstd::move(value));
    }
    EXPECT_EQ(frames.len(), usize(3));
    EXPECT_EQ(frames[usize()].type, u8(1));
    EXPECT_EQ(frames[usize(1)].type, u8());
    EXPECT_EQ(frames[usize(2)].type, u8(3));
}

TEST(Http2, ContinuationAndNegativeStreamWindows) {
    h2::ClientConnection c;
    c.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    auto id = c.open(h2_request()).unwrap();
    c.send(id, Bytes::copy_from_slice("12345678"_bytes)).unwrap();
    c.feed(h2::frame(u8(4), u8(), u32(), h2_hex("000400000000"_str))).unwrap();
    EXPECT_EQ(c.writable(id), usize());
    c.feed(h2::frame(u8(8), u8(), id, h2_hex("00000008"_str))).unwrap();
    EXPECT_EQ(c.writable(id), usize());
    c.feed(h2::frame(u8(8), u8(), id, h2_hex("00000001"_str))).unwrap();
    EXPECT_EQ(c.writable(id), usize(1));
    c.feed(h2_headers(id, "88"_str, 1)).unwrap();
    EXPECT_TRUE(c.take_events().is_empty());
    c.feed(h2::frame(u8(9), u8(4), id, {})).unwrap();
    auto events = c.take_events();
    EXPECT_EQ(events.len(), usize(2));
    EXPECT_TRUE(events[usize()].is_Head());
    EXPECT_TRUE(events[usize(1)].is_End());
}
TEST(Http2, RequestPreparationRejectsConnectionFieldsAndWhitespace) {
    for (auto name : array<ref<str>, 2> { "connection"_str, "x-field"_str }) {
        lihttpto::RequestParts parts {
            lihttpto::Url::parse_http("https://example.com/"_str).unwrap(),
            lihttpto::Method::parse("GET"_str).unwrap(),
            {}
        };
        parts.headers.add(name, name == "connection"_str ? "close"_str : " value "_str).unwrap();
        auto prepared = lihttpto::PreparedRequest::make(rstd::move(parts)).unwrap();
        EXPECT_TRUE(prepared.http2_request().is_err());
    }
}

TEST(Http2, LocalShutdownLeavesGoawayAsTheLastFrame) {
    h2::ClientConnection c;
    c.take_output();
    c.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    c.take_output();
    auto id = c.open(h2_request()).unwrap();
    c.shutdown(u32(1));
    c.reset(id);
    c.consume(id, usize(1));
    auto output = c.take_output();
    EXPECT_EQ(output.len(), usize(2));
    h2::FrameDecoder decoder;
    auto             frames = decoder.feed(rstd::move(output[usize(1)].bytes)).unwrap();
    EXPECT_EQ(frames[usize()].type, u8(7));
    EXPECT_TRUE(c.send(id, {}).is_err());
    c.shutdown();
    EXPECT_TRUE(c.take_output().is_empty());
}
TEST(Http2, TypedOutputAttributesEachDataPayloadAndExcludesControlFrames) {
    auto connection = h2::ClientConnection::make({}).unwrap();
    connection.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    auto first  = connection.open(h2_request()).unwrap();
    auto second = connection.open(h2_request()).unwrap();
    connection.send(first, Bytes::copy_from_slice("abc"_bytes)).unwrap();
    connection.send(second, Bytes::copy_from_slice("defgh"_bytes), true).unwrap();
    connection.send(first, Bytes::copy_from_slice("ij"_bytes), true).unwrap();
    usize count;
    for (auto& output : connection.take_output()) {
        if (output.payload_length == usize()) {
            EXPECT_EQ(output.payload_offset, usize());
            continue;
        }
        EXPECT_EQ(output.payload_offset, usize(9));
        EXPECT_EQ(output.bytes.len(), output.payload_offset + output.payload_length);
        EXPECT_EQ(output.stream, count == usize(1) ? second : first);
        EXPECT_EQ(output.payload_length,
                  count == usize(1)  ? usize(5)
                  : count == usize() ? usize(3)
                                     : usize(2));
        ++count;
    }
    EXPECT_EQ(count, usize(3));
}
TEST(Http2, ConnectAuthorityPreservesSensitiveFieldsAndRejectsHttpMessageFields) {
    Vec<h2::Field> fields;
    fields.push({ "proxy-authorization"_Str, Bytes::copy_from_slice("Basic secret"_bytes), true });
    fields.push({ "padding"_Str, Bytes::copy_from_slice("random"_bytes), true });
    auto request = h2::connect_request("[2001:db8::1]:443"_str, fields.as_slice()).unwrap();
    EXPECT_TRUE(request.tunnel);
    EXPECT_EQ(request.fields.len(), usize(4));
    EXPECT_EQ(request.fields[usize()].name.as_str(), ":method"_str);
    EXPECT_EQ(request.fields[usize(1)].name.as_str(), ":authority"_str);
    EXPECT_TRUE(request.fields[usize(2)].sensitive);
    h2::HpackDecoder decoder;
    auto decoded = decoder.decode(h2::encode_fields(request.fields.as_slice()).unwrap()).unwrap();
    EXPECT_TRUE(decoded[usize(2)].sensitive);
    EXPECT_TRUE(decoded[usize(3)].sensitive);
    for (auto authority : array<ref<str>, 5> { "example.com"_str,
                                               "example.com:0"_str,
                                               "example.com:abc"_str,
                                               "user@example.com:443"_str,
                                               "example.com:443/path"_str })
        EXPECT_TRUE(h2::connect_request(authority).is_err());
    fields.push({ "content-length"_Str, Bytes::copy_from_slice("0"_bytes), false });
    EXPECT_TRUE(h2::connect_request("example.com:443"_str, fields.as_slice()).is_err());
}
TEST(Http2, ConnectIgnoresSuccessfulContentLengthAndPreservesBothHalfCloses) {
    h2::ClientConnection connection;
    connection.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    auto id = connection.open(h2::connect_request("example.com:443"_str).unwrap()).unwrap();
    connection.feed(h2_headers(id, "880f0d0130"_str)).unwrap();
    connection.feed(h2::frame(u8(), u8(1), id, Bytes::copy_from_slice("abc"_bytes))).unwrap();
    auto events = connection.take_events();
    EXPECT_EQ(events.len(), usize(3));
    EXPECT_TRUE(events[usize(1)].is_Data());
    EXPECT_TRUE(events[usize(2)].is_End());
    EXPECT_TRUE(connection.send(id, Bytes::copy_from_slice("still writable"_bytes)).is_ok());
    EXPECT_TRUE(connection.send(id, {}, true).is_ok());
    connection.consume(id, usize(3));
    connection.forget(id);
    EXPECT_EQ(connection.capacity(), usize(128));
}
TEST(Http2, ConnectNonSuccessStillUsesNormalResponseFraming) {
    h2::ClientConnection connection;
    connection.feed(h2::frame(u8(4), u8(), u32(), {})).unwrap();
    auto id = connection.open(h2::connect_request("example.com:443"_str).unwrap()).unwrap();
    Vec<h2::Field> fields;
    fields.push({ ":status"_Str, Bytes::copy_from_slice("407"_bytes), false });
    fields.push({ "content-length"_Str, Bytes::copy_from_slice("0"_bytes), false });
    connection.feed(h2::frame(u8(1), u8(4), id, h2::encode_fields(fields.as_slice()).unwrap()))
        .unwrap();
    connection.feed(h2::frame(u8(), u8(1), id, Bytes::copy_from_slice("bad"_bytes))).unwrap();
    bool reset = false;
    for (const auto& event : connection.take_events()) reset = reset || event.is_Reset();
    EXPECT_TRUE(reset);
}
