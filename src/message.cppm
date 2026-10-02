module;
#include <rstd/enum.hpp>

export module lihttpto:message;
export import :http_error;
export import :header;
export import rstd;

namespace lihttpto
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::vec::Vec;

export class Method : public DefaultInClass<Method, Clone> {
public:
    Method(Method&&) noexcept                    = default;
    auto operator=(Method&&) noexcept -> Method& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<Method, HttpParseError>;

    [[nodiscard]]
    auto as_ref() const noexcept -> ref<str>;

    [[nodiscard]]
    auto clone() const -> Method;

private:
    explicit Method(String value) noexcept;

    String value_;
};

export class MessageVersion {
public:
    constexpr MessageVersion(u8 major, u8 minor) noexcept: major_(major), minor_(minor) {}

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<MessageVersion, HttpParseError>;

    [[nodiscard]]
    constexpr auto major() const noexcept -> u8 {
        return major_;
    }

    [[nodiscard]]
    constexpr auto minor() const noexcept -> u8 {
        return minor_;
    }

private:
    u8 major_;
    u8 minor_;
};

export class StatusCode {
public:
    [[nodiscard]]
    static auto make(u16 value) -> Result<StatusCode, HttpParseError>;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<StatusCode, HttpParseError>;

    [[nodiscard]]
    constexpr auto value() const noexcept -> u16 {
        return value_;
    }

private:
    explicit constexpr StatusCode(u16 value) noexcept: value_(value) {}

    u16 value_;
};

export class MessageRequestLine : public DefaultInClass<MessageRequestLine, Clone> {
public:
    MessageRequestLine(Method method, String target, MessageVersion version) noexcept;

    [[nodiscard]]
    auto method() const noexcept -> const Method&;

    [[nodiscard]]
    auto target() const noexcept -> ref<str>;

    [[nodiscard]]
    auto version() const noexcept -> MessageVersion;

    [[nodiscard]]
    auto clone() const -> MessageRequestLine;

private:
    Method         method_;
    String         target_;
    MessageVersion version_;
};

export class StatusLine : public DefaultInClass<StatusLine, Clone> {
public:
    StatusLine(Option<MessageVersion> version,
               StatusCode             status,
               Option<HeaderValue>    reason) noexcept;

    [[nodiscard]]
    auto version() const noexcept -> Option<MessageVersion>;

    [[nodiscard]]
    auto status() const noexcept -> StatusCode;

    [[nodiscard]]
    auto reason() const noexcept -> Option<ref<HeaderValue>>;

    [[nodiscard]]
    auto clone() const -> StatusLine;

private:
    Option<MessageVersion> version_;
    StatusCode             status_;
    Option<HeaderValue>    reason_;
};

export class StartLine : public DefaultInClass<StartLine, Clone> {
    RSTD_ENUM(StartLine, (Request, (MessageRequestLine value;)), (Response, (StatusLine value;)))

    [[nodiscard]]
    auto clone() const -> StartLine;
};

export struct ResponseHead {
    StatusCode             status;
    Headers                headers;
    Option<MessageVersion> version;
    Option<HeaderValue>    reason;
};

export class MessageHead : public DefaultInClass<MessageHead, Clone> {
public:
    MessageHead(StartLine start, Headers headers) noexcept;

    [[nodiscard]]
    static auto parse(slice<u8> input) -> Result<MessageHead, HttpParseError>;

    [[nodiscard]]
    auto start() const noexcept -> const StartLine&;

    [[nodiscard]]
    auto headers() const noexcept -> const Headers&;

    [[nodiscard]]
    auto status_code() const noexcept -> Option<u16>;

    [[nodiscard]]
    auto has_field(ref<str> name) const noexcept -> bool;

    [[nodiscard]]
    auto clone() const -> MessageHead;

    auto into_response() && -> Result<ResponseHead, HttpParseError>;

private:
    StartLine start_;
    Headers   headers_;
};

export class Http1HeadEvent {
    RSTD_ENUM(Http1HeadEvent, (NeedMore), (Complete, (MessageHead head; usize consumed;)))
};

export class Http1HeadParser {
public:
    static constexpr usize MaxHeaderBytes { 64 * 1024 };

    explicit Http1HeadParser(bool  transport_metadata = false,
                             usize max_header_bytes   = MaxHeaderBytes) noexcept
        : transport_metadata_(transport_metadata), max_header_bytes_(max_header_bytes) {}
    Http1HeadParser(Http1HeadParser&&) noexcept                    = default;
    auto operator=(Http1HeadParser&&) noexcept -> Http1HeadParser& = default;

