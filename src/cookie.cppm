export module lihttpto:cookie;
export import :http_error;
export import rstd;

namespace lihttpto
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

export class Cookie : public DefaultInClass<Cookie, Clone> {
public:
    Cookie(Cookie&&) noexcept                    = default;
    auto operator=(Cookie&&) noexcept -> Cookie& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<Cookie, CookieError>;

    [[nodiscard]]
    static auto parse_bytes(slice<u8> input) -> Result<Cookie, CookieError>;

    [[nodiscard]]
    auto name() const noexcept -> ref<str>;

    [[nodiscard]]
    auto value() const noexcept -> ref<str>;

    [[nodiscard]]
    auto is_quoted() const noexcept -> bool;

    [[nodiscard]]
    auto encode() const -> String;

    [[nodiscard]]
    auto clone() const -> Cookie;

private:
    friend class CookieHeader;
    friend class SetCookie;

    Cookie(String name, String value, bool quoted) noexcept;

    String name_;
    String value_;
    bool   quoted_;
};

export class CookieIter : public DefaultInClass<CookieIter, Iterator> {
public:
    using Item = ref<Cookie>;

    CookieIter(const Cookie* current, const Cookie* end) noexcept;

    [[nodiscard]]
    auto next() noexcept -> Option<Item>;

private:
    const Cookie* current_;
    const Cookie* end_;
};

export class CookieHeader : public DefaultInClass<CookieHeader, Clone> {
public:
    CookieHeader() noexcept                                  = default;
    CookieHeader(CookieHeader&&) noexcept                    = default;
    auto operator=(CookieHeader&&) noexcept -> CookieHeader& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<CookieHeader, CookieError>;

    [[nodiscard]]
    static auto parse_bytes(slice<u8> input) -> Result<CookieHeader, CookieError>;

    void add(Cookie cookie);

    [[nodiscard]]
    auto get(ref<str> name) const noexcept -> Option<ref<Cookie>>;

    [[nodiscard]]
    auto len() const noexcept -> usize;

    [[nodiscard]]
    auto is_empty() const noexcept -> bool;

    [[nodiscard]]
    auto iter() const noexcept -> CookieIter;

    [[nodiscard]]
    auto encode() const -> String;

    [[nodiscard]]
    auto clone() const -> CookieHeader;

private:
    Vec<Cookie> cookies_;
};

export class CookieAttribute : public DefaultInClass<CookieAttribute, Clone> {
public:
    CookieAttribute(CookieAttribute&&) noexcept                    = default;
    auto operator=(CookieAttribute&&) noexcept -> CookieAttribute& = default;

    [[nodiscard]]
    auto name() const noexcept -> ref<str>;

    [[nodiscard]]
    auto value() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto name_equals(ref<str> name) const noexcept -> bool;

    [[nodiscard]]
    auto clone() const -> CookieAttribute;

private:
    friend class SetCookie;

    CookieAttribute(String name, Option<String> value) noexcept;

    String         name_;
    Option<String> value_;
};

export class CookieAttributeIter : public DefaultInClass<CookieAttributeIter, Iterator> {
public:
    using Item = ref<CookieAttribute>;

    CookieAttributeIter(const CookieAttribute* current, const CookieAttribute* end) noexcept;

    [[nodiscard]]
    auto next() noexcept -> Option<Item>;

private:
    const CookieAttribute* current_;
    const CookieAttribute* end_;
};

export class SetCookie : public DefaultInClass<SetCookie, Clone> {
public:
    SetCookie(SetCookie&&) noexcept                    = default;
    auto operator=(SetCookie&&) noexcept -> SetCookie& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<SetCookie, CookieError>;

    [[nodiscard]]
    static auto parse_bytes(slice<u8> input) -> Result<SetCookie, CookieError>;

    [[nodiscard]]
    auto cookie() const noexcept -> const Cookie&;

    [[nodiscard]]
    auto attribute(ref<str> name) const noexcept -> Option<ref<CookieAttribute>>;

    [[nodiscard]]
    auto attributes() const noexcept -> CookieAttributeIter;

    [[nodiscard]]
    auto secure() const noexcept -> bool;

    [[nodiscard]]
    auto http_only() const noexcept -> bool;

    [[nodiscard]]
    auto encode() const -> String;

    [[nodiscard]]
    auto clone() const -> SetCookie;

private:
    SetCookie(Cookie cookie, Vec<CookieAttribute> attributes) noexcept;

    Cookie               cookie_;
    Vec<CookieAttribute> attributes_;
};

} // namespace lihttpto

namespace rstd
{

export template<>
struct Impl<str_::FromStr, lihttpto::Cookie> : ImplBase<lihttpto::Cookie> {
    using Err = lihttpto::CookieError;

    static auto from_str(ref<str> input) -> Result<lihttpto::Cookie, Err> {
        return lihttpto::Cookie::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, lihttpto::CookieHeader> : ImplBase<lihttpto::CookieHeader> {
    using Err = lihttpto::CookieError;

    static auto from_str(ref<str> input) -> Result<lihttpto::CookieHeader, Err> {
        return lihttpto::CookieHeader::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, lihttpto::SetCookie> : ImplBase<lihttpto::SetCookie> {
    using Err = lihttpto::CookieError;

    static auto from_str(ref<str> input) -> Result<lihttpto::SetCookie, Err> {
        return lihttpto::SetCookie::parse(input);
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::Cookie> : ImplBase<lihttpto::Cookie> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto encoded = this->self().encode();
        auto bytes   = encoded.as_str().as_bytes();
        return formatter.write_raw(bytes.as_raw_ptr(), bytes.len().to_primitive());
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::CookieHeader> : ImplBase<lihttpto::CookieHeader> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto encoded = this->self().encode();
        auto bytes   = encoded.as_str().as_bytes();
        return formatter.write_raw(bytes.as_raw_ptr(), bytes.len().to_primitive());
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::SetCookie> : ImplBase<lihttpto::SetCookie> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto encoded = this->self().encode();
        auto bytes   = encoded.as_str().as_bytes();
        return formatter.write_raw(bytes.as_raw_ptr(), bytes.len().to_primitive());
    }
};

} // namespace rstd

namespace lihttpto
{

static_assert(Impled<Cookie, Clone>);
static_assert(Impled<Cookie, rstd::str_::FromStr>);
static_assert(Impled<Cookie, rstd::fmt::Display>);
static_assert(Impled<CookieIter, Iterator>);
static_assert(Impled<CookieHeader, Clone>);
static_assert(Impled<CookieHeader, rstd::str_::FromStr>);
static_assert(Impled<CookieHeader, rstd::fmt::Display>);
static_assert(Impled<CookieAttribute, Clone>);
static_assert(Impled<CookieAttributeIter, Iterator>);
static_assert(Impled<SetCookie, Clone>);
static_assert(Impled<SetCookie, rstd::str_::FromStr>);
static_assert(Impled<SetCookie, rstd::fmt::Display>);

} // namespace lihttpto
