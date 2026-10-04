#include <rstd/test/gtest.hpp>
#include <rstd/enum.hpp>
#include <string>
#include <string_view>
import lihttpto;
import lihttpto.parser;
import rstd.cppstd;
using namespace rstd::prelude;
using namespace rstd::literals;

auto as_rstd_str(std::string_view value) -> rstd::ref<rstd::str> {
    return rstd::cppstd::as_str(value).unwrap();
}

TEST(HttpValues, MessageParserByteLimits) {
    auto head  = "HTTP/1.1 200 OK\r\nX: a\r\n\r\n"_bytes;
    auto exact = lihttpto::Http1HeadParser { true, head.len() };
    EXPECT_TRUE(exact.push(head)->is_Complete());
    auto short_head = lihttpto::Http1HeadParser { true, head.len() - usize(1) };
    auto rejected   = short_head.push(head);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_TRUE(rejected.unwrap_err().kind().is_HeaderTooLarge());
    auto split = lihttpto::Http1HeadParser { true, head.len() - usize(1) };
    EXPECT_TRUE(split.push("HTTP/1.1 200 OK\r\n"_bytes)->is_NeedMore());
    EXPECT_TRUE(split.push("X: a\r\n\r\n"_bytes).is_err());
    auto zero = lihttpto::Http1HeadParser { true, usize() };
    EXPECT_TRUE(zero.push(head).is_err());

    auto fields       = "X: a\r\n\r\n"_bytes;
    auto exact_fields = lihttpto::Http1FieldSectionParser { fields.len() };
    EXPECT_TRUE(exact_fields.push(fields)->is_Complete());
    auto short_fields    = lihttpto::Http1FieldSectionParser { fields.len() - usize(1) };
    auto rejected_fields = short_fields.push(fields);
    ASSERT_TRUE(rejected_fields.is_err());
    EXPECT_TRUE(rejected_fields.unwrap_err().kind().is_HeaderTooLarge());
}

TEST(HttpValues, HttpOriginSerialization) {
    auto url    = lihttpto::Url::parse_http("HTTPS://Example.COM:443/path?q=x"_str).unwrap();
    auto origin = url.http_origin().unwrap();
    EXPECT_EQ(origin.authority().as_str(), "example.com"_str);
    EXPECT_EQ(origin.serialize().as_str(), "https://example.com"_str);
    auto same = lihttpto::Url::parse_http("https://example.com/other"_str).unwrap();
    EXPECT_TRUE(url.same_http_origin(same));
    auto v6 = lihttpto::Url::parse_http("http://[::1]:8080"_str).unwrap().http_origin().unwrap();
    EXPECT_EQ(v6.authority().as_str(), "[::1]:8080"_str);
    EXPECT_EQ(v6.serialize().as_str(), "http://[::1]:8080"_str);
    EXPECT_TRUE(lihttpto::Url::parse("/relative"_str).is_err());
    EXPECT_TRUE(lihttpto::Url::parse("data:text/plain,hello"_str).unwrap().http_origin().is_none());
}

TEST(HttpValues, UrlEncoding) {
    using namespace lihttpto;

    EXPECT_EQ(rstd::cppstd::as_string_view(encode_component("a b/+~"_str).as_str()),
              "a%20b%2F%2B~");

    auto decoded = decode_component("a%20b%2Fb+plus"_str);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(decoded.unwrap().as_str()), "a b/b+plus");

    auto invalid = decode_component("a%20b%ZZ"_str);
    ASSERT_TRUE(invalid.is_err());
    EXPECT_TRUE(invalid.unwrap_err().kind().is_InvalidPercentEncoding());
    EXPECT_EQ(invalid.unwrap_err().offset().to_primitive(), 6u);

    auto form = decode_form_component("a+b"_str);
    ASSERT_TRUE(form.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(form.unwrap().as_str()), "a b");
}

TEST(HttpValues, QueryParamsPreserveOrderedRepeatedValues) {
    using lihttpto::QueryParams;

    auto parsed = QueryParams::parse_form("first=one&repeat=a&empty=&repeat=b+c"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto query = rstd::move(parsed).unwrap();

    EXPECT_EQ(query.len().to_primitive(), 4u);
    EXPECT_EQ(rstd::cppstd::as_string_view(*query.get("first"_str)), "one");

    auto values = query.values("repeat"_str);
    auto first  = values.next();
    auto second = values.next();
    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*first), "a");
    EXPECT_EQ(rstd::cppstd::as_string_view(*second), "b c");
    EXPECT_TRUE(values.next().is_none());

    EXPECT_EQ(rstd::cppstd::as_string_view(query.encode_form().as_str()),
              "first=one&repeat=a&empty=&repeat=b+c");

    query.set("repeat"_str, "replacement"_str);
    EXPECT_EQ(rstd::cppstd::as_string_view(*query.get("repeat"_str)), "replacement");
    EXPECT_EQ(query.values("repeat"_str).next()->size().to_primitive(), 11u);

    auto invalid_utf8 = QueryParams::parse_form("key=%FF"_str);
    ASSERT_TRUE(invalid_utf8.is_err());
    EXPECT_TRUE(invalid_utf8.unwrap_err().kind().is_InvalidUtf8());
    EXPECT_EQ(invalid_utf8.unwrap_err().offset().to_primitive(), 4u);

    auto from_trait = rstd::from_str<QueryParams>("a=1&a=2"_str);
    ASSERT_TRUE(from_trait.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", from_trait.unwrap())), "a=1&a=2");

    auto raw = QueryParams::parse_query("value=a+b&space=a%20b"_str);
    ASSERT_TRUE(raw.is_ok());
    auto raw_query = rstd::move(raw).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(*raw_query.get("value"_str)), "a+b");
    EXPECT_EQ(rstd::cppstd::as_string_view(*raw_query.get("space"_str)), "a b");
    EXPECT_EQ(rstd::cppstd::as_string_view(raw_query.encode_query().as_str()),
              "value=a%2Bb&space=a%20b");

    auto raw_trait = rstd::from_str<QueryParams>("value=a+b"_str);
    ASSERT_TRUE(raw_trait.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", raw_trait.unwrap())), "value=a%2Bb");
}

