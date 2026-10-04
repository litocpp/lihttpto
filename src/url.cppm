export module lihttpto:url;
export import :http_error;
export import rstd;

namespace lihttpto
{

using namespace rstd::prelude;
using rstd::string::String;
using rstd::sync::Arc;

export class UrlOrigin : public DefaultInClass<UrlOrigin, Clone> {
public:
    struct Tuple {
        String      scheme;
        String      host;
        Option<u16> port;
    };
    UrlOrigin();
    auto is_opaque() const -> bool;
    auto tuple() const -> Option<ref<Tuple>>;
    auto serialize() const -> String;
    auto same_origin(const UrlOrigin& other) const -> bool;
    auto clone() const -> UrlOrigin;

private:
    friend class Url;
    struct OpaqueIdentity {};
    explicit UrlOrigin(Tuple tuple);
    explicit UrlOrigin(Arc<OpaqueIdentity> identity);
    Option<Tuple>               tuple_;
    Option<Arc<OpaqueIdentity>> identity_;
};

export struct HttpOrigin {
    String scheme;
    String host;
    u16    port;
    auto   authority() const -> String;
    auto   serialize() const -> String;
};

export class Url : public DefaultInClass<Url, Clone> {
    struct Host {
        enum class Kind
        {
            Domain,
            Ipv4,
            Ipv6,
            Opaque
        };
        Kind   kind = Kind::Domain;
        String text;
        u32    ipv4 {};
        u16    ipv6[8] {};
        auto   clone() const -> Host;
        auto   serialize() const -> String;
    };
    struct Record {
        String         scheme;
        String         username;
        String         password;
        Option<Host>   host;
        Option<u16>    port;
        Vec<String>    segments;
        bool           opaque = false;
        String         opaque_path;
        Option<String> query;
        Option<String> fragment;
        auto           clone() const -> Record;
    };
    friend class UrlParser;
    struct Component {
        usize offset {};
        usize size {};
        bool  present = false;
    };

public:
    Url() noexcept                         = default;
    Url(Url&&) noexcept                    = default;
    auto operator=(Url&&) noexcept -> Url& = default;

    [[nodiscard]]
    static auto parse(ref<str> input) -> Result<Url, UrlError>;

    [[nodiscard]]
    static auto parse(ref<str> input, const Url& base) -> Result<Url, UrlError>;

    [[nodiscard]]
    static auto parse_http(ref<str> input) -> Result<Url, UrlError>;

    [[nodiscard]]
    auto as_ref() const noexcept -> ref<str>;

    [[nodiscard]]
    auto scheme() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto authority() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto userinfo() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto username() const noexcept -> ref<str>;

    [[nodiscard]]
    auto password() const noexcept -> ref<str>;

    [[nodiscard]]
    auto host() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto port() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto effective_port() const -> Option<u16>;

    [[nodiscard]]
    auto path() const noexcept -> ref<str>;

    [[nodiscard]]
    auto query() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto fragment() const noexcept -> Option<ref<str>>;

    [[nodiscard]]
    auto request_target() const -> String;

    [[nodiscard]]
    auto resolve(ref<str> input) const -> Result<Url, UrlError>;

    [[nodiscard]]
    auto same_http_origin(const Url& other) const -> bool;

    [[nodiscard]]
    auto http_origin() const -> Option<HttpOrigin>;

    [[nodiscard]]
    auto origin() const -> Result<UrlOrigin, UrlError>;

    [[nodiscard]]
    auto clone() const -> Url;

private:
    explicit Url(Record record);
    void serialize();

    [[nodiscard]]
    auto component(Component value) const noexcept -> Option<ref<str>>;

    Record    record_;
    String    source_;
    Component scheme_;
    Component authority_;
    Component userinfo_;
    Component host_;
    Component port_;
    Component path_ { .present = true };
    Component query_;
    Component fragment_;
};

static_assert(Impled<Url, Clone>);
static_assert(Impled<Url, rstd::convert::AsRef<str>>);

} // namespace lihttpto

namespace rstd
{

export template<>
struct Impl<str_::FromStr, lihttpto::Url> : ImplBase<lihttpto::Url> {
    using Err = lihttpto::UrlError;

    static auto from_str(ref<str> input) -> Result<lihttpto::Url, Err> {
        return lihttpto::Url::parse(input);
    }
};

export template<>
struct Impl<fmt::Display, lihttpto::Url> : ImplBase<lihttpto::Url> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        auto value = this->self().as_ref();
        return formatter.write_raw(value.data(), value.size().to_primitive());
    }
};

} // namespace rstd

namespace lihttpto
{

static_assert(Impled<Url, rstd::str_::FromStr>);
static_assert(Impled<Url, rstd::fmt::Display>);

} // namespace lihttpto
