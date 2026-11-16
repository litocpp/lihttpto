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
    options.allow_unverified_suffixes  = true;
    options.allow_secure_from_insecure = true;
    CookieStorage store(rstd::move(options));
    auto          origin = Url::parse_http("http://api.example.com/foo"_str).unwrap();
    EXPECT_TRUE(store.store(
        SetCookie::parse("shared=one; Domain=example.com; Path=/"_str).unwrap(), origin, i64(100)));
    EXPECT_TRUE(
        store.store(SetCookie::parse("secure=two; Secure; Path=/"_str).unwrap(), origin, i64(100)));
    auto sibling = Url::parse_http("http://www.example.com/"_str).unwrap();
    EXPECT_EQ(store.select(sibling, i64(100)).encode().as_str(), "shared=one"_str);
    EXPECT_TRUE(store.select(origin, i64(100)).get("secure"_str).is_none());
    auto secure = Url::parse_http("https://api.example.com/"_str).unwrap();
    EXPECT_TRUE(store.select(secure, i64(100)).get("secure"_str).is_some());
    EXPECT_FALSE(
        store.store(SetCookie::parse("bad=1; Domain=other.test"_str).unwrap(), origin, i64(100)));
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
    auto          url = Url::parse_http("https://api.example.com/a/b"_str).unwrap();
    EXPECT_TRUE(original.store(
        SetCookie::parse(
            "sid=one; Domain=example.com; Path=/a; Secure; HttpOnly; SameSite=Lax; Max-Age=2"_str)
            .unwrap(),
        url,
        i64(100)));
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
    auto invalid            = restored.snapshot_all();
    invalid[usize()].domain = String::make("com"_str);
    EXPECT_TRUE(restored.replace_snapshot(rstd::move(invalid)).is_err());
    EXPECT_EQ(restored.snapshot_all().len(), usize(2));
}
TEST(CookieStorage, RetainsExpiredReplacementsAndBoundsMaxAgeWhenConfigured) {
    CookieOptions options;
    options.retain_expired_replacements = true;
    options.max_age_expiry              = Some(i64(253402300799LL));
    CookieStorage store(rstd::move(options));
    auto          url = Url::parse_http("https://example.test/"_str).unwrap();
    EXPECT_TRUE(
        store.store(SetCookie::parse("sid=old; Path=/; HttpOnly"_str).unwrap(), url, i64(100)));
    EXPECT_TRUE(store.store(
        SetCookie::parse("sid=deleted; Path=/; Max-Age=0"_str).unwrap(), url, i64(100)));
    EXPECT_TRUE(store.select(url, i64(100)).is_empty());
    auto expired = store.snapshot_all();
    ASSERT_EQ(expired.len(), usize(1));
    EXPECT_EQ(expired[usize()].cookie.value(), "old"_str);
    EXPECT_TRUE(expired[usize()].http_only);
    EXPECT_EQ(*expired[usize()].expires, i64(0));
    EXPECT_TRUE(
        store.store(SetCookie::parse("never=added; Max-Age=0"_str).unwrap(), url, i64(100)));
    EXPECT_EQ(store.snapshot_all().len(), usize(1));
    EXPECT_TRUE(store.store(
        SetCookie::parse("long=life; Max-Age=9223372036854775807"_str).unwrap(), url, i64(100)));
    auto snapshot = store.snapshot_all();
    ASSERT_EQ(snapshot.len(), usize(2));
    EXPECT_EQ(*snapshot[usize(1)].expires, i64(253402300799LL));
}
TEST(CookieStorage, RevisionTracksStoreEraseAndClearWithoutNoops) {
    CookieStorage store;
    auto          url    = Url::parse_http("https://example.test/"_str).unwrap();
    auto          accept = [&](ref<str> text, i64 now = i64(100)) {
        return store.store(SetCookie::parse(text).unwrap(), url, now);
    };
    EXPECT_EQ(store.revision(), Some(u64()));
    store.clear();
    store.expire(i64(100));
    EXPECT_FALSE(store.erase("example.test"_str, "/"_str, "missing"_str));
    EXPECT_FALSE(accept("bad=one; Domain=other.test"_str));
    EXPECT_TRUE(accept("missing=one; Max-Age=0"_str));
    EXPECT_EQ(store.revision(), Some(u64()));
    ASSERT_TRUE(accept("sid=one"_str));
    EXPECT_EQ(store.revision(), Some(u64(1)));
    auto view = store.snapshot_view();
    static_assert(__is_same(decltype(view[usize()]), const StoredCookie&));
    ASSERT_EQ(view.len(), usize(1));
    EXPECT_EQ(view[usize()].cookie.value(), "one"_str);
    EXPECT_EQ(view.as_raw_ptr(), store.snapshot_view().as_raw_ptr());
    EXPECT_TRUE(accept("sid=one"_str));
    EXPECT_EQ(store.revision(), Some(u64(1)));
    EXPECT_TRUE(accept("sid=two"_str));
    EXPECT_EQ(store.revision(), Some(u64(2)));
    EXPECT_TRUE(accept("sid=two; SameSite=Lax"_str));
    EXPECT_EQ(store.revision(), Some(u64(3)));
    EXPECT_TRUE(accept("sid=two; Max-Age=10"_str));
    EXPECT_EQ(store.revision(), Some(u64(4)));
    EXPECT_TRUE(accept("sid=two; Max-Age=10"_str));
    EXPECT_EQ(store.revision(), Some(u64(4)));
    EXPECT_TRUE(accept("sid=two; Max-Age=10"_str, i64(101)));
    EXPECT_EQ(store.revision(), Some(u64(5)));
    EXPECT_FALSE(store.erase("example.test"_str, "/other"_str, "sid"_str));
    EXPECT_EQ(store.revision(), Some(u64(5)));
    EXPECT_TRUE(store.erase("example.test"_str, "/"_str, "sid"_str));
    EXPECT_EQ(store.revision(), Some(u64(6)));
    EXPECT_TRUE(accept("sid=one"_str));
    EXPECT_TRUE(accept("other=two"_str));
    store.clear();
    EXPECT_EQ(store.revision(), Some(u64(9)));
    store.clear();
    EXPECT_EQ(store.revision(), Some(u64(9)));
    EXPECT_TRUE(store.snapshot_view().is_empty());
}
TEST(CookieStorage, RevisionTracksEverySnapshotFieldAndOrder) {
    auto url = Url::parse_http("https://example.test/"_str).unwrap();
    for (unsigned field = 0; field < 12; ++field) {
        CookieStorage store;
        ASSERT_TRUE(store.store(SetCookie::parse("sid=one"_str).unwrap(), url, i64(100)));
        auto initial = store.snapshot_all();
        initial[usize()].raw_cookie.clear();
        initial[usize()].expires = Some(i64(1000));
        ASSERT_TRUE(store.replace_snapshot(rstd::move(initial)).is_ok());
        auto  before  = store.revision();
        auto  changed = store.snapshot_all();
        auto& cookie  = changed[usize()];
        switch (field) {
        case 0: cookie.raw_cookie = "sid=one; SameSite=Lax"_Str; break;
        case 1: cookie.domain = "other.test"_Str; break;
        case 2: cookie.path = "/other"_Str; break;
        case 3: cookie.expires = Some(i64(1001)); break;
        case 4: cookie.expiry_nanoseconds = 1; break;
        case 5: cookie.host_only = false; break;
        case 6: cookie.secure = true; break;
        case 7: cookie.http_only = true; break;
        case 8: cookie.path_explicit = true; break;
        case 9: cookie.cookie = Cookie::parse("sid=two"_str).unwrap(); break;
        case 10: cookie.cookie = Cookie::parse("sid=\"one\""_str).unwrap(); break;
        case 11: cookie.cookie = Cookie::parse("other=one"_str).unwrap(); break;
        }
        ASSERT_TRUE(store.replace_snapshot(rstd::move(changed)).is_ok());
        EXPECT_EQ(store.revision(), Some(*before + u64(1)));
        EXPECT_TRUE(store.replace_snapshot(store.snapshot_all()).is_ok());
        EXPECT_EQ(store.revision(), Some(*before + u64(1)));
    }
    CookieStorage store;
    ASSERT_TRUE(store.store(SetCookie::parse("a=one"_str).unwrap(), url, i64(100)));
    ASSERT_TRUE(store.store(SetCookie::parse("b=two"_str).unwrap(), url, i64(100)));
    auto reordered      = store.snapshot_all();
    auto first          = rstd::move(reordered[usize()]);
    reordered[usize()]  = rstd::move(reordered[usize(1)]);
    reordered[usize(1)] = rstd::move(first);
    EXPECT_TRUE(store.replace_snapshot(rstd::move(reordered)).is_ok());
    EXPECT_EQ(store.revision(), Some(u64(3)));
    EXPECT_EQ(store.snapshot_view()[usize()].cookie.name(), "b"_str);
    auto equivalent            = store.snapshot_all();
    equivalent[usize()].domain = ".EXAMPLE.TEST"_Str;
    EXPECT_TRUE(store.replace_snapshot(rstd::move(equivalent)).is_ok());
    EXPECT_EQ(store.revision(), Some(u64(3)));
    auto invalid                        = store.snapshot_all();
    invalid[usize()].expiry_nanoseconds = 1000000000U;
    EXPECT_TRUE(store.replace_snapshot(rstd::move(invalid)).is_err());
    EXPECT_EQ(store.revision(), Some(u64(3)));
}
TEST(CookieStorage, RevisionTracksExpirationAndCapacityEviction) {
    CookieOptions options;
    options.max_entries = usize(2);
    CookieStorage store(rstd::move(options));
    auto          url    = Url::parse_http("https://example.test/"_str).unwrap();
    auto          accept = [&](ref<str> text, i64 now = i64(100)) {
        return store.store(SetCookie::parse(text).unwrap(), url, now);
    };
    ASSERT_TRUE(accept("a=one; Max-Age=1"_str));
    ASSERT_TRUE(accept("b=two"_str));
    EXPECT_EQ(store.select(url, i64(101)).encode().as_str(), "b=two"_str);
    EXPECT_EQ(store.snapshot_all().len(), usize(2));
    EXPECT_EQ(store.revision(), Some(u64(2)));
    ASSERT_TRUE(accept("c=three"_str, i64(101)));
    EXPECT_EQ(store.revision(), Some(u64(3)));
    EXPECT_EQ(store.snapshot_view().len(), usize(2));
    ASSERT_TRUE(accept("d=four"_str, i64(101)));
    EXPECT_EQ(store.revision(), Some(u64(4)));
    EXPECT_EQ(store.snapshot_view()[usize()].cookie.name(), "c"_str);
    ASSERT_TRUE(accept("c=gone; Max-Age=0"_str));
    EXPECT_EQ(store.revision(), Some(u64(5)));
    ASSERT_TRUE(accept("c=gone; Max-Age=0"_str));
    EXPECT_EQ(store.revision(), Some(u64(5)));
    ASSERT_TRUE(accept("d=four; Max-Age=1"_str));
    EXPECT_EQ(store.snapshot(i64(101)).len(), usize());
    EXPECT_EQ(store.revision(), Some(u64(7)));
    store.expire(i64(101));
    EXPECT_EQ(store.revision(), Some(u64(7)));
    ASSERT_TRUE(accept("a=one; Max-Age=1"_str));
    (void)store.export_netscape(i64(101));
    EXPECT_EQ(store.revision(), Some(u64(9)));
    ASSERT_TRUE(accept("a=one; Max-Age=1"_str));
    store.expire(i64(101));
    EXPECT_EQ(store.revision(), Some(u64(11)));
}
TEST(CookieStorage, RevisionIgnoresDisabledAndRejectedInsertions) {
    CookieOptions options;
    options.max_entries = usize();
    CookieStorage disabled(rstd::move(options));
    auto          url = Url::parse_http("https://example.test/"_str).unwrap();
    EXPECT_TRUE(disabled.store(SetCookie::parse("sid=one"_str).unwrap(), url, i64(100)));
    EXPECT_TRUE(disabled.replace_snapshot({}).is_ok());
    EXPECT_EQ(disabled.revision(), Some(u64()));
    CookieOptions limited;
    limited.max_cookie_bytes = usize(1);
    CookieStorage store(rstd::move(limited));
    EXPECT_FALSE(store.store(SetCookie::parse("sid=one"_str).unwrap(), url, i64(100)));
    EXPECT_EQ(store.revision(), Some(u64()));
}
TEST(CookieStorage, RevisionTracksRetainedDeletionOnlyOnce) {
    CookieOptions options;
    options.retain_expired_replacements = true;
    CookieStorage store(rstd::move(options));
    auto          url = Url::parse_http("https://example.test/"_str).unwrap();
    ASSERT_TRUE(store.store(SetCookie::parse("sid=one"_str).unwrap(), url, i64(100)));
    auto deleted = SetCookie::parse("sid=gone; Max-Age=0"_str).unwrap();
    EXPECT_TRUE(store.store(deleted, url, i64(100)));
    EXPECT_EQ(store.revision(), Some(u64(2)));
    EXPECT_TRUE(store.store(deleted, url, i64(100)));
    EXPECT_TRUE(
        store.store(SetCookie::parse("absent=gone; Max-Age=0"_str).unwrap(), url, i64(100)));
    EXPECT_EQ(store.revision(), Some(u64(2)));
    EXPECT_EQ(store.snapshot_view()[usize()].cookie.value(), "one"_str);
    auto precise                        = store.snapshot_all();
    precise[usize()].expiry_nanoseconds = 1;
    ASSERT_TRUE(store.replace_snapshot(rstd::move(precise)).is_ok());
    EXPECT_TRUE(store.store(deleted, url, i64(100)));
    EXPECT_EQ(store.revision(), Some(u64(4)));
    EXPECT_EQ(store.snapshot_view()[usize()].expiry_nanoseconds, 0U);
}
TEST(CookieStorage, RevisionComparesFinalImportedContent) {
    CookieStorage store;
    auto          file = "example.test\tFALSE\t/\tFALSE\t0\tsid\tone\n"_str;
    ASSERT_TRUE(store.import_netscape(file, i64(100)).is_ok());
    EXPECT_EQ(store.revision(), Some(u64(1)));
    EXPECT_TRUE(store.import_netscape(file, i64(100)).is_ok());
    EXPECT_TRUE(store.import_netscape("# empty\n"_str, i64(100)).is_ok());
    EXPECT_TRUE(store
                    .import_netscape("example.test\tFALSE\t/\tFALSE\t0\tsid\ttemporary\n"
                                     "example.test\tFALSE\t/\tFALSE\t0\tsid\tone\n"_str,
                                     i64(100))
                    .is_ok());
    EXPECT_EQ(store.revision(), Some(u64(1)));
    EXPECT_TRUE(
        store.import_netscape("example.test\tFALSE\t/\tFALSE\t0\tsid\ttwo\ninvalid\n"_str, i64(100))
            .is_err());
    EXPECT_EQ(store.revision(), Some(u64(1)));
    EXPECT_TRUE(store.import_netscape("example.test\tFALSE\t/\tFALSE\t0\tsid\ttwo\n"_str, i64(100))
                    .is_ok());
    EXPECT_EQ(store.revision(), Some(u64(2)));
    EXPECT_TRUE(store.import_netscape("example.test\tFALSE\t/\tFALSE\t99\tsid\ttwo\n"_str, i64(100))
                    .is_ok());
    EXPECT_EQ(store.revision(), Some(u64(3)));
    EXPECT_TRUE(store.snapshot_view().is_empty());
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
