#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;
struct Suffixes {
    auto is_public_suffix(ref<str> value) const -> bool {
        return value == "com"_str || value == "co.uk"_str;
    }
};
namespace rstd
{
template<>
struct Impl<PublicSuffixPolicy, Suffixes> : ImplBase<Suffixes> {
    auto is_public_suffix(ref<str> value) const -> bool {
        return this->self().is_public_suffix(value);
    }
};
} // namespace rstd
auto rules() -> CookieOptions {
    CookieOptions value;
    value.suffixes = Some(rstd::sync::Arc<rstd::dyn<PublicSuffixPolicy>>::make(Suffixes {}));
    return value;
}
TEST(CookieStorage, ScopeOrderingReplacementAndExpiration) {
    CookieStorage store(rules());
    auto          source = Url::parse_http("https://api.example.com/a/b"_str).unwrap();
    auto          accept = [&](ref<str> value, i64 now = i64(100)) {
        return store.store(SetCookie::parse(value).unwrap(), source, now);
    };
    EXPECT_TRUE(accept("id=host; HttpOnly"_str));
    EXPECT_TRUE(accept("id=domain; Domain=.EXAMPLE.com; Path=/; Secure"_str));
    EXPECT_TRUE(accept("n=long; Path=/a/b"_str));
    EXPECT_EQ(store.select(source, i64(100)).encode().as_str(), "n=long; id=host; id=domain"_str);
    auto other = Url::parse_http("https://other.example.com/a/b"_str).unwrap();
    EXPECT_EQ(store.select(other, i64(100)).encode().as_str(), "id=domain"_str);
    EXPECT_TRUE(store.select(Url::parse_http("http://other.example.com/a/b"_str).unwrap(), i64(100))
                    .is_empty());
    EXPECT_EQ(store.select(Url::parse_http("https://api.example.com/ab"_str).unwrap(), i64(100))
                  .encode()
                  .as_str(),
              "id=domain"_str);
    EXPECT_FALSE(accept("bad=1; Domain=com"_str));
    EXPECT_FALSE(accept("bad=1; Domain=evil.com"_str));
    EXPECT_TRUE(accept("id=new; Max-Age=2; Expires=Wed, 21 Oct 2015 07:28:00 GMT"_str));
    EXPECT_EQ(store.select(source, i64(101)).encode().as_str(), "n=long; id=new; id=domain"_str);
    EXPECT_EQ(store.select(source, i64(102)).encode().as_str(), "n=long; id=domain"_str);
    EXPECT_TRUE(accept("id=gone; Domain=example.com; Path=/; Max-Age=0"_str));
    EXPECT_TRUE(store.select(other, i64(100)).is_empty());
    CookieStorage conservative;
    EXPECT_FALSE(conservative.store(
        SetCookie::parse("a=1; Domain=example.com"_str).unwrap(), source, i64(100)));
    EXPECT_TRUE(conservative.store(
        SetCookie::parse("a=1; Domain=api.example.com"_str).unwrap(), source, i64(100)));
    EXPECT_TRUE(conservative.select(other, i64(100)).is_empty());
}
TEST(CookieStorage, CookieDatesAreIndependentOfHttpDateGrammar) {
    auto expected = http_date("Wed, 21 Oct 2015 07:28:00 GMT"_str).unwrap();
    for (auto text : array<ref<str>, 3> { "Wed, 21 Oct 2015 07:28:00 GMT"_str,
                                          "Wednesday, 21-Oct-15 07:28:00 GMT"_str,
                                          "07:28:00 21 oCt 2015"_str })
        EXPECT_EQ(cookie_date(text).unwrap(), expected);
    for (auto text : array<ref<str>, 5> { "21 Oct 1500 07:28:00"_str,
                                          "31 Feb 2015 07:28:00"_str,
                                          "21 Oct 2015 24:28:00"_str,
                                          "21 Oct 2015 07:28:60"_str,
                                          "21 Oct1 2015 07:28:00"_str })
        EXPECT_TRUE(cookie_date(text).is_none());
}
TEST(CookieStorage, ExplicitUnverifiedSuffixAndInsecureSourcePolicy) {
    CookieOptions options;
    options.allow_unverified_suffixes = true;
    options.allow_secure_from_insecure = true;
    CookieStorage store(rstd::move(options));
    auto origin = Url::parse_http("http://api.example.com/foo"_str).unwrap();
    EXPECT_TRUE(store.store(SetCookie::parse("shared=one; Domain=example.com; Path=/"_str).unwrap(),
                            origin, i64(100)));
    EXPECT_TRUE(store.store(SetCookie::parse("secure=two; Secure; Path=/"_str).unwrap(),
                            origin, i64(100)));
    auto sibling = Url::parse_http("http://www.example.com/"_str).unwrap();
    EXPECT_EQ(store.select(sibling, i64(100)).encode().as_str(), "shared=one"_str);
    EXPECT_TRUE(store.select(origin, i64(100)).get("secure"_str).is_none());
    auto secure = Url::parse_http("https://api.example.com/"_str).unwrap();
    EXPECT_TRUE(store.select(secure, i64(100)).get("secure"_str).is_some());
    EXPECT_FALSE(store.store(SetCookie::parse("bad=1; Domain=other.test"_str).unwrap(),
                             origin, i64(100)));
}
TEST(CookieStorage, PersistenceIsTransactionalAndRetainsHttpOnlyAndSessionCookies) {
    CookieStorage store(rules());
    auto          url = Url::parse_http("https://api.example.com/a/b"_str).unwrap();
    EXPECT_TRUE(store.store(
        SetCookie::parse("a=\"one\"; Domain=example.com; Path=/; Secure; HttpOnly"_str).unwrap(),
        url,
        i64(100)));
    EXPECT_TRUE(
        store.store(SetCookie::parse("b=two; Path=/a; Max-Age=60"_str).unwrap(), url, i64(100)));
    auto          file = store.export_netscape(i64(100));
    CookieStorage loaded(rules());
    EXPECT_TRUE(loaded.import_netscape(file.as_str(), i64(100)).is_ok());
    EXPECT_TRUE(loaded.import_netscape(file.as_str(), i64(100)).is_ok());
    EXPECT_EQ(loaded.snapshot(i64(100)).len(), usize(2));
    EXPECT_EQ(loaded.select(url, i64(100)).encode().as_str(), "b=two; a=\"one\""_str);
    EXPECT_TRUE(loaded.snapshot(i64(100))[usize()].http_only);
    auto invalid = rstd::format("{}bad\tline\n", file);
    EXPECT_TRUE(loaded.import_netscape(invalid.as_str(), i64(100)).is_err());
    EXPECT_EQ(loaded.export_netscape(i64(100)).as_str(), file.as_str());
    EXPECT_TRUE(loaded.import_netscape("com\tTRUE\t/\tFALSE\t0\tbad\t1\n"_str, i64(100)).is_err());
    EXPECT_TRUE(
        loaded.import_netscape("example.com\tFALSE\t/\tFALSE\t0\ta\tbad value\n"_str, i64(100))
            .is_err());
}
TEST(CookieStorage, SnapshotPreservesMetadataAndRestoresAtomically) {
    CookieStorage original(rules());
    auto url = Url::parse_http("https://api.example.com/a/b"_str).unwrap();
    EXPECT_TRUE(original.store(SetCookie::parse(
        "sid=one; Domain=example.com; Path=/a; Secure; HttpOnly; SameSite=Lax; Max-Age=2"_str).unwrap(),
        url, i64(100)));
    EXPECT_TRUE(original.store(SetCookie::parse("session=two"_str).unwrap(), url, i64(100)));
    EXPECT_EQ(original.select(url, i64(102)).encode().as_str(), "session=two"_str);
    auto snapshot = original.snapshot_all();
    ASSERT_EQ(snapshot.len(), usize(2));
    EXPECT_TRUE(snapshot[usize()].path_explicit);
    EXPECT_TRUE(snapshot[usize()].raw_cookie.as_str().contains("SameSite=Lax"_str));
    EXPECT_EQ(*snapshot[usize()].expires, i64(102));
    EXPECT_FALSE(snapshot[usize()].host_only);
    EXPECT_FALSE(snapshot[usize(1)].path_explicit);
    CookieStorage restored(rules());
    EXPECT_TRUE(restored.replace_snapshot(rstd::move(snapshot)).is_ok());
    EXPECT_EQ(restored.snapshot_all().len(), usize(2));
    EXPECT_EQ(restored.select(url, i64(102)).encode().as_str(), "session=two"_str);
    auto invalid = restored.snapshot_all();
    invalid[usize()].domain = String::make("com"_str);
    EXPECT_TRUE(restored.replace_snapshot(rstd::move(invalid)).is_err());
    EXPECT_EQ(restored.snapshot_all().len(), usize(2));
}
TEST(CookieStorage, RetainsExpiredReplacementsAndBoundsMaxAgeWhenConfigured) {
    CookieOptions options;
    options.retain_expired_replacements = true;
    options.max_age_expiry = Some(i64(253402300799LL));
    CookieStorage store(rstd::move(options));
    auto url = Url::parse_http("https://example.test/"_str).unwrap();
    EXPECT_TRUE(store.store(SetCookie::parse("sid=old; Path=/; HttpOnly"_str).unwrap(), url, i64(100)));
    EXPECT_TRUE(store.store(SetCookie::parse("sid=deleted; Path=/; Max-Age=0"_str).unwrap(), url, i64(100)));
    EXPECT_TRUE(store.select(url, i64(100)).is_empty());
    auto expired = store.snapshot_all();
    ASSERT_EQ(expired.len(), usize(1));
    EXPECT_EQ(expired[usize()].cookie.value(), "old"_str);
    EXPECT_TRUE(expired[usize()].http_only);
    EXPECT_EQ(*expired[usize()].expires, i64(0));
    EXPECT_TRUE(store.store(SetCookie::parse("never=added; Max-Age=0"_str).unwrap(), url, i64(100)));
    EXPECT_EQ(store.snapshot_all().len(), usize(1));
    EXPECT_TRUE(store.store(SetCookie::parse("long=life; Max-Age=9223372036854775807"_str).unwrap(),
                            url, i64(100)));
    auto snapshot = store.snapshot_all();
    ASSERT_EQ(snapshot.len(), usize(2));
    EXPECT_EQ(*snapshot[usize(1)].expires, i64(253402300799LL));
}
TEST(ResponseFields, ContentLengthRejectsDuplicateAndInvalidValues) {
    Headers fields;
    EXPECT_TRUE(content_length(fields).unwrap().is_none());
    fields.add("Content-Length"_str, "42"_str).unwrap();
    EXPECT_EQ(*content_length(fields).unwrap(), u64(42));
    fields.add("content-length"_str, "42"_str).unwrap();
    EXPECT_EQ(content_length(fields).unwrap_err(), DecodeError::AmbiguousFraming);
    fields.remove("content-length"_str);
    fields.add("Content-Length"_str, "+1"_str).unwrap();
    EXPECT_TRUE(content_length(fields).is_err());
}
