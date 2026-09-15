#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

TEST(Query, OwnedFormDecodingPreservesBoundariesAndDuplicates) {
    auto fields =
        decode_form_query("q=C%2B%2B&q=KV+Cache&%26=%3D&x=%252B&flag&=v&"_str.as_bytes()).unwrap();
    ASSERT_EQ(fields.len(), usize(7));
    EXPECT_EQ(rstd::str_::from_utf8(fields[usize()].value.as_slice()).unwrap(), "C++"_str);
    EXPECT_EQ(rstd::str_::from_utf8(fields[usize(1)].value.as_slice()).unwrap(), "KV Cache"_str);
    EXPECT_EQ(rstd::str_::from_utf8(fields[usize(2)].name.as_slice()).unwrap(), "&"_str);
    EXPECT_EQ(rstd::str_::from_utf8(fields[usize(2)].value.as_slice()).unwrap(), "="_str);
    EXPECT_EQ(rstd::str_::from_utf8(fields[usize(3)].value.as_slice()).unwrap(), "%2B"_str);
    EXPECT_TRUE(fields[usize(4)].value.is_empty());
    EXPECT_TRUE(fields[usize(5)].name.is_empty());
    EXPECT_TRUE(fields[usize(6)].name.is_empty());
    auto binary = decode_form_query("x=%00%FF"_str.as_bytes()).unwrap();
    EXPECT_EQ(binary[usize()].value[usize()], u8());
    EXPECT_EQ(binary[usize()].value[usize(1)], u8(255));
}
TEST(Query, RejectsBadEscapesAndLimits) {
    for (auto text : array<ref<str>, 3> { "x=%"_str, "x=%1"_str, "x=%GG"_str })
        EXPECT_TRUE(decode_form_query(text.as_bytes()).is_err());
    EXPECT_EQ(decode_form_query("a=b"_str.as_bytes(), usize(2)).unwrap_err(),
              DecodeError::TooLarge);
    EXPECT_TRUE(decode_form_query("a=b"_str.as_bytes(), usize(3), usize(1)).is_ok());
    EXPECT_TRUE(decode_form_query("a&b"_str.as_bytes(), usize(3), usize(1)).is_err());
    EXPECT_TRUE(decode_form_query(""_str.as_bytes(), usize(), usize()).is_ok());
}

namespace
{
auto line(ref<str> input) -> Result<RequestLine, DecodeError> {
    RequestLineDecoder decoder;
    auto               result = decoder.feed(input.as_bytes(), true);
    if (result.is_err()) return Err(result.unwrap_err());
    return Ok(decoder.take().unwrap());
}
auto host(ref<str> input) -> Result<RequestHead, DecodeError> {
    RequestHeadDecoder decoder;
    auto               start = decoder.feed("GET / HTTP/1.1\r\nHost: "_str.as_bytes());
    if (start.is_err()) return Err(start.unwrap_err());
    auto middle = decoder.feed(input.as_bytes());
    if (middle.is_err()) return Err(middle.unwrap_err());
    auto end = decoder.feed("\r\n\r\n"_str.as_bytes(), true);
    if (end.is_err()) return Err(end.unwrap_err());
    return Ok(decoder.take().unwrap());
}
} // namespace