    [[nodiscard]]
    auto push(slice<u8> input) -> Result<Http1HeadEvent, HttpParseError>;

    [[nodiscard]]
    auto finish() -> Result<MessageHead, HttpParseError>;

private:
    bool              transport_metadata_ { false };
    usize             max_header_bytes_;
    Vec<u8>           buffer_;
    usize             line_start_ {};
    usize             scan_ {};
    Option<StartLine> start_;
    Headers           headers_;
    bool              complete_ = false;
    bool              failed_   = false;
};

export class Http1FieldSectionEvent {
    RSTD_ENUM(Http1FieldSectionEvent, (NeedMore), (Complete, (Headers fields; usize consumed;)))
};

export class Http1FieldSectionParser {
public:
    static constexpr usize MaxHeaderBytes = Http1HeadParser::MaxHeaderBytes;

    explicit Http1FieldSectionParser(usize max_header_bytes = MaxHeaderBytes) noexcept
        : max_header_bytes_(max_header_bytes) {}
    Http1FieldSectionParser(Http1FieldSectionParser&&) noexcept                    = default;
    auto operator=(Http1FieldSectionParser&&) noexcept -> Http1FieldSectionParser& = default;

    [[nodiscard]]
    auto push(slice<u8> input) -> Result<Http1FieldSectionEvent, HttpParseError>;

    [[nodiscard]]
    auto finish() -> Result<Headers, HttpParseError>;

private:
    usize   max_header_bytes_;
    Vec<u8> buffer_;
    usize   line_start_ {};
    usize   scan_ {};
    Headers fields_;
    bool    complete_ = false;
    bool    failed_   = false;
};

} // namespace lihttpto

namespace rstd
{

export template<>
struct Impl<str_::FromStr, lihttpto::Method> : ImplBase<lihttpto::Method> {
    using Err = lihttpto::HttpParseError;

    static auto from_str(ref<str> input) -> Result<lihttpto::Method, Err> {
        return lihttpto::Method::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, lihttpto::MessageVersion> : ImplBase<lihttpto::MessageVersion> {
    using Err = lihttpto::HttpParseError;

    static auto from_str(ref<str> input) -> Result<lihttpto::MessageVersion, Err> {
        return lihttpto::MessageVersion::parse(input);
    }
};

export template<>
struct Impl<str_::FromStr, lihttpto::StatusCode> : ImplBase<lihttpto::StatusCode> {
    using Err = lihttpto::HttpParseError;

    static auto from_str(ref<str> input) -> Result<lihttpto::StatusCode, Err> {
        return lihttpto::StatusCode::parse(input);
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::Method> : ImplBase<lihttpto::Method> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = this->self().as_ref();
        return formatter.write_raw(value.data(), value.size().to_primitive());
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::MessageVersion> : ImplBase<lihttpto::MessageVersion> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = rstd::array<u8, 8> {
            u8('H'), u8('T'),
            u8('T'), u8('P'),
            u8('/'), u8('0' + this->self().major().to_primitive()),
            u8('.'), u8('0' + this->self().minor().to_primitive()),
        };
        return formatter.write_raw(value.data(), value.len().to_primitive());
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::StatusCode> : ImplBase<lihttpto::StatusCode> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = this->self().value().to_primitive();
        auto bytes = rstd::array<u8, 3> {
            u8('0' + value / 100),
            u8('0' + value / 10 % 10),
            u8('0' + value % 10),
        };
        return formatter.write_raw(bytes.data(), bytes.len().to_primitive());
    }
};

} // namespace rstd

namespace lihttpto
{

static_assert(Impled<Method, Clone>);
static_assert(Impled<Method, AsRef<str>>);
static_assert(Impled<Method, rstd::str_::FromStr>);
static_assert(Impled<Method, rstd::fmt::Display>);
static_assert(Impled<MessageVersion, Clone>);
static_assert(Impled<MessageVersion, rstd::str_::FromStr>);
static_assert(Impled<MessageVersion, rstd::fmt::Display>);
static_assert(Impled<StatusCode, Clone>);
static_assert(Impled<StatusCode, rstd::str_::FromStr>);
static_assert(Impled<StatusCode, rstd::fmt::Display>);
static_assert(Impled<MessageRequestLine, Clone>);
static_assert(Impled<StatusLine, Clone>);
static_assert(Impled<StartLine, Clone>);
static_assert(Impled<MessageHead, Clone>);

} // namespace lihttpto