TEST(HttpValues, CookieValuesParseAttributesAndPreserveOrder) {
    using namespace lihttpto;

    auto pair = rstd::from_str<Cookie>("session=\"abc123\""_str);
    ASSERT_TRUE(pair.is_ok());
    auto cookie = rstd::move(pair).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(cookie.name()), "session");
    EXPECT_EQ(rstd::cppstd::as_string_view(cookie.value()), "abc123");
    EXPECT_TRUE(cookie.is_quoted());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", cookie)), "session=\"abc123\"");

    auto header = CookieHeader::parse(" \tfirst=one; repeat=a; repeat=\"b\" \t"_str);
    ASSERT_TRUE(header.is_ok());
    auto cookies = rstd::move(header).unwrap();
    EXPECT_EQ(cookies.len().to_primitive(), 3u);
    auto repeat = cookies.get("repeat"_str);
    ASSERT_TRUE(repeat.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view((**repeat).value()), "a");
    EXPECT_TRUE(cookies.get("Repeat"_str).is_none());
    EXPECT_EQ(rstd::cppstd::as_string_view(cookies.encode().as_str()),
              "first=one; repeat=a; repeat=\"b\"");

    auto set =
        SetCookie::parse("session=abc123; Path=/account; Secure; HttpOnly; SameSite=Lax"_str);
    ASSERT_TRUE(set.is_ok());
    auto set_cookie = rstd::move(set).unwrap();
    EXPECT_TRUE(set_cookie.secure());
    EXPECT_TRUE(set_cookie.http_only());
    auto path = set_cookie.attribute("path"_str);
    ASSERT_TRUE(path.is_some());
    ASSERT_TRUE((**path).value().is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**path).value()), "/account");
    EXPECT_EQ(set_cookie.attributes().count().to_primitive(), 4u);
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", set_cookie)),
              "session=abc123; Path=/account; Secure; HttpOnly; SameSite=Lax");

    auto invalid_name = Cookie::parse("bad name=value"_str);
    ASSERT_TRUE(invalid_name.is_err());
    EXPECT_TRUE(invalid_name.unwrap_err().kind().is_InvalidName());
    EXPECT_EQ(invalid_name.unwrap_err().offset().to_primitive(), 3u);

    auto invalid_value = Cookie::parse("name=a,b"_str);
    ASSERT_TRUE(invalid_value.is_err());
    EXPECT_TRUE(invalid_value.unwrap_err().kind().is_InvalidValue());
    EXPECT_EQ(invalid_value.unwrap_err().offset().to_primitive(), 6u);

    auto trailing = CookieHeader::parse("a=1;"_str);
    ASSERT_TRUE(trailing.is_err());
    EXPECT_TRUE(trailing.unwrap_err().kind().is_EmptyName());
    EXPECT_EQ(trailing.unwrap_err().offset().to_primitive(), 4u);
}

TEST(HttpValues, ParserCursorCompositionAndErrors) {
    namespace parser = lihttpto::parser;

    auto cursor = parser::Cursor { "alpha"_str };
    auto prefix = parser::take_literal(cursor, "alp"_str);
    ASSERT_TRUE(prefix.is_ok());
    EXPECT_EQ(prefix.unwrap().begin.to_primitive(), 0u);
    EXPECT_EQ(prefix.unwrap().end.to_primitive(), 3u);
    EXPECT_EQ(cursor.offset().to_primitive(), 3u);
    auto prefix_bytes = cursor.slice(parser::Span { usize(), usize(3) });
    auto prefix_text  = rstd::str_::from_utf8_unchecked(prefix_bytes);
    EXPECT_EQ(rstd::cppstd::as_string_view(prefix_text), "alp");

    auto incomplete = parser::take_literal(cursor, "habet"_str);
    ASSERT_TRUE(incomplete.is_err());
    auto incomplete_error = rstd::move(incomplete).unwrap_err();
    EXPECT_TRUE(incomplete_error.is_incomplete());
    EXPECT_FALSE(incomplete_error.is_committed());
    EXPECT_EQ(incomplete_error.offset().to_primitive(), 5u);
    EXPECT_EQ(cursor.offset().to_primitive(), 3u);

    auto uncommitted_cursor = parser::Cursor { "ac"_str };
    auto uncommitted        = parser::choice(
        uncommitted_cursor,
        [](parser::Cursor& input) -> parser::ParseResult<parser::Span> {
            auto begin = input.mark();
            auto first = parser::take_byte(input, u8('a'));
            if (first.is_err()) return rstd::Err(rstd::move(first).unwrap_err());
            auto second = parser::take_byte(input, u8('b'));
            if (second.is_err()) return rstd::Err(rstd::move(second).unwrap_err());
            return rstd::Ok(input.span_from(begin));
        },
        [](parser::Cursor& input) {
            return parser::take_literal(input, "ac"_str);
        });
    ASSERT_TRUE(uncommitted.is_ok());
    EXPECT_EQ(uncommitted_cursor.offset().to_primitive(), 2u);

    auto committed_cursor = parser::Cursor { "ac"_str };
    auto committed        = parser::choice(
        committed_cursor,
        [](parser::Cursor& input) -> parser::ParseResult<parser::Span> {
            auto begin = input.mark();
            auto first = parser::take_byte(input, u8('a'));
            if (first.is_err()) return rstd::Err(rstd::move(first).unwrap_err());
            auto second = parser::committed(parser::take_byte(input, u8('b')));
            if (second.is_err()) return rstd::Err(rstd::move(second).unwrap_err());
            return rstd::Ok(input.span_from(begin));
        },
        [](parser::Cursor& input) {
            return parser::take_literal(input, "ac"_str);
        });
    ASSERT_TRUE(committed.is_err());
    auto committed_error = rstd::move(committed).unwrap_err();
    EXPECT_TRUE(committed_error.is_committed());
    EXPECT_EQ(committed_error.offset().to_primitive(), 1u);
    EXPECT_EQ(committed_cursor.offset().to_primitive(), 1u);

    auto attempted_cursor = parser::Cursor { "ac"_str };
    auto attempted        = parser::attempt(
        attempted_cursor, [](parser::Cursor& input) -> parser::ParseResult<parser::Span> {
            auto begin = input.mark();
            auto first = parser::take_byte(input, u8('a'));
            if (first.is_err()) return rstd::Err(rstd::move(first).unwrap_err());
            auto second = parser::take_byte(input, u8('b'));
            if (second.is_err()) return rstd::Err(rstd::move(second).unwrap_err());
            return rstd::Ok(input.span_from(begin));
        });
    ASSERT_TRUE(attempted.is_err());
    EXPECT_EQ(attempted_cursor.offset().to_primitive(), 0u);

    auto optional_cursor = parser::Cursor { "?value"_str };
    auto present         = parser::optional(optional_cursor, [](parser::Cursor& input) {
        return parser::take_byte(input, u8('?'));
    });
    ASSERT_TRUE(present.is_ok());
    EXPECT_TRUE(present.unwrap().is_some());
    EXPECT_EQ(optional_cursor.offset().to_primitive(), 1u);
    auto absent = parser::optional(optional_cursor, [](parser::Cursor& input) {
        return parser::take_byte(input, u8('#'));
    });
    ASSERT_TRUE(absent.is_ok());
    EXPECT_TRUE(absent.unwrap().is_none());
    EXPECT_EQ(optional_cursor.offset().to_primitive(), 1u);

    auto partial_cursor = parser::Cursor { "aX"_str };
    auto partial        = parser::optional(partial_cursor, [](parser::Cursor& input) {
        return parser::take_literal(input, "ab"_str);
    });
    ASSERT_TRUE(partial.is_err());
    EXPECT_EQ(partial.unwrap_err().offset().to_primitive(), 1u);
    EXPECT_EQ(partial_cursor.offset().to_primitive(), 0u);

    auto sequence_cursor = parser::Cursor { "\r\nrest"_str };
    auto sequenced       = parser::sequence(
        sequence_cursor,
        [](parser::Cursor& input) {
            return parser::take_byte(input, u8('\r'));
        },
        [](parser::Cursor& input) {
            return parser::take_byte(input, u8('\n'));
        });
    ASSERT_TRUE(sequenced.is_ok());
    EXPECT_EQ(sequenced.unwrap().size().to_primitive(), 2u);

    auto repeat_cursor = parser::Cursor { "///path"_str };
    auto repeated      = parser::repeat(
        repeat_cursor,
        [](parser::Cursor& input) {
            return parser::take_byte(input, u8('/'));
        },
        usize(1),
        usize(4));
    ASSERT_TRUE(repeated.is_ok());
    EXPECT_EQ(repeated.unwrap().size().to_primitive(), 3u);
    EXPECT_EQ(repeat_cursor.offset().to_primitive(), 3u);

    auto delimited_cursor = parser::Cursor { "[ok]"_str };
    auto delimited        = parser::delimited(
        delimited_cursor,
        [](parser::Cursor& input) {
            return parser::take_byte(input, u8('['));
        },
        [](parser::Cursor& input) {
            return parser::take_literal(input, "ok"_str);
        },
        [](parser::Cursor& input) {
            return parser::committed(parser::take_byte(input, u8(']')));
        });
    ASSERT_TRUE(delimited.is_ok());
    EXPECT_EQ(delimited.unwrap().size().to_primitive(), 2u);
    EXPECT_EQ(delimited_cursor.offset().to_primitive(), 4u);

    auto unclosed_cursor = parser::Cursor { "[ok"_str };
    auto unclosed        = parser::delimited(
        unclosed_cursor,
        [](parser::Cursor& input) {
            return parser::take_byte(input, u8('['));
        },
        [](parser::Cursor& input) {
            return parser::take_literal(input, "ok"_str);
        },
        [](parser::Cursor& input) {
            return parser::committed(parser::take_byte(input, u8(']')));
        });
    ASSERT_TRUE(unclosed.is_err());
    EXPECT_TRUE(unclosed.unwrap_err().is_committed());
}

