export module lihttpto:cookie_store;
export import :cookie;
export import :url;
export import :date;
using namespace rstd::prelude;
export namespace lihttpto
{
struct PublicSuffixPolicy {
    template<class Self, class = void>
    struct Api {
        using Trait = PublicSuffixPolicy;
        auto is_public_suffix(ref<str> domain) const -> bool {
            return rstd::trait_call<0>(this, domain);
        }
    };
    template<class T>
    using Funcs = rstd::TraitFuncs<&T::is_public_suffix>;
};
struct CookieOptions {
    usize max_entries { 3000 }, max_cookie_bytes { 4096 };
    Option<rstd::sync::Arc<rstd::dyn<PublicSuffixPolicy>>> suffixes;
    // Without a suffix policy this permits cookies scoped to public suffixes.
    bool allow_unverified_suffixes {};
    // Permits insecure origins to set cookies later sent over HTTPS.
    bool        allow_secure_from_insecure {};
    bool        retain_expired_replacements {};
    Option<i64> max_age_expiry;
    auto        clone() const -> CookieOptions {
        return { max_entries,
                 max_cookie_bytes,
                 suffixes.clone(),
                 allow_unverified_suffixes,
                 allow_secure_from_insecure,
                 retain_expired_replacements,
                 max_age_expiry };
    }
};
struct StoredCookie {
    Cookie      cookie;
    String      domain, path;
    Option<i64> expires;
    bool        host_only { true }, secure {}, http_only {};
    String      raw_cookie;
    bool        path_explicit {};
    unsigned    expiry_nanoseconds {};
    auto        clone() const -> StoredCookie {
        return { cookie.clone(),    domain.clone(),     path.clone(),
                 expires,           host_only,          secure,
                 http_only,         raw_cookie.clone(), path_explicit,
                 expiry_nanoseconds };
    }
};
class CookieFileError {
    usize line_;

public:
    explicit CookieFileError(usize line): line_(line) {}
    auto line() const -> usize { return line_; }
};
auto cookie_date(ref<str> text) -> Option<i64>;
class CookieStorage {
    CookieOptions     options_;
    Vec<StoredCookie> entries_;
    Option<u64>       revision_ { Some(u64()) };
    auto              insert(StoredCookie cookie, i64 now) -> bool;
    auto              allowed_domain(ref<str> domain) const -> bool;
    void              content_changed();
    void              replace_entries(Vec<StoredCookie> cookies);

public:
    explicit CookieStorage(CookieOptions options = {}): options_(rstd::move(options)) {}
    auto store(const SetCookie& cookie, const Url& origin, i64 now) -> bool;
    auto select(const Url& target, i64 now) -> CookieHeader;
    auto snapshot(i64 now) -> Vec<StoredCookie>;
    auto snapshot_all() const -> Vec<StoredCookie>;
    // The view is invalidated by mutation or destruction of this storage.
    auto snapshot_view() const [[clang::lifetimebound]] -> slice<StoredCookie> {
        return entries_.as_slice();
    }
    // Storage-local content version; None stays disabled after exhaustion.
    auto revision() const -> Option<u64> { return revision_; }
    // Restores trusted persisted state; a configured suffix policy is still enforced.
    auto replace_snapshot(Vec<StoredCookie> cookies) -> Result<empty, CookieFileError>;
    auto import_netscape(ref<str> input, i64 now) -> Result<empty, CookieFileError>;
    auto export_netscape(i64 now) -> String;
    void clear();
    auto erase(ref<str> domain, ref<str> path, ref<str> name) -> bool;
    void expire(i64 now);
};
} // namespace lihttpto
export namespace rstd
{
template<>
struct Impl<fmt::Display, lihttpto::CookieFileError> : ImplBase<lihttpto::CookieFileError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(
            fmt::Arguments::make("invalid cookie file at line {}", this->self().line()));
    }
};
template<>
struct Impl<fmt::Debug, lihttpto::CookieFileError> : Impl<fmt::Display, lihttpto::CookieFileError> {
};
template<>
struct Impl<error::Error, lihttpto::CookieFileError>
    : DefaultInImpl<error::Error, lihttpto::CookieFileError> {};
} // namespace rstd
