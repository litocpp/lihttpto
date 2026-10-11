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