TEST(HttpValues, UrlParsesOwnedComponentsAndTraits) {
    using lihttpto::Url;

    auto parsed = Url::parse("foo://user@example.com:8042/over/there?name=ferret#nose"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto url = rstd::move(parsed).unwrap();

    EXPECT_EQ(rstd::cppstd::as_string_view(*url.scheme()), "foo");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.authority()), "user@example.com:8042");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.userinfo()), "user");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.host()), "example.com");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.port()), "8042");
    EXPECT_EQ(rstd::cppstd::as_string_view(url.path()), "/over/there");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.query()), "name=ferret");
    EXPECT_EQ(rstd::cppstd::as_string_view(*url.fragment()), "nose");
    EXPECT_EQ(rstd::cppstd::to_string(url.request_target()), "/over/there?name=ferret");

    auto cloned = rstd::as<rstd::clone::Clone>(url).clone();
    EXPECT_EQ(rstd::cppstd::as_string_view(cloned.as_ref()),
              "foo://user@example.com:8042/over/there?name=ferret#nose");
    EXPECT_EQ(
        rstd::cppstd::as_string_view(rstd::as<rstd::convert::AsRef<rstd::str>>(cloned).as_ref()),
        rstd::cppstd::as_string_view(cloned.as_ref()));
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", cloned)),
              "foo://user@example.com:8042/over/there?name=ferret#nose");

    EXPECT_TRUE(rstd::from_str<Url>("../relative?"_str).is_err());
    auto from_base = Url::parse("../relative?"_str, url);
    ASSERT_TRUE(from_base.is_ok());
    auto relative = rstd::move(from_base).unwrap();
    ASSERT_TRUE(relative.query().is_some());
    EXPECT_EQ(relative.query()->size().to_primitive(), 0u);
    EXPECT_TRUE(relative.fragment().is_none());

    auto empty_fragment = relative.resolve("#"_str);
    ASSERT_TRUE(empty_fragment.is_ok());
    auto empty_fragment_url = rstd::move(empty_fragment).unwrap();
    EXPECT_TRUE(empty_fragment_url.query().is_some());
    ASSERT_TRUE(empty_fragment_url.fragment().is_some());
    EXPECT_EQ(empty_fragment_url.fragment()->size().to_primitive(), 0u);

    auto encoded_path = Url::parse("http://example.com/a%2Fb"_str);
    ASSERT_TRUE(encoded_path.is_ok());
    auto encoded_path_url = rstd::move(encoded_path).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(encoded_path_url.path()), "/a%2Fb");
    EXPECT_EQ(rstd::cppstd::to_string(encoded_path_url.request_target()), "/a%2Fb");
}

