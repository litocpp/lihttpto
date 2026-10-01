module;
#include <rstd/enum.hpp>

export module lihttpto:http_error;
export import rstd;

namespace lihttpto
{

using namespace rstd::prelude;

export struct UrlErrorKind {
    RSTD_ENUM(UrlErrorKind,
              (InvalidSyntax),
              (InvalidCharacter),
              (InvalidPercentEncoding),
              (InvalidIpAddress),
              (InvalidPort),
              (UnexpectedEnd),
              (MissingScheme),
              (UnsupportedScheme),
              (MissingAuthority),
              (MissingHost))
};

export struct QueryErrorKind {
    RSTD_ENUM(QueryErrorKind, (InvalidPercentEncoding), (InvalidUtf8))
};

export struct CookieErrorKind {
    RSTD_ENUM(CookieErrorKind,
              (EmptyName),
              (InvalidName),
              (InvalidValue),
              (InvalidAttribute),
              (InvalidSyntax))
};

export struct HttpParseErrorKind {
    RSTD_ENUM(HttpParseErrorKind,
              (InvalidStartLine),
              (InvalidHeaderLine),
              (InvalidSyntax),
              (HeaderTooLarge),
              (UnexpectedEof))
};

export class UrlError {
public:
    constexpr UrlError(UrlErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const UrlErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    UrlErrorKind kind_;
    usize        offset_;
};

export class HttpParseError {
public:
    constexpr HttpParseError(HttpParseErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const HttpParseErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    HttpParseErrorKind kind_;
    usize              offset_;
};

export class QueryError {
public:
    constexpr QueryError(QueryErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const QueryErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    QueryErrorKind kind_;
    usize          offset_;
};

export class CookieError {
public:
    constexpr CookieError(CookieErrorKind kind, usize offset) noexcept
        : kind_(rstd::move(kind)), offset_(offset) {}

    [[nodiscard]]
    constexpr auto kind() const noexcept -> const CookieErrorKind& {
        return kind_;
    }

    [[nodiscard]]
    constexpr auto offset() const noexcept -> usize {
        return offset_;
    }

private:
    CookieErrorKind kind_;
    usize           offset_;
};

inline auto message(const UrlErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case UrlErrorKind::Tag::InvalidSyntax: return "invalid URI reference";
    case UrlErrorKind::Tag::InvalidCharacter: return "invalid character in URI reference";
    case UrlErrorKind::Tag::InvalidPercentEncoding:
        return "invalid percent encoding in URI reference";
    case UrlErrorKind::Tag::InvalidIpAddress: return "invalid IP address in URI reference";
    case UrlErrorKind::Tag::InvalidPort: return "invalid port in URI reference";
    case UrlErrorKind::Tag::UnexpectedEnd: return "unexpected end of URI reference";
    case UrlErrorKind::Tag::MissingScheme: return "HTTP URL is missing a scheme";
    case UrlErrorKind::Tag::UnsupportedScheme: return "HTTP URL has an unsupported scheme";
    case UrlErrorKind::Tag::MissingAuthority: return "HTTP URL is missing an authority";
    case UrlErrorKind::Tag::MissingHost: return "HTTP URL is missing a host";
    }
    rstd::unreachable();
}

inline auto message(const HttpParseErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case HttpParseErrorKind::Tag::InvalidStartLine: return "invalid HTTP start line";
    case HttpParseErrorKind::Tag::InvalidHeaderLine: return "invalid HTTP field line";
    case HttpParseErrorKind::Tag::InvalidSyntax: return "invalid HTTP message syntax";
    case HttpParseErrorKind::Tag::HeaderTooLarge: return "HTTP field section is too large";
    case HttpParseErrorKind::Tag::UnexpectedEof: return "unexpected end of HTTP message head";
    }
    rstd::unreachable();
}

inline auto message(const QueryErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case QueryErrorKind::Tag::InvalidPercentEncoding: return "invalid percent encoding in query";
    case QueryErrorKind::Tag::InvalidUtf8: return "invalid UTF-8 in query";
    }
    rstd::unreachable();
}

inline auto message(const CookieErrorKind& kind) noexcept -> const char* {
    switch (kind.tag()) {
    case CookieErrorKind::Tag::EmptyName: return "empty cookie name";
    case CookieErrorKind::Tag::InvalidName: return "invalid cookie name";
    case CookieErrorKind::Tag::InvalidValue: return "invalid cookie value";
    case CookieErrorKind::Tag::InvalidAttribute: return "invalid cookie attribute";
    case CookieErrorKind::Tag::InvalidSyntax: return "invalid cookie syntax";
    }
    rstd::unreachable();
}

} // namespace lihttpto

namespace rstd
{

export template<>
struct Impl<fmt::Display, lihttpto::UrlError> : ImplBase<lihttpto::UrlError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = lihttpto::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::HttpParseError> : ImplBase<lihttpto::HttpParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = lihttpto::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::QueryError> : ImplBase<lihttpto::QueryError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = lihttpto::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::CookieError> : ImplBase<lihttpto::CookieError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto* message = lihttpto::message(this->self().kind());
        return formatter.write_raw(message, rstd::strlen(message));
    }
};

export template<>
struct Impl<fmt::Debug, lihttpto::UrlError> : ImplBase<lihttpto::UrlError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, lihttpto::HttpParseError> : ImplBase<lihttpto::HttpParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, lihttpto::QueryError> : ImplBase<lihttpto::QueryError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<fmt::Debug, lihttpto::CookieError> : ImplBase<lihttpto::CookieError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

export template<>
struct Impl<error::Error, lihttpto::UrlError> : DefaultInImpl<error::Error, lihttpto::UrlError> {};

export template<>
struct Impl<error::Error, lihttpto::HttpParseError>
    : DefaultInImpl<error::Error, lihttpto::HttpParseError> {};

export template<>
struct Impl<error::Error, lihttpto::QueryError>
    : DefaultInImpl<error::Error, lihttpto::QueryError> {};

export template<>
struct Impl<error::Error, lihttpto::CookieError>
    : DefaultInImpl<error::Error, lihttpto::CookieError> {};

} // namespace rstd

namespace lihttpto
{

static_assert(Impled<UrlError, rstd::error::Error>);
static_assert(Impled<HttpParseError, rstd::error::Error>);
static_assert(Impled<QueryError, rstd::error::Error>);
static_assert(Impled<CookieError, rstd::error::Error>);

} // namespace lihttpto
