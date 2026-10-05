#include <rstd/test/gtest.hpp>
import lihttpto;
import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

static auto request() -> UpgradeRequest {
    return UpgradeRequest::make(Method::parse("POST"_str).unwrap(),
                                "/attach?stream=true"_str,
                                "localhost"_str,
                                UpgradeProtocol::make("tcp"_str).unwrap())
        .unwrap();
}
static auto response(ref<str> text) -> ResponseHead {
    return rstd::move(MessageHead::parse(text.as_bytes()).unwrap()).into_response().unwrap();
}
TEST(Upgrade, RequestIsBoundedAndRejectsInjection) {
    auto value = request();
    EXPECT_TRUE(
        rstd::str_::from_utf8(value.bytes().as_slice()).unwrap() ==
        "POST /attach?stream=true HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: tcp\r\nContent-Length: 0\r\n\r\n"_str);
    for (auto target :
         array<ref<str>, 3> { "/x\r\nInjected: yes"_str, "http://x/"_str, "/%zz"_str })
        EXPECT_TRUE(UpgradeRequest::make(Method::parse("POST"_str).unwrap(),
                                         target,
                                         "localhost"_str,
                                         UpgradeProtocol::make("tcp"_str).unwrap())
                        .is_err());
    EXPECT_TRUE(UpgradeProtocol::make("tcp\r\n"_str).is_err());
    EXPECT_TRUE(UpgradeRequest::make(Method::parse("POST"_str).unwrap(),
                                     "/"_str,
                                     "localhost\r\nx"_str,
                                     UpgradeProtocol::make("tcp"_str).unwrap())
                    .is_err());
    Headers reserved;
    reserved.add("Content-Length"_str, "3"_str).unwrap();
    EXPECT_TRUE(UpgradeRequest::make(Method::parse("POST"_str).unwrap(),
                                     "/"_str,
                                     "localhost"_str,
                                     UpgradeProtocol::make("tcp"_str).unwrap(),
                                     reserved)
                    .unwrap_err() == UpgradeError::ReservedHeader);
    EXPECT_TRUE(UpgradeRequest::make(Method::parse("POST"_str).unwrap(),
                                     "/"_str,
                                     "localhost"_str,
                                     UpgradeProtocol::make("tcp"_str).unwrap(),
                                     {},
                                     usize(10))
                    .unwrap_err() == UpgradeError::TooLarge);
}
TEST(Upgrade, ResponseMustSelectOfferedProtocol) {
    auto value = request();
    EXPECT_TRUE(
        value
            .validate(response(
                "HTTP/1.1 101 Switching Protocols\r\nConnection: keep-alive\r\nConnection: UpGrAdE\r\nUpgrade: TCP\r\n\r\n"_str))
            .is_ok());
    for (auto headers : array<ref<str>, 7> {
             "Connection: Upgrade\r\n"_str,
             "Upgrade: tcp\r\n"_str,
             "Connection: Upgrade, close\r\nUpgrade: tcp\r\n"_str,
             "Connection: Upgrade\r\nUpgrade: websocket\r\n"_str,
             "Connection: Upgrade\r\nUpgrade: tcp\r\nUpgrade: tcp\r\n"_str,
             "Connection: Upgrade\r\nUpgrade: tcp\r\nContent-Length: 0\r\n"_str,
             "Connection: Upgrade\r\nUpgrade: tcp\r\nTransfer-Encoding: chunked\r\n"_str }) {
        auto text = rstd::format("HTTP/1.1 101 Switching Protocols\r\n{}\r\n", headers);
        EXPECT_TRUE(value.validate(response(text.as_str())).is_err());
    }
    EXPECT_TRUE(value.validate(response("HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\n"_str))
                    .unwrap_err() == UpgradeError::RejectedStatus);
    auto protocol = UpgradeProtocol::make("sample"_str, Some("vA"_str)).unwrap();
    EXPECT_TRUE(protocol.matches("SAMPLE/vA"_str.as_bytes()));
    EXPECT_TRUE(! protocol.matches("sample/va"_str.as_bytes()));
}

TEST(Upgrade, HeadConsumptionIsRelativeToCurrentFragment) {
    auto head =
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: tcp\r\n\r\n"_str;
    auto wire = rstd::format("{}payload", head);
    for (usize split; split < head.len(); ++split) {
        Http1HeadParser parser;
        auto            bytes = wire.as_str().as_bytes();
        auto            first = parser.push(slice<u8>::from_raw_parts(bytes.as_raw_ptr(), split));
        ASSERT_TRUE(first.is_ok() && first->is_NeedMore());
        auto tail   = slice<u8>::from_raw_parts(bytes.as_raw_ptr() + split.to_primitive(),
                                                bytes.len() - split);
        auto second = parser.push(tail);
        ASSERT_TRUE(second.is_ok() && second->is_Complete());
        auto complete = rstd::move(second).unwrap().as_Complete();
        EXPECT_TRUE(complete.consumed == head.len());
        EXPECT_TRUE(complete.input_consumed == head.len() - split);
        auto remaining =
            slice<u8>::from_raw_parts(tail.as_raw_ptr() + complete.input_consumed.to_primitive(),
                                      tail.len() - complete.input_consumed);
        EXPECT_TRUE(remaining == "payload"_str.as_bytes());
    }
}

TEST(Upgrade, ResponseFramingAndInformationalPolicy) {
    auto value = request();
    EXPECT_TRUE(*value.classify(response("HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n"_str)) ==
                UpgradeDisposition::Informational);
    EXPECT_TRUE(value.classify(response("HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n"_str))
                    .is_err());
    for (auto fields : array<ref<str>, 3> {
             "Content-Length: 2\r\n"_str, "Transfer-Encoding: chunked\r\n"_str, ""_str }) {
        auto text    = rstd::format("HTTP/1.1 403 Forbidden\r\n{}\r\n", fields);
        auto framing = value.body_framing(response(text.as_str()));
        ASSERT_TRUE(framing.is_ok());
        EXPECT_TRUE(framing->kind == (fields.is_empty()                   ? BodyKind::UntilEof
                                      : fields.starts_with("Content"_str) ? BodyKind::FixedLength
                                                                          : BodyKind::Chunked));
    }
    for (auto fields :
         array<ref<str>, 4> { "Content-Length: 2\r\nContent-Length: 3\r\n"_str,
                              "Content-Length: 2\r\nTransfer-Encoding: chunked\r\n"_str,
                              "Transfer-Encoding: gzip\r\n"_str,
                              "Content-Length: -1\r\n"_str }) {
        auto text = rstd::format("HTTP/1.1 403 Forbidden\r\n{}\r\n", fields);
        EXPECT_TRUE(value.body_framing(response(text.as_str())).is_err());
    }
    BodyLimits limits;
    limits.data_bytes = u64(3);
    BodyDecoder body({ BodyKind::UntilEof }, limits);
    EXPECT_TRUE(body.feed("ab"_str.as_bytes()).is_ok());
    EXPECT_TRUE(body.feed("cd"_str.as_bytes()).unwrap_err() == DecodeError::TooLarge);
    EXPECT_TRUE(body.finish().is_err());
    BodyDecoder complete({ BodyKind::UntilEof }, limits);
    EXPECT_TRUE(complete.feed("abc"_str.as_bytes()).is_ok());
    EXPECT_TRUE(complete.finish().is_ok());
}