TEST(HttpValues, HttpUrlValidationReportsKindsAndOffsets) {
    using lihttpto::Url;

    auto invalid_percent = Url::parse("http://example.com/%zz"_str);
    ASSERT_TRUE(invalid_percent.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(invalid_percent->path()), "/%zz");

    auto invalid_character = Url::parse("http://example.com/a b"_str);
    ASSERT_TRUE(invalid_character.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(invalid_character->path()), "/a%20b");

    auto missing_scheme = Url::parse_http("//example.com/path"_str);
    ASSERT_TRUE(missing_scheme.is_err());
    EXPECT_TRUE(missing_scheme.unwrap_err().kind().is_MissingScheme());

    auto unsupported = Url::parse_http("ftp://example.com/path"_str);
    ASSERT_TRUE(unsupported.is_err());
    EXPECT_TRUE(unsupported.unwrap_err().kind().is_UnsupportedScheme());

    auto implicit_authority = Url::parse_http("http:path"_str);
    ASSERT_TRUE(implicit_authority.is_ok());
    EXPECT_EQ(rstd::cppstd::as_string_view(implicit_authority->as_ref()), "http://path/");

    auto missing_host = Url::parse_http("http://?query"_str);
    ASSERT_TRUE(missing_host.is_err());
    EXPECT_TRUE(missing_host.unwrap_err().kind().is_MissingHost());

    auto request = Url::parse_http("https://[2001:db8::1]/resource?#fragment"_str);
    if (request.is_err()) {
        auto error = rstd::move(request).unwrap_err();
        FAIL() << "URL error kind " << error.kind().index() << " at "
               << error.offset().to_primitive();
    }
    auto value = rstd::move(request).unwrap();
    EXPECT_EQ(rstd::cppstd::as_string_view(value.as_ref()),
              "https://[2001:db8::1]/resource?#fragment");
    EXPECT_EQ(rstd::cppstd::to_string(value.request_target()), "/resource?");
}

TEST(HttpValues, UrlResolvesReferencesAgainstBase) {
    using lihttpto::Url;

    auto base_result = Url::parse("http://a/b/c/d;p?q"_str);
    ASSERT_TRUE(base_result.is_ok());
    auto base = rstd::move(base_result).unwrap();

    struct Example {
        const char* reference;
        const char* expected;
    };
    constexpr Example examples[] = {
        { "g:h", "g:h" },
        { "g", "http://a/b/c/g" },
        { "./g", "http://a/b/c/g" },
        { "g/", "http://a/b/c/g/" },
        { "/g", "http://a/g" },
        { "//g", "http://g/" },
        { "?y", "http://a/b/c/d;p?y" },
        { "g?y", "http://a/b/c/g?y" },
        { "#s", "http://a/b/c/d;p?q#s" },
        { "g#s", "http://a/b/c/g#s" },
        { "g?y#s", "http://a/b/c/g?y#s" },
        { ";x", "http://a/b/c/;x" },
        { "g;x", "http://a/b/c/g;x" },
        { "g;x?y#s", "http://a/b/c/g;x?y#s" },
        { "", "http://a/b/c/d;p?q" },
        { ".", "http://a/b/c/" },
        { "./", "http://a/b/c/" },
        { "..", "http://a/b/" },
        { "../", "http://a/b/" },
        { "../g", "http://a/b/g" },
        { "../..", "http://a/" },
        { "../../", "http://a/" },
        { "../../g", "http://a/g" },
        { "../../../g", "http://a/g" },
        { "../../../../g", "http://a/g" },
        { "/./g", "http://a/g" },
        { "/../g", "http://a/g" },
        { "g.", "http://a/b/c/g." },
        { ".g", "http://a/b/c/.g" },
        { "g..", "http://a/b/c/g.." },
        { "..g", "http://a/b/c/..g" },
        { "./../g", "http://a/b/g" },
        { "./g/.", "http://a/b/c/g/" },
        { "g/./h", "http://a/b/c/g/h" },
        { "g/../h", "http://a/b/c/h" },
        { "g;x=1/./y", "http://a/b/c/g;x=1/y" },
        { "g;x=1/../y", "http://a/b/c/y" },
        { "g?y/./x", "http://a/b/c/g?y/./x" },
        { "g?y/../x", "http://a/b/c/g?y/../x" },
        { "g#s/./x", "http://a/b/c/g#s/./x" },
        { "g#s/../x", "http://a/b/c/g#s/../x" },
        { "http:g", "http://a/b/c/g" },
    };

    for (auto const& example : examples) {
        auto resolved = base.resolve(as_rstd_str(example.reference));
        ASSERT_TRUE(resolved.is_ok()) << example.reference;
        EXPECT_EQ(rstd::cppstd::as_string_view(resolved.unwrap().as_ref()), example.expected)
            << example.reference;
    }
}

TEST(HttpValues, UrlValidatesIpLiterals) {
    using lihttpto::Url;

    constexpr const char* valid[] = {
        "http://[::]/",
        "http://[::1]/",
        "http://[2001:db8::1]/",
        "http://[1:2:3:4:5:6:7:8]/",
        "http://[::ffff:192.0.2.1]/",
    };
    for (auto value : valid) {
        auto parsed = Url::parse_http(as_rstd_str(value));
        EXPECT_TRUE(parsed.is_ok()) << value;
    }

    constexpr const char* invalid[] = {
        "http://[:]/",       "http://[1:2:3:4:5:6:7]/",    "http://[1:2:3:4:5:6:7:8:9]/",
        "http://[1::2::3]/", "http://[::ffff:999.0.2.1]/", "http://[v.fe80]/",
        "http://[v1.]/",     "http://[v1.fe80::a]/",
    };
    for (auto value : invalid) {
        auto parsed = Url::parse_http(as_rstd_str(value));
        ASSERT_TRUE(parsed.is_err()) << value;
        EXPECT_TRUE(parsed.unwrap_err().kind().is_InvalidIpAddress()) << value;
    }

    auto invalid_ipv4 = Url::parse_http("http://999.0.2.1/"_str);
    ASSERT_TRUE(invalid_ipv4.is_err());
    EXPECT_TRUE(invalid_ipv4.unwrap_err().kind().is_InvalidIpAddress());
    EXPECT_EQ(invalid_ipv4.unwrap_err().offset().to_primitive(), 7u);

    auto invalid_port = Url::parse_http("http://example.com:65536/"_str);
    ASSERT_TRUE(invalid_port.is_err());
    EXPECT_TRUE(invalid_port.unwrap_err().kind().is_InvalidPort());
    EXPECT_EQ(invalid_port.unwrap_err().offset().to_primitive(), 23u);
}