TEST(RequestTarget, KeepsSegmentBoundariesAndDecodesOnce) {
    auto parsed = line("GET /api/a%2Fb/%252e%252e//../?q=a+b&x=%2F? HTTP/1.1\r\n"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto& target = parsed->resource;
    EXPECT_EQ(target.form, TargetForm::Origin);
    EXPECT_EQ(target.escaped_path.as_str(), "/api/a%2Fb/%252e%252e//../"_str);
    ASSERT_EQ(target.segments.len(), usize(6));
    EXPECT_EQ(rstd::str_::from_utf8(target.segments[usize(1)].as_slice()).unwrap(), "a/b"_str);
    EXPECT_EQ(rstd::str_::from_utf8(target.segments[usize(2)].as_slice()).unwrap(), "%2e%2e"_str);
    EXPECT_TRUE(target.segments[usize(3)].is_empty());
    EXPECT_EQ(rstd::str_::from_utf8(target.segments[usize(4)].as_slice()).unwrap(), ".."_str);
    EXPECT_TRUE(target.segments[usize(5)].is_empty());
    ASSERT_TRUE(target.query.is_some());
    EXPECT_EQ(target.query->as_str(), "q=a+b&x=%2F?"_str);
    auto opaque = line("GET /%00%FF HTTP/1.1\r\n"_str);
    ASSERT_TRUE(opaque.is_ok());
    EXPECT_EQ(opaque->resource.segments[usize()][usize()], u8());
    EXPECT_EQ(opaque->resource.segments[usize()][usize(1)], u8(255));
}

TEST(RequestTarget, PreservesEmptyQueryAndAbsoluteRoot) {
    auto empty = line("GET https://Example.test? HTTP/1.1\r\n"_str);
    ASSERT_TRUE(empty.is_ok());
    EXPECT_EQ(empty->resource.form, TargetForm::Absolute);
    EXPECT_EQ(*empty->resource.scheme, TargetScheme::Https);
    EXPECT_EQ(empty->resource.escaped_path.as_str(), "/"_str);
    ASSERT_TRUE(empty->resource.query.is_some());
    EXPECT_TRUE(empty->resource.query->is_empty());
    auto absent = line("GET http://x HTTP/1.1\r\n"_str);
    ASSERT_TRUE(absent.is_ok());
    EXPECT_TRUE(absent->resource.query.is_none());
    EXPECT_EQ(absent->resource.escaped_path.as_str(), "/"_str);
    auto double_slash = line("GET //path HTTP/1.1\r\n"_str);
    ASSERT_TRUE(double_slash.is_ok());
    EXPECT_EQ(double_slash->resource.form, TargetForm::Origin);
    EXPECT_TRUE(double_slash->resource.authority.is_none());
    EXPECT_EQ(double_slash->resource.segments.len(), usize(2));
    EXPECT_TRUE(double_slash->resource.segments[usize()].is_empty());
}

TEST(RequestTarget, MethodSpecificForms) {
    auto connect = line("CONNECT [2001:db8::1]:443 HTTP/1.1\r\n"_str);
    ASSERT_TRUE(connect.is_ok());
    EXPECT_EQ(connect->resource.form, TargetForm::Authority);
    EXPECT_EQ(connect->resource.authority->kind, HostKind::Ipv6);
    EXPECT_EQ(*connect->resource.authority->port, u16(443));
    auto options = line("OPTIONS * HTTP/1.1\r\n"_str);
    ASSERT_TRUE(options.is_ok());
    EXPECT_EQ(options->resource.form, TargetForm::Asterisk);
    EXPECT_TRUE(options->resource.segments.is_empty());
}

TEST(RequestTarget, RejectsInvalidAndUnsupportedTargets) {
    ref<str> invalid[] = { "GET * HTTP/1.1\r\n"_str,
                           "options * HTTP/1.1\r\n"_str,
                           "CONNECT /rpc HTTP/1.1\r\n"_str,
                           "CONNECT x HTTP/1.1\r\n"_str,
                           "CONNECT x: HTTP/1.1\r\n"_str,
                           "CONNECT x:65536 HTTP/1.1\r\n"_str,
                           "GET relative HTTP/1.1\r\n"_str,
                           "GET /% HTTP/1.1\r\n"_str,
                           "GET /%0 HTTP/1.1\r\n"_str,
                           "GET /%gg HTTP/1.1\r\n"_str,
                           "GET /?q=%zz HTTP/1.1\r\n"_str,
                           "GET /#fragment HTTP/1.1\r\n"_str,
                           "GET /?q=x#fragment HTTP/1.1\r\n"_str,
                           "GET /a\\b HTTP/1.1\r\n"_str,
                           "GET /[a] HTTP/1.1\r\n"_str,
                           "GET http:///path HTTP/1.1\r\n"_str,
                           "GET http://user@host/x HTTP/1.1\r\n"_str,
                           "GET http://[invalid]/ HTTP/1.1\r\n"_str,
                           "GET http://x:bad/ HTTP/1.1\r\n"_str,
                           "GET http://x#frag HTTP/1.1\r\n"_str,
                           "GET ftp://x/path HTTP/1.1\r\n"_str };
    for (auto input : invalid) EXPECT_TRUE(line(input).is_err());
    EXPECT_EQ(line("GET ftp://x/ HTTP/1.1\r\n"_str).unwrap_err(), DecodeError::UnsupportedTarget);
}

TEST(RequestTarget, AuthoritySyntaxAndAddressKinds) {
    ref<str> names[] = { "example.test"_str,  "example.test:0"_str, "example.test:65535"_str,
                         "example.test:"_str, "%65xample.test"_str, "a!$&'()*+,;=b"_str };
    for (auto input : names) {
        auto parsed = host(input);
        ASSERT_TRUE(parsed.is_ok());
        EXPECT_EQ(parsed->authority->kind, HostKind::Name);
    }
    auto v4 = host("127.0.0.1:80"_str);
    ASSERT_TRUE(v4.is_ok());
    EXPECT_EQ(v4->authority->kind, HostKind::Ipv4);
    EXPECT_EQ(*v4->authority->port, u16(80));
    ref<str> addresses[] = { "[::]"_str,  "[::1]:80"_str,           "[1:2:3:4:5:6:7:8]"_str,
                             "[1::]"_str, "[::ffff:192.0.2.1]"_str, "[1:2:3:4:5:6:192.0.2.1]"_str };
    for (auto input : addresses) {
        auto parsed = host(input);
        ASSERT_TRUE(parsed.is_ok());
        EXPECT_EQ(parsed->authority->kind, HostKind::Ipv6);
    }
    auto future = host("[v1.a:b]"_str);
    ASSERT_TRUE(future.is_ok());
    EXPECT_EQ(future->authority->kind, HostKind::Future);
}

TEST(RequestTarget, RejectsInvalidHostLiteralsAndPorts) {
    ref<str> invalid[] = { "user@host"_str,
                           "host/path"_str,
                           "host?q"_str,
                           "host#x"_str,
                           "host\\x"_str,
                           "host%qz"_str,
                           "host:65536"_str,
                           "host:99999999999999999999999999"_str,
                           "host:-1"_str,
                           "host:1:2"_str,
                           "[::1"_str,
                           "[]"_str,
                           "[127.0.0.1]"_str,
                           "[::1]suffix"_str,
                           "[:::]"_str,
                           "[1::2::3]"_str,
                           "[1:2:3:4:5:6:7]"_str,
                           "[1:2:3:4:5:6:7:8:9]"_str,
                           "[1:2:3:4:5:6:7::8]"_str,
                           "[12345::]"_str,
                           "[gg::]"_str,
                           "[::ffff:256.0.0.1]"_str,
                           "[::ffff:01.0.0.1]"_str,
                           "[::ffff:1.2.3.4:5]"_str,
                           "[fe80::1%25eth0]"_str,
                           "[v1.]"_str,
                           "[v.a]"_str };
    for (auto input : invalid) EXPECT_TRUE(host(input).is_err());
}

TEST(RequestTarget, HeadReusesOwnedTargetAndSelectsAbsoluteAuthorityEverySplit) {
    auto wire =
        "GET http://target.test:8080/a%2Fb?q=1 HTTP/1.1\r\nHost: ignored.test:80\r\n\r\n"_str;
    for (usize split {}; split <= wire.len(); ++split) {
        RequestHeadDecoder decoder;
        ASSERT_TRUE(
            decoder.feed(slice<u8>::from_raw_parts(wire.as_bytes().as_raw_ptr(), split)).is_ok());
        auto end = decoder.feed(
            slice<u8>::from_raw_parts(wire.as_bytes().as_raw_ptr() + split.to_primitive(),
                                      wire.len() - split),
            true);
        ASSERT_TRUE(end.is_ok());
        auto head = decoder.take();
        ASSERT_TRUE(head.is_some());
        decoder.reset();
        EXPECT_EQ(head->authority->name.as_str(), "target.test"_str);
        EXPECT_EQ(*head->authority->port, u16(8080));
        EXPECT_EQ(head->line.resource.authority->name.as_str(), "target.test"_str);
        EXPECT_EQ(rstd::str_::from_utf8(head->line.resource.segments[usize()].as_slice()).unwrap(),
                  "a/b"_str);
    }
    RequestHeadDecoder duplicate;
    EXPECT_TRUE(
        duplicate.feed("GET http://x/ HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n"_str.as_bytes(), true)
            .is_err());
    auto origin = host("source.test:4321"_str);
    ASSERT_TRUE(origin.is_ok());
    EXPECT_EQ(origin->authority->name.as_str(), "source.test"_str);
}
