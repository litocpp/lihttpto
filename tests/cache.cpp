#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
TEST(HttpCacheFields, DirectivesDatesAndValidators) {
    lihttpto::Headers headers;
    headers.push(lihttpto::Header::make("Cache-Control"_str,
                                        "PRIVATE, max-age=\"60\", no-cache=\"x,y\""_bytes)
                     .unwrap());
    headers.push(lihttpto::Header::make("ETag"_str, "W/\"a,b\""_bytes).unwrap());
    auto fields = lihttpto::CacheFields::parse(headers);
    EXPECT_TRUE(fields.control.private_response);
    EXPECT_TRUE(fields.control.no_cache);
    EXPECT_EQ(*fields.control.max_age, i64(60));
    EXPECT_EQ(fields.etag->as_str(), "W/\"a,b\""_str);
    EXPECT_TRUE(lihttpto::entity_tag_matches("\"other\", \"a,b\""_str, "W/\"a,b\""_str));
    EXPECT_FALSE(lihttpto::entity_tag_matches("\"a\""_str, "\"a,b\""_str));
    EXPECT_EQ(*lihttpto::http_date("Sun, 06 Nov 1994 08:49:37 GMT"_str), i64(784111777));
    EXPECT_EQ(*lihttpto::http_date("Sunday, 06-Nov-94 08:49:37 GMT"_str), i64(784111777));
    EXPECT_EQ(*lihttpto::http_date("Sun Nov  6 08:49:37 1994"_str), i64(784111777));
    EXPECT_TRUE(lihttpto::http_date("Sun, 31 Feb 1994 08:49:37 GMT"_str).is_none());
    headers.push(lihttpto::Header::make("cache-control"_str, "max-age=120"_bytes).unwrap());
    EXPECT_FALSE(lihttpto::CacheFields::parse(headers).reusable);
}
TEST(HttpCacheFields, NotModifiedPreservesBodyHeaders) {
    lihttpto::Headers old, update;
    old.push(lihttpto::Header::make("content-length"_str, "15"_bytes).unwrap());
    old.push(lihttpto::Header::make("content-encoding"_str, "gzip"_bytes).unwrap());
    update.push(lihttpto::Header::make("content-length"_str, "0"_bytes).unwrap());
    update.push(lihttpto::Header::make("cache-control"_str, "max-age=30"_bytes).unwrap());
    auto merged = lihttpto::merge_not_modified(old, update);
    EXPECT_EQ(*lihttpto::header_text(merged, "content-length"_str), "15"_str);
    EXPECT_EQ(*lihttpto::header_text(merged, "content-encoding"_str), "gzip"_str);
    EXPECT_EQ(*lihttpto::CacheFields::parse(merged).control.max_age, i64(30));
    merged.push(lihttpto::Header::make("Vary"_str, "*"_bytes).unwrap());
    EXPECT_FALSE(lihttpto::CacheFields::parse(merged).reusable);
}

TEST(HttpDate, CalendarValidationFormattingAndObsoleteYearWindow) {
    auto reference = *lihttpto::http_date("Sun, 06 Nov 1994 08:49:37 GMT"_str);
    for (auto text : array<ref<str>, 3> { "Sun, 06 Nov 1994 08:49:37 GMT"_str,
                                          "Sunday, 06-Nov-94 08:49:37 GMT"_str,
                                          "Sun Nov  6 08:49:37 1994"_str })
        EXPECT_EQ(*lihttpto::http_date(text, reference), reference);
    EXPECT_EQ(lihttpto::format_http_date(reference)->as_str(), "Sun, 06 Nov 1994 08:49:37 GMT"_str);
    EXPECT_EQ(lihttpto::format_http_date(i64())->as_str(), "Thu, 01 Jan 1970 00:00:00 GMT"_str);
    EXPECT_EQ(lihttpto::format_http_date(i64(-1))->as_str(), "Wed, 31 Dec 1969 23:59:59 GMT"_str);
    EXPECT_EQ(*lihttpto::http_date("Sunday, 06-Nov-44 08:49:37 GMT"_str, reference),
              *lihttpto::http_date("Sun, 06 Nov 2044 08:49:37 GMT"_str));
    EXPECT_EQ(*lihttpto::http_date("Sunday, 06-Nov-44 08:49:38 GMT"_str, reference),
              *lihttpto::http_date("Sun, 06 Nov 1944 08:49:38 GMT"_str));
    auto later_reference = *lihttpto::http_date("Mon, 28 Feb 2050 00:00:00 GMT"_str);
    EXPECT_EQ(*lihttpto::http_date("Tuesday, 29-Feb-00 00:00:00 GMT"_str, later_reference),
              *lihttpto::http_date("Tue, 29 Feb 2000 00:00:00 GMT"_str));
    EXPECT_TRUE(lihttpto::http_date("Thu, 29 Feb 2024 00:00:00 GMT"_str).is_some());
    for (auto value : array<ref<str>, 8> { "Thu, 29 Feb 2023 00:00:00 GMT"_str,
                                           "Sun, 06 Nov 1994 24:49:37 GMT"_str,
                                           "Sun, 06 Nov 1994 08:60:37 GMT"_str,
                                           "Sun, 06 Nov 1994 08:49:61 GMT"_str,
                                           "Sun, 06 Nov 1994 08:49:37 UTC"_str,
                                           "Sun, 06 nov 1994 08:49:37 GMT"_str,
                                           "Bad, 06 Nov 1994 08:49:37 GMT"_str,
                                           "Sun, 06 Nov 1994 08:49:37 GMT extra"_str })
        EXPECT_TRUE(lihttpto::http_date(value).is_none());
    EXPECT_EQ(*lihttpto::http_date("Sat, 31 Dec 2016 23:59:60 GMT"_str),
              *lihttpto::http_date("Sun, 01 Jan 2017 00:00:00 GMT"_str));
}

TEST(Cache, PolicyKeepsAgeAcrossValidation) {
    lihttpto::Headers headers;
    headers.add("Cache-Control"_str, "max-age=60"_str).unwrap();
    headers.add("Age"_str, "10"_str).unwrap();
    auto policy = lihttpto::CachePolicy::make(headers, i64(100), i64(102));
    EXPECT_EQ(policy.age(i64(105)), i64(15));
    EXPECT_TRUE(policy.fresh(i64(105), {}));
    EXPECT_FALSE(policy.fresh(i64(160), {}));
}

TEST(HttpAge, OrderedListAndSaturation) {
    lihttpto::Headers headers;
    EXPECT_TRUE(lihttpto::response_age(headers)->is_none());
    headers.add("Age"_str, " 3, 900"_str).unwrap();
    headers.add("age"_str, "invalid"_str).unwrap();
    EXPECT_EQ(**lihttpto::response_age(headers), u64(3));
    EXPECT_EQ(lihttpto::CacheFields::parse(headers).age, i64(3));
    for (auto text : array<ref<str>, 5> { "-1"_str, ""_str, ", 2"_str, "bad, 2"_str, "1 2"_str }) {
        headers.set("Age"_str, text).unwrap();
        EXPECT_TRUE(lihttpto::response_age(headers).is_err());
        EXPECT_FALSE(lihttpto::CacheFields::parse(headers).reusable);
    }
    headers.set("Age"_str, "99999999999999999999999999999999"_str).unwrap();
    EXPECT_EQ(**lihttpto::response_age(headers), u64(2147483648ull));
}