TEST(HttpValues, UrlNormalizesRecordAndSerializesIdempotently) {
    using lihttpto::Url;
    struct Example {
        ref<str> input;
        ref<str> expected;
    };
    Example examples[] = {
        { " \tHtTpS://EXAM\nPLE.com:00443/a/%2E/b/../c d?x='y'#f g\r"_str,
          "https://example.com/a/c%20d?x=%27y%27#f%20g"_str },
        { "http:\\\\0x7f.1\\a\\..\\b"_str, "http://127.0.0.1/b"_str },
        { "https://[0:0:0:0:0:ffff:192.0.2.1]:443"_str, "https://[::ffff:c000:201]/"_str },
        { "http://u@a:p:q@EXAMPLE.com"_str, "http://u%40a:p%3Aq@example.com/"_str },
        { "https://:@example.com/a%zz/中文"_str,
          "https://example.com/a%zz/%E4%B8%AD%E6%96%87"_str },
        { "custom://EXAMPLE/a^b?q='x'#x`y"_str, "custom://EXAMPLE/a%5Eb?q='x'#x%60y"_str },
        { "data:hello world ?x#f"_str, "data:hello world%20?x#f"_str },
    };
    for (const auto& example : examples) {
        auto parsed = Url::parse(example.input);
        ASSERT_TRUE(parsed.is_ok());
        EXPECT_EQ(parsed->as_ref(), example.expected);
        auto reparsed = Url::parse(parsed->as_ref());
        ASSERT_TRUE(reparsed.is_ok());
        EXPECT_EQ(reparsed->as_ref(), parsed->as_ref());
        EXPECT_EQ(parsed->clone().as_ref(), parsed->as_ref());
    }
    auto credentials = Url::parse("https://user:pass@example.com/"_str).unwrap();
    EXPECT_TRUE(credentials.port().is_none());
    EXPECT_EQ(*credentials.effective_port(), u16(443));
    EXPECT_EQ(*Url::parse("http://example.com:0/"_str)->effective_port(), u16());
    EXPECT_TRUE(Url::parse("custom://host/"_str)->effective_port().is_none());
    EXPECT_EQ(credentials.username(), "user"_str);
    EXPECT_EQ(credentials.password(), "pass"_str);
    auto port_error = Url::parse("\thttps://a:\n65536"_str).unwrap_err();
    EXPECT_TRUE(port_error.kind().is_InvalidPort());
    EXPECT_EQ(port_error.offset(), usize(16));
}

TEST(HttpValues, UrlBaseAndOriginsUseParsedRecord) {
    using lihttpto::Url;
    auto base = Url::parse("https://user:pass@EXAMPLE.com:443/a/b?q#old"_str).unwrap();
    EXPECT_EQ(base.resolve("https:../c"_str)->as_ref(), "https://user:pass@example.com/c"_str);
    EXPECT_EQ(base.resolve("\\\\other.invalid\\x"_str)->as_ref(), "https://other.invalid/x"_str);
    EXPECT_EQ(base.resolve("?"_str)->as_ref(), "https://user:pass@example.com/a/b?"_str);
    EXPECT_EQ(base.resolve(""_str)->as_ref(), "https://user:pass@example.com/a/b?q"_str);
    EXPECT_EQ(base.as_ref(), "https://user:pass@example.com/a/b?q#old"_str);
    auto opaque = Url::parse("data:text/plain,hello?x#old"_str).unwrap();
    EXPECT_EQ(opaque.resolve("#new"_str)->as_ref(), "data:text/plain,hello?x#new"_str);
    EXPECT_TRUE(opaque.resolve("next"_str).is_err());
    EXPECT_TRUE(opaque.resolve(""_str).is_err());
    auto ipv4 = Url::parse("http://127.1/"_str).unwrap();
    EXPECT_TRUE(ipv4.same_http_origin(Url::parse("http://0x7f000001:80/"_str).unwrap()));
    EXPECT_FALSE(ipv4.same_http_origin(Url::parse("http://127.0.0.1:81/"_str).unwrap()));
    auto ipv6 = Url::parse("https://[2001:DB8:0:0:0:0:0:1]/"_str).unwrap();
    EXPECT_TRUE(ipv6.same_http_origin(Url::parse("https://[2001:db8::1]:443/"_str).unwrap()));
    EXPECT_TRUE(
        Url::parse_http("file:///tmp/input"_str).unwrap_err().kind().is_UnsupportedScheme());
    EXPECT_TRUE(Url::parse("https://é.example/"_str).unwrap_err().kind().is_UnsupportedHost());
}

TEST(HttpValues, FileUrlHostDriveAndBaseResolution) {
    using lihttpto::Url;
    struct Example {
        ref<str> input;
        ref<str> expected;
    };
    Example examples[] = {
        { "file:"_str, "file:///"_str },
        { "file://LOCALhost/etc/a/../b"_str, "file:///etc/b"_str },
        { "file://C|/a"_str, "file:///C:/a"_str },
        { "file:C|\\a\\..\\b"_str, "file:///C:/b"_str },
        { "file://server/share/../asset"_str, "file://server/asset"_str },
        { "file://[0:0:0:0:0:0:0:1]/a"_str, "file://[::1]/a"_str },
        { "file:///C:/../../x"_str, "file:///C:/x"_str },
        { "file:////a"_str, "file:////a"_str },
    };
    for (const auto& example : examples) {
        auto parsed = Url::parse(example.input);
        ASSERT_TRUE(parsed.is_ok());
        EXPECT_EQ(parsed->as_ref(), example.expected);
        EXPECT_EQ(Url::parse(parsed->as_ref())->as_ref(), example.expected);
        EXPECT_TRUE(parsed->effective_port().is_none());
        EXPECT_TRUE(parsed->origin()->is_opaque());
    }
    auto    base         = Url::parse("file://server/C:/one/two?q#old"_str).unwrap();
    Example references[] = {
        { "../three"_str, "file://server/C:/three"_str },
        { "/three"_str, "file://server/C:/three"_str },
        { "/"_str, "file://server/C:/"_str },
        { "/D|/three"_str, "file://server/D:/three"_str },
        { "D|/three"_str, "file://server/D:/three"_str },
        { "//localhost/tmp"_str, "file:///tmp"_str },
        { "//D:/three"_str, "file:///D:/three"_str },
        { "file:../three"_str, "file://server/C:/three"_str },
        { "#new"_str, "file://server/C:/one/two?q#new"_str },
        { "?"_str, "file://server/C:/one/two?"_str },
        { ""_str, "file://server/C:/one/two?q"_str },
    };
    for (const auto& example : references) {
        auto resolved = base.resolve(example.input);
        ASSERT_TRUE(resolved.is_ok());
        EXPECT_EQ(resolved->as_ref(), example.expected);
    }
    EXPECT_EQ(base.as_ref(), "file://server/C:/one/two?q#old"_str);
    for (auto input : { "file://user@host/a"_str,
                        "file://host:80/a"_str,
                        "file://host:/a"_str,
                        "file://[::1]:80/a"_str })
        EXPECT_TRUE(Url::parse(input).is_err());
}

