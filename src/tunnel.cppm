export module lihttpto:tunnel;
export import :response_head;
export import :url;
using namespace rstd::prelude;
using namespace rstd::literals;
export namespace lihttpto
{
auto connect_request(const Url& target, usize limit = usize(65536))
    -> Result<rstd::bytes::Bytes, DecodeError> {
    auto origin = target.http_origin();
    if (origin.is_none() || target.userinfo().is_some()) return Err(DecodeError::UnsupportedTarget);
    auto authority = rstd::format("{}:{}", origin->host, origin->port);
    if (parse_authority(authority.as_str().as_bytes(), true).is_err())
        return Err(DecodeError::InvalidHost);
    auto head = rstd::format("CONNECT {} HTTP/1.1\r\nHost: {}\r\n\r\n", authority, authority);
    if (head.len() > limit) return Err(DecodeError::TooLarge);
    return Ok(rstd::bytes::Bytes::copy_from_slice(head.as_str().as_bytes()));
}
enum class TunnelDisposition
{
    Informational,
    Established,
    Rejected
};
struct DecodedTunnelHead {
    ResponseHead      head;
    BodyFraming       framing;
    TunnelDisposition disposition;
    usize             input_consumed;
};
class TunnelHeadDecoder {
    Http1HeadParser parser_;
    usize           limit_, used_, informational_;
    bool            finished_ {};

public:
    explicit TunnelHeadDecoder(usize limit = usize(65536)): parser_(false, limit), limit_(limit) {}
    auto feed(slice<u8> input) -> Result<Option<DecodedTunnelHead>, DecodeError> {
        if (finished_) return Err(DecodeError::InvalidState);
        auto parsed = parser_.push(input);
        if (parsed.is_err()) return Err(DecodeError::InvalidHeader);
        if (parsed->is_NeedMore()) return Ok(None());
        auto event = rstd::move(parsed->as_Complete());
        auto head  = rstd::move(event.head).into_response();
        if (head.is_err()) return Err(DecodeError::InvalidHeader);
        if (head->version.is_none() || head->version->major() != u8(1) ||
            head->version->minor() > u8(1))
            return Err(DecodeError::UnsupportedVersion);
        auto status = head->status.value();
        if (status == u16(101)) return Err(DecodeError::UnsupportedTarget);
        used_ += event.consumed;
        auto        disposition = status < u16(200)   ? TunnelDisposition::Informational
                                  : status < u16(300) ? TunnelDisposition::Established
                                                      : TunnelDisposition::Rejected;
        BodyFraming framing;
        if (disposition != TunnelDisposition::Established) {
            auto decoded = response_body_framing(*head, Method::parse("CONNECT"_str).unwrap());
            if (decoded.is_err()) return Err(decoded.unwrap_err());
            framing = *decoded;
        }
        if (disposition == TunnelDisposition::Informational) {
            if (++informational_ > usize(16) || used_ >= limit_) return Err(DecodeError::TooLarge);
            parser_ = Http1HeadParser(false, limit_ - used_);
        } else
            finished_ = true;
        return Ok(Some(DecodedTunnelHead {
            rstd::move(head).unwrap(), framing, disposition, event.input_consumed }));
    }
};
} // namespace lihttpto
