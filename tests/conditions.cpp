#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

auto representation(ref<str> tag = "\"one\""_str) -> RepresentationMetadata {
    RepresentationMetadata result;
    result.length        = u64(10);
    result.etag          = Some(EntityTag::parse(tag.as_bytes()).unwrap());
    result.last_modified = http_date("Sun, 06 Nov 1994 08:49:37 GMT"_str);
    return result;
}
auto evaluate(const Headers& headers, ref<str> method = "GET"_str) -> RepresentationDecision {
    return RequestConditions::parse(headers).unwrap().evaluate(method, representation());
}
TEST(RequestConditions, PreconditionsFollowTagAndDatePrecedenceBeforeRange) {
    Headers headers;
    headers.add("if-match"_str, "\"one\""_str).unwrap();
    headers.add("if-unmodified-since"_str, "Sun, 06 Nov 1994 08:49:36 GMT"_str).unwrap();
    headers.add("if-none-match"_str, "\"other\""_str).unwrap();
    headers.add("if-modified-since"_str, "Sun, 06 Nov 1994 08:49:37 GMT"_str).unwrap();
    headers.add("range"_str, "bytes=1-3"_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Partial);
    headers.set("if-none-match"_str, "W/\"one\""_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::NotModified);
    EXPECT_EQ(evaluate(headers, "PUT"_str).kind, RepresentationKind::PreconditionFailed);
    headers.set("if-match"_str, "W/\"one\""_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::PreconditionFailed);
    headers.remove("if-match"_str);
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::PreconditionFailed);
    headers.remove("if-unmodified-since"_str);
    headers.remove("if-none-match"_str);
    EXPECT_EQ(evaluate(headers, "HEAD"_str).kind, RepresentationKind::NotModified);
    EXPECT_EQ(evaluate(headers, "POST"_str).kind, RepresentationKind::Full);
    EXPECT_EQ(evaluate(headers, "OPTIONS"_str).kind, RepresentationKind::Full);
}
TEST(RequestConditions, OwnsRepeatedListsAndOpaqueBytesAfterHeadersAreDestroyed) {
    Option<RequestConditions> parsed;
    array<u8, 3>              wire { u8('"'), u8(0xff), u8('"') };
    {
        Headers headers;
        headers.add("if-none-match"_str, ", \"other,a\","_str).unwrap();
        headers.push(Header::make("if-none-match"_str, wire.as_slice()).unwrap());
        parsed = Some(RequestConditions::parse(headers).unwrap());
    }
    auto meta = representation();
    meta.etag = Some(EntityTag::parse(wire.as_slice()).unwrap());
    EXPECT_EQ(parsed->evaluate("GET"_str, meta).kind, RepresentationKind::NotModified);
    EXPECT_TRUE(EntityTag::parse("W/\"a\\b,c\""_bytes)
                    .unwrap()
                    .weak_eq(EntityTag::parse("\"a\\b,c\""_bytes).unwrap()));
    EXPECT_FALSE(EntityTag::parse("W/\"a\""_bytes)
                     .unwrap()
                     .strong_eq(EntityTag::parse("\"a\""_bytes).unwrap()));
    EXPECT_FALSE(entity_tag_matches("\"one\", bad"_str, "\"one\""_str));
}
TEST(RequestConditions, WildcardUsesExistenceNotAvailabilityOfETag) {
    Headers headers;
    headers.add("if-match"_str, "*"_str).unwrap();
    auto parsed = RequestConditions::parse(headers).unwrap();
    auto meta   = representation();
    meta.etag   = None();
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::Full);
    meta.exists = false;
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::PreconditionFailed);
    headers.remove("if-match"_str);
    headers.add("if-none-match"_str, "*"_str).unwrap();
    parsed = RequestConditions::parse(headers).unwrap();
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::Full);
    meta.exists = true;
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::NotModified);
}
TEST(RequestConditions, InvalidTagConditionsFailButInvalidOrDuplicateDatesAreIgnored) {
    for (auto value : array<ref<str>, 6> { ""_str,
                                           "bad"_str,
                                           "\"a\" extra"_str,
                                           "*, \"a\""_str,
                                           "\"a\" \"b\""_str,
                                           "w/\"a\""_str }) {
        Headers headers;
        headers.add("if-match"_str, value).unwrap();
        EXPECT_TRUE(RequestConditions::parse(headers).is_err());
    }
    Headers headers;
    headers.add("if-match"_str, "*"_str).unwrap();
    headers.add("if-match"_str, "\"one\""_str).unwrap();
    EXPECT_TRUE(RequestConditions::parse(headers).is_err());
    headers.clear();
    headers.add("if-modified-since"_str, "bad"_str).unwrap();
    headers.add("if-unmodified-since"_str, "bad"_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Full);
    headers.set("if-modified-since"_str, "Sun, 06 Nov 1994 08:49:37 GMT"_str).unwrap();
    headers.add("if-modified-since"_str, "Sun, 06 Nov 1994 08:49:37 GMT"_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Full);
}
TEST(RequestConditions, SingleRangesClampAndFormatTheirSelectedRepresentation) {
    struct Case {
        ref<str> value;
        u64      offset, length;
    };
    for (auto item : array<Case, 6> { Case { "bytes=2-4"_str, u64(2), u64(3) },
                                      Case { "bytes=7-"_str, u64(7), u64(3) },
                                      Case { "bytes=-4"_str, u64(6), u64(4) },
                                      Case { "Bytes=8-999"_str, u64(8), u64(2) },
                                      Case { "bytes=-999"_str, u64(), u64(10) },
                                      Case { "bytes=0002-0004"_str, u64(2), u64(3) } }) {
        Headers headers;
        headers.add("range"_str, item.value).unwrap();
        auto selected = evaluate(headers);
        EXPECT_EQ(selected.status().value(), u16(206));
        EXPECT_EQ(selected.range.offset, item.offset);
        EXPECT_EQ(selected.range.length, item.length);
        EXPECT_EQ(selected.content_range(u64(10))->as_str(),
                  rstd::format("bytes {}-{}/10", item.offset, item.offset + item.length - u64(1))
                      .as_str());
        EXPECT_EQ(evaluate(headers, "HEAD"_str).kind, RepresentationKind::Full);
    }
    for (auto value :
         array<ref<str>, 3> { "bytes=10-"_str, "bytes=999-1000"_str, "bytes=-0"_str }) {
        Headers headers;
        headers.add("range"_str, value).unwrap();
        auto selected = evaluate(headers);
        EXPECT_EQ(selected.status().value(), u16(416));
        EXPECT_EQ(selected.content_range(u64(10))->as_str(), "bytes */10"_str);
    }
}
TEST(RequestConditions, UnsupportedInvalidAndOverflowRangesAreIgnoredWithoutWrapping) {
    for (auto value : array<ref<str>, 10> { "bytes=0-0,-1"_str,
                                            "pages=0-1"_str,
                                            "bytes=9-2"_str,
                                            "bytes="_str,
                                            "bytes=-"_str,
                                            "bytes=+1-2"_str,
                                            "bytes=1 - 2"_str,
                                            "bytes=18446744073709551616-"_str,
                                            "bytes=0-18446744073709551616"_str,
                                            "bytes=-18446744073709551616"_str }) {
        Headers headers;
        headers.add("range"_str, value).unwrap();
        EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Full);
    }
    Headers headers;
    headers.add("range"_str, "bytes=0-1"_str).unwrap();
    headers.add("range"_str, "bytes=2-3"_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Full);
    headers.remove("range"_str);
    headers.add("range"_str, "bytes=0-"_str).unwrap();
    auto parsed = RequestConditions::parse(headers).unwrap();
    auto meta   = representation();
    meta.length = u64();
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::Full);
    meta.length        = u64(10);
    meta.accept_ranges = false;
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::Full);
}
TEST(RequestConditions, IfRangeRequiresExactStrongValidatorAndFallsBackToFull) {
    Headers headers;
    headers.add("range"_str, "bytes=2-3"_str).unwrap();
    auto meta = representation();
    for (auto value : array<ref<str>, 4> {
             "W/\"one\""_str, "\"other\""_str, "bad"_str, "Sun, 06 Nov 1994 08:49:38 GMT"_str }) {
        headers.set("if-range"_str, value).unwrap();
        EXPECT_EQ(RequestConditions::parse(headers)->evaluate("GET"_str, meta).kind,
                  RepresentationKind::Full);
    }
    headers.set("if-range"_str, "\"one\""_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Partial);
    meta.etag = Some(EntityTag::parse("W/\"one\""_bytes).unwrap());
    EXPECT_EQ(RequestConditions::parse(headers)->evaluate("GET"_str, meta).kind,
              RepresentationKind::Full);
    headers.set("if-range"_str, "Sun, 06 Nov 1994 08:49:37 GMT"_str).unwrap();
    auto parsed = RequestConditions::parse(headers).unwrap();
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::Full);
    meta.last_modified_strong = true;
    EXPECT_EQ(parsed.evaluate("GET"_str, meta).kind, RepresentationKind::Partial);
    headers.add("if-range"_str, "\"one\""_str).unwrap();
    EXPECT_EQ(evaluate(headers).kind, RepresentationKind::Full);
}