TEST(HttpValues, UrlOriginDistinguishesTupleAndOpaqueIdentity) {
    using lihttpto::Url;
    for (auto scheme : { "http"_str, "https"_str, "ftp"_str, "ws"_str, "wss"_str }) {
        auto url    = Url::parse(rstd::format("{}://EXAMPLE.com/a", scheme).as_str()).unwrap();
        auto origin = url.origin().unwrap();
        ASSERT_FALSE(origin.is_opaque());
        EXPECT_EQ(origin.tuple()->get().scheme.as_str(), scheme);
        EXPECT_EQ(origin.tuple()->get().host.as_str(), "example.com"_str);
        EXPECT_TRUE(origin.tuple()->get().port.is_none());
        EXPECT_EQ(origin.serialize().as_str(), rstd::format("{}://example.com", scheme).as_str());
        EXPECT_TRUE(origin.same_origin(origin.clone()));
    }
    auto http       = Url::parse("https://example.com/"_str).unwrap().origin().unwrap();
    auto other_port = Url::parse("https://example.com:444/"_str).unwrap().origin().unwrap();
    EXPECT_FALSE(http.same_origin(other_port));
    EXPECT_EQ(other_port.serialize().as_str(), "https://example.com:444"_str);
    for (auto input : { "data:hello"_str, "file:///a"_str, "custom://host/a"_str }) {
        auto url = Url::parse(input).unwrap();
        auto a   = url.origin().unwrap();
        auto b   = url.origin().unwrap();
        ASSERT_TRUE(a.is_opaque());
        EXPECT_TRUE(a.tuple().is_none());
        EXPECT_EQ(a.serialize().as_str(), "null"_str);
        EXPECT_EQ(b.serialize().as_str(), "null"_str);
        EXPECT_FALSE(a.same_origin(b));
        EXPECT_FALSE(a.same_origin(http));
        EXPECT_TRUE(a.same_origin(a));
        EXPECT_TRUE(a.same_origin(rstd::as<rstd::clone::Clone>(a).clone()));
    }
}

TEST(HttpValues, BlobOriginDoesNotExpandHttpTransportScope) {
    using lihttpto::Url;
    auto http = Url::parse("https://example.com/"_str).unwrap();
    auto blob = Url::parse("blob:https://EXAMPLE.com:443/identifier"_str).unwrap();
    EXPECT_TRUE(blob.origin()->same_origin(http.origin().unwrap()));
    EXPECT_FALSE(blob.same_http_origin(http));
    EXPECT_TRUE(blob.http_origin().is_none());
    EXPECT_TRUE(Url::parse_http(blob.as_ref()).is_err());
    for (auto input : { "blob:blob:https://example.com/"_str,
                        "blob:ftp://example.com/"_str,
                        "blob:ws://example.com/"_str,
                        "blob:file:///x"_str,
                        "blob:invalid"_str,
                        "blob:http%3a//example.com/"_str })
        EXPECT_TRUE(Url::parse(input)->origin()->is_opaque());
    auto unsupported = Url::parse("blob:https://é.example/id"_str).unwrap().origin();
    ASSERT_TRUE(unsupported.is_err());
    EXPECT_TRUE(unsupported.unwrap_err().kind().is_UnsupportedHost());
}

TEST(HttpValues, UriWireParserDoesNotApplyBrowserRepairs) {
    namespace uri = lihttpto::parser::uri;
    for (auto input : { "https://example.com/a b"_str,
                        "https:\\\\example.com/a"_str,
                        "https://exam\nple.com/a"_str,
                        "https://example.com/%zz"_str }) {
        EXPECT_TRUE(lihttpto::Url::parse(input).is_ok());
        EXPECT_TRUE(uri::parse(input).is_err());
    }
    EXPECT_TRUE(uri::parse("../relative?"_str).is_ok());
    EXPECT_TRUE(uri::parse("http://[v1.fe80::a]/"_str).is_ok());
    EXPECT_TRUE(lihttpto::Url::parse("http://[v1.fe80::a]/"_str).is_err());
}

TEST(HttpValues, MessageHeadParsesTypedResponseAndDuplicateFields) {
    using lihttpto::MessageHead;

    auto parsed = MessageHead::parse("HTTP/1.1 200 OK\r\n"
                                     "Set-Cookie: a=1\r\n"
                                     "set-cookie:\tb=2 \t\r\n"
                                     "X-Empty:\r\n"
                                     "\r\n"_bytes);
    ASSERT_TRUE(parsed.is_ok());
    auto head = rstd::move(parsed).unwrap();

    ASSERT_TRUE(head.start().is_Response());
    auto const& status = head.start().as_Response().value;
    EXPECT_EQ(status.status().value().to_primitive(), 200u);
    auto version = status.version();
    ASSERT_TRUE(version.is_some());
    EXPECT_EQ(version->major().to_primitive(), 1u);
    EXPECT_EQ(version->minor().to_primitive(), 1u);
    auto reason = status.reason();
    ASSERT_TRUE(reason.is_some());
    ASSERT_TRUE((**reason).to_str().ok().is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**reason).to_str().ok()), "OK");

    EXPECT_EQ(head.status_code().unwrap().to_primitive(), 200u);
    EXPECT_EQ(head.headers().get_all("set-cookie"_str).len().to_primitive(), 2u);
    EXPECT_TRUE(head.has_field("x-empty"_str));
    auto empty = head.headers().get("X-Empty"_str);
    ASSERT_TRUE(empty.is_some());
    EXPECT_EQ((**empty).as_slice().len().to_primitive(), 0u);

    auto clone = head.clone();
    EXPECT_EQ(clone.status_code().unwrap().to_primitive(), 200u);
    EXPECT_EQ(clone.headers().get_all("SET-COOKIE"_str).len().to_primitive(), 2u);

    auto saw_response = false;
    auto start        = head.start().clone();
    RSTD_MATCH(rstd::move(start)) {
        RSTD_CASE(Request, value) {
            (void)value;
            break;
        }
        RSTD_CASE(Response, value) {
            saw_response = value.status().value() == u16(200);
            break;
        }
    }
    EXPECT_TRUE(saw_response);
}

TEST(HttpValues, Http1HeadParserComposesAcrossArbitraryChunks) {
    auto parser = lihttpto::Http1HeadParser {};
    auto first  = parser.push("HTTP/1.1 204 No"_bytes);
    ASSERT_TRUE(first.is_ok());
    EXPECT_TRUE(first.unwrap().is_NeedMore());

    auto second = parser.push(" Content\r\nX-Test"_bytes);
    ASSERT_TRUE(second.is_ok());
    EXPECT_TRUE(second.unwrap().is_NeedMore());

    auto third = parser.push(": value\r\n\r\n"_bytes);
    ASSERT_TRUE(third.is_ok());
    auto event = rstd::move(third).unwrap();
    ASSERT_TRUE(event.is_Complete());
    auto completed = rstd::move(event).as_Complete();
    auto head      = rstd::move(completed.head);
    EXPECT_EQ(head.status_code().unwrap().to_primitive(), 204u);
    EXPECT_TRUE(head.has_field("x-test"_str));

    auto incomplete = lihttpto::Http1HeadParser {};
    auto partial    = incomplete.push("HTTP/1.1 200 OK\r\nX: value"_bytes);
    ASSERT_TRUE(partial.is_ok());
    EXPECT_TRUE(partial.unwrap().is_NeedMore());
    auto ended = incomplete.finish();
    ASSERT_TRUE(ended.is_err());
    EXPECT_TRUE(ended.unwrap_err().kind().is_UnexpectedEof());
    EXPECT_EQ(ended.unwrap_err().offset().to_primitive(), 25u);

    constexpr auto head_with_body  = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody"_bytes;
    constexpr auto head_size       = sizeof("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n") - 1;
    auto           followed        = lihttpto::Http1HeadParser {};
    auto           followed_result = followed.push(head_with_body);
    ASSERT_TRUE(followed_result.is_ok());
    auto followed_event = rstd::move(followed_result).unwrap();
    ASSERT_TRUE(followed_event.is_Complete());
    auto followed_complete = rstd::move(followed_event).as_Complete();
    EXPECT_EQ(followed_complete.consumed.to_primitive(), head_size);
    EXPECT_EQ(followed_complete.head.status_code().unwrap().to_primitive(), 200u);

    auto exact = lihttpto::MessageHead::parse(head_with_body);
    ASSERT_TRUE(exact.is_err());
    EXPECT_TRUE(exact.unwrap_err().kind().is_InvalidSyntax());
    EXPECT_EQ(exact.unwrap_err().offset().to_primitive(), head_size);

    auto oversized = std::string(lihttpto::Http1HeadParser::MaxHeaderBytes.to_primitive() + 1, 'x');
    auto oversized_parser = lihttpto::Http1HeadParser {};
    auto oversized_result = oversized_parser.push(as_rstd_str(oversized).as_bytes());
    ASSERT_TRUE(oversized_result.is_err());
    EXPECT_TRUE(oversized_result.unwrap_err().kind().is_HeaderTooLarge());
    EXPECT_EQ(oversized_result.unwrap_err().offset().to_primitive(),
              lihttpto::Http1HeadParser::MaxHeaderBytes.to_primitive());

    auto large_body_input = std::string("HTTP/1.1 200 OK\r\n\r\n");
    large_body_input.append(lihttpto::Http1HeadParser::MaxHeaderBytes.to_primitive() + 1, 'x');
    auto large_body_parser = lihttpto::Http1HeadParser {};
    auto large_body_result = large_body_parser.push(as_rstd_str(large_body_input).as_bytes());
    ASSERT_TRUE(large_body_result.is_ok());
    EXPECT_TRUE(large_body_result.unwrap().is_Complete());
}

TEST(HttpValues, Http1FieldSectionParserKeepsTrailersSeparate) {
    auto parser = lihttpto::Http1FieldSectionParser {};
    auto first  = parser.push("Digest: first\r\nX-Tra"_bytes);
    ASSERT_TRUE(first.is_ok());
    EXPECT_TRUE(first.unwrap().is_NeedMore());

    constexpr auto remainder = "iler: second\r\n\r\nbody"_bytes;
    auto           second    = parser.push(remainder);
    ASSERT_TRUE(second.is_ok());
    auto event = rstd::move(second).unwrap();
    ASSERT_TRUE(event.is_Complete());
    auto complete = rstd::move(event).as_Complete();
    EXPECT_EQ(complete.consumed.to_primitive(),
              sizeof("Digest: first\r\nX-Trailer: second\r\n\r\n") - 1);
    EXPECT_EQ(complete.fields.len().to_primitive(), 2u);
    EXPECT_TRUE(complete.fields.contains("digest"_str));
    EXPECT_TRUE(complete.fields.contains("x-trailer"_str));

    auto initial =
        lihttpto::MessageHead::parse("HTTP/1.1 200 OK\r\nX-Initial: value\r\n\r\n"_bytes);
    ASSERT_TRUE(initial.is_ok());
    EXPECT_TRUE(initial.unwrap().headers().contains("x-initial"_str));
    EXPECT_FALSE(initial.unwrap().headers().contains("x-trailer"_str));

    auto incomplete = lihttpto::Http1FieldSectionParser {};
    auto partial    = incomplete.push("X-Trailer: value\r\n"_bytes);
    ASSERT_TRUE(partial.is_ok());
    EXPECT_TRUE(partial.unwrap().is_NeedMore());
    auto ended = incomplete.finish();
    ASSERT_TRUE(ended.is_err());
    EXPECT_TRUE(ended.unwrap_err().kind().is_UnexpectedEof());
}

TEST(HttpValues, MessageHeadParsesRequestTargetFormsAndTraits) {
    struct Example {
        const char* line;
        const char* method;
        const char* target;
    };
    constexpr Example examples[] = {
        { "GET /path?x=1 HTTP/1.1\r\n\r\n", "GET", "/path?x=1" },
        { "OPTIONS * HTTP/1.1\r\n\r\n", "OPTIONS", "*" },
        { "CONNECT example.com:443 HTTP/1.1\r\n\r\n", "CONNECT", "example.com:443" },
        { "GET http://example.com/path HTTP/1.1\r\n\r\n", "GET", "http://example.com/path" },
    };

    for (auto const& example : examples) {
        auto parsed = lihttpto::MessageHead::parse(as_rstd_str(example.line).as_bytes());
        ASSERT_TRUE(parsed.is_ok()) << example.line;
        auto head = rstd::move(parsed).unwrap();
        ASSERT_TRUE(head.start().is_Request()) << example.line;
        auto const& request = head.start().as_Request().value;
        EXPECT_EQ(rstd::cppstd::as_string_view(request.method().as_ref()), example.method);
        EXPECT_EQ(rstd::cppstd::as_string_view(request.target()), example.target);
        EXPECT_EQ(request.version().major().to_primitive(), 1u);
        EXPECT_EQ(request.version().minor().to_primitive(), 1u);
    }

    auto method = rstd::from_str<lihttpto::Method>("PATCH"_str);
    ASSERT_TRUE(method.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", method.unwrap())), "PATCH");
    auto version = rstd::from_str<lihttpto::MessageVersion>("HTTP/2.0"_str);
    ASSERT_TRUE(version.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", version.unwrap())), "HTTP/2.0");
    auto status = rstd::from_str<lihttpto::StatusCode>("418"_str);
    ASSERT_TRUE(status.is_ok());
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", status.unwrap())), "418");
}

TEST(HttpValues, MessageHeadReportsStartAndFieldErrors) {
    auto invalid_status = lihttpto::MessageHead::parse("HTTP/1.1 099 Bad\r\n\r\n"_bytes);
    ASSERT_TRUE(invalid_status.is_err());
    EXPECT_TRUE(invalid_status.unwrap_err().kind().is_InvalidStartLine());
    EXPECT_EQ(invalid_status.unwrap_err().offset().to_primitive(), 9u);

    auto invalid_target_text = std::string { "GET /" };
    invalid_target_text.push_back(static_cast<char>(0xff));
    invalid_target_text.append(" HTTP/1.1\r\n\r\n");
    auto invalid_target = lihttpto::MessageHead::parse(
        rstd::slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(invalid_target_text.data()),
                                        usize(invalid_target_text.size())));
    ASSERT_TRUE(invalid_target.is_err());
    EXPECT_TRUE(invalid_target.unwrap_err().kind().is_InvalidStartLine());
    EXPECT_EQ(invalid_target.unwrap_err().offset().to_primitive(), 5u);

    auto bad_name_text = std::string { "HTTP/1.1 200 OK\r\nBad Name: value\r\n\r\n" };
    auto bad_name      = lihttpto::MessageHead::parse(rstd::slice<u8>::from_raw_parts(
        reinterpret_cast<const byte*>(bad_name_text.data()), usize(bad_name_text.size())));
    ASSERT_TRUE(bad_name.is_err());
    EXPECT_TRUE(bad_name.unwrap_err().kind().is_InvalidHeaderLine());
    EXPECT_EQ(bad_name.unwrap_err().offset().to_primitive(), bad_name_text.find("Bad Name") + 3);

    auto obs_fold_text = std::string { "HTTP/1.1 200 OK\r\nX: value\r\n continuation\r\n\r\n" };
    auto obs_fold      = lihttpto::MessageHead::parse(rstd::slice<u8>::from_raw_parts(
        reinterpret_cast<const byte*>(obs_fold_text.data()), usize(obs_fold_text.size())));
    ASSERT_TRUE(obs_fold.is_err());
    EXPECT_TRUE(obs_fold.unwrap_err().kind().is_InvalidHeaderLine());
    EXPECT_EQ(obs_fold.unwrap_err().offset().to_primitive(), obs_fold_text.find(" continuation"));

    auto bare_cr_text = std::string { "HTTP/1.1 200 OK\r\nX: safe\rbad\r\n\r\n" };
    auto bare_cr      = lihttpto::MessageHead::parse(rstd::slice<u8>::from_raw_parts(
        reinterpret_cast<const byte*>(bare_cr_text.data()), usize(bare_cr_text.size())));
    ASSERT_TRUE(bare_cr.is_err());
    EXPECT_TRUE(bare_cr.unwrap_err().kind().is_InvalidHeaderLine());
    EXPECT_EQ(bare_cr.unwrap_err().offset().to_primitive(), bare_cr_text.find("\rbad"));
}

TEST(HttpValues, HttpErrorDisplayTraitsDescribeStableKinds) {
    using namespace lihttpto;

    auto url     = UrlError { UrlErrorKind::UnsupportedScheme(), usize(4) };
    auto header  = HeaderError::InvalidValue().at(usize(7));
    auto message = HttpParseError { HttpParseErrorKind::HeaderTooLarge(), usize(9) };
    auto query   = QueryError { QueryErrorKind::InvalidUtf8(), usize(2) };
    auto cookie  = CookieError { CookieErrorKind::InvalidAttribute(), usize(5) };

    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", url)), "unsupported URL scheme");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", header)), "invalid HTTP field value");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", message)),
              "HTTP field section is too large");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", query)), "invalid UTF-8 in query");
    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{}", cookie)), "invalid cookie attribute");

    EXPECT_EQ(rstd::cppstd::to_string(rstd::format("{:?}", url)), "unsupported URL scheme");
    EXPECT_TRUE(rstd::as<rstd::error::Error>(url).source().is_none());
    EXPECT_TRUE(rstd::as<rstd::error::Error>(header).source().is_none());
    EXPECT_TRUE(rstd::as<rstd::error::Error>(message).source().is_none());
    EXPECT_TRUE(rstd::as<rstd::error::Error>(query).source().is_none());
    EXPECT_TRUE(rstd::as<rstd::error::Error>(cookie).source().is_none());

    auto erased = rstd::boxed::Box<rstd::dyn<rstd::error::Error>>::make(rstd::move(url));
    EXPECT_TRUE(rstd::error::is<UrlError>(erased.as_ref()));
    EXPECT_TRUE(rstd::move(erased).downcast<UrlError>().is_ok());
}
TEST(MediaType, ContentTypeEssenceAndParameters) {
    auto parsed =
        lihttpto::MediaType::parse(" Application/JSON; charset=utf-8; note=\"a;\\\"b\" \t"_str);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_TRUE(parsed->matches("application"_str, "json"_str));
    EXPECT_FALSE(parsed->matches("application"_str, "problem+json"_str));
    ASSERT_TRUE(parsed->parameter("CHARSET"_str).is_some());
    EXPECT_TRUE(parsed->parameter_eq_ignore_ascii_case("CHARSET"_str, "UTF-8"_str));
    ASSERT_TRUE(parsed->parameter("note"_str).is_some());
    EXPECT_TRUE(parsed->parameter_eq_ignore_ascii_case("note"_str, "a;\"b"_str));
    EXPECT_TRUE(parsed->parameter("absent"_str).is_none());
    EXPECT_TRUE(
        lihttpto::MediaType::parse("text/plain; charset=utf-8; Charset=us-ascii"_str).is_err());
    for (auto value : rstd::array<ref<str>, 8> { "application"_str,
                                                 "application/"_str,
                                                 "*/json"_str,
                                                 "application/json, text/plain"_str,
                                                 "application/json; charset="_str,
                                                 "application/json; p=\"unterminated"_str,
                                                 "application/json; p=\"bad\r\""_str,
                                                 "application/json;"_str })
        EXPECT_TRUE(lihttpto::MediaType::parse(value).is_err());
}
