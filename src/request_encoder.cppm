export module lihttpto:request_encoder;
export import :messages;
export import :body_encoder;
import :response;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
export namespace lihttpto
{
class RequestEncoder {
    Bytes       head_;
    BodyFraming framing_;
    BodyEncoder body_;
    bool        started_ {};
    bool        failed_ {};
    bool        keep_alive_;
    usize       limit_;
    Option<u64> expected_;
    RequestEncoder(Bytes       head,
                   BodyFraming framing,
                   usize       limit,
                   bool        keep_alive,
                   Option<u64> expected)
        : head_(rstd::move(head)),
          framing_(framing),
          body_(framing, limit, expected),
          keep_alive_(keep_alive),
          limit_(limit),
          expected_(expected) {}

public:
    static auto make(const RequestParts& parts,
                     Option<u64>         length = Some(u64()),
                     usize               limit  = usize(65536),
                     const Headers* trailers = nullptr) -> Result<RequestEncoder, ResponseError> {
        if (trailers && validate_trailers(*trailers).is_err())
            return Err(ResponseError::InvalidTrailer);
        auto origin = parts.url.http_origin();
        if (origin.is_none() || parts.url.userinfo().is_some() ||
            parts.method.as_ref() == "CONNECT"_str)
            return Err(ResponseError::InvalidHeader);
        auto target = parts.url.request_target();
        if (parse_request_target(parts.method.as_ref(), target.as_str().as_bytes()).is_err())
            return Err(ResponseError::InvalidHeader);
        auto        authority = origin->authority();
        Option<u64> declared;
        bool        host = false, transfer = false, close = false, persistent = true;
        for (const auto& field : parts.headers) {
            if (field.name.matches("host"_str)) {
                if (host || ! ascii_equal(field.value.as_slice(), authority.as_str()))
                    return Err(ResponseError::InvalidHeader);
                host = true;
            } else if (field.name.matches("content-length"_str)) {
                if (declared.is_some()) return Err(ResponseError::InvalidHeader);
                auto parsed = parse_length(field.value.as_slice());
                if (parsed.is_err()) return Err(ResponseError::InvalidHeader);
                declared = Some(*parsed);
            } else if (field.name.matches("transfer-encoding"_str)) {
                if (transfer || ! ascii_equal(field.value.as_slice(), "chunked"_str))
                    return Err(ResponseError::InvalidHeader);
                transfer = true;
            } else if (field.name.matches("upgrade"_str))
                return Err(ResponseError::ReservedHeader);
            else if (field.name.matches("connection"_str)) {
                bool upgrade = false;
                if (! parse_connection(field.value.as_slice(), close, persistent, &upgrade) ||
                    upgrade)
                    return Err(ResponseError::InvalidHeader);
            } else if (field.name.matches("expect"_str) &&
                       ! ascii_equal(field.value.as_slice(), "100-continue"_str))
                return Err(ResponseError::InvalidHeader);
        }
        bool chunked = transfer || trailers || length.is_none();
        if ((transfer && declared.is_some()) ||
            (declared.is_some() && (chunked || length.is_none() || *declared != *length)))
            return Err(ResponseError::InvalidHeader);
        BodyFraming framing = chunked ? BodyFraming { BodyKind::Chunked }
                                      : BodyFraming { BodyKind::FixedLength, *length };
        Vec<u8>     bytes;
        auto        append = [&](ref<str> text) {
            return append_bytes(bytes, text.as_bytes(), limit);
        };
        if (! append(parts.method.as_ref()) || ! append(" "_str) || ! append(target.as_str()) ||
            ! append(" HTTP/1.1\r\nHost: "_str) || ! append(authority.as_str()) ||
            ! append("\r\n"_str))
            return Err(ResponseError::TooLarge);
        for (const auto& field : parts.headers) {
            if (field.name.matches("host"_str) || field.name.matches("content-length"_str) ||
                field.name.matches("transfer-encoding"_str))
                continue;
            if (! append(field.name.as_str()) || ! append(": "_str) ||
                ! append_bytes(bytes, field.value.as_slice(), limit) || ! append("\r\n"_str))
                return Err(ResponseError::TooLarge);
        }
        if (! chunked) {
            if (! append("Content-Length: "_str) ||
                ! append(rstd::format("{}", *length).as_str()) || ! append("\r\n"_str))
                return Err(ResponseError::TooLarge);
        } else if (! append("Transfer-Encoding: chunked\r\n"_str))
            return Err(ResponseError::TooLarge);
        if (! append("\r\n"_str)) return Err(ResponseError::TooLarge);
        return Ok(RequestEncoder(
            Bytes::copy_from_slice(bytes.as_slice()), framing, limit, ! close, length));
    }
    auto framing() const -> BodyFraming { return framing_; }
    auto keep_alive() const -> bool { return keep_alive_; }
    auto restart() const -> Result<RequestEncoder, ResponseError> {
        if (started_ || failed_) return Err(ResponseError::InvalidState);
        return Ok(RequestEncoder(head_.clone(), framing_, limit_, keep_alive_, expected_));
    }
    auto take_head() -> Result<Bytes, ResponseError> {
        if (started_ || failed_) {
            failed_ = true;
            return Err(ResponseError::InvalidState);
        }
        started_ = true;
        return Ok(rstd::exchange(head_, Bytes {}));
    }
    auto encode(BodyFrame frame) -> Result<Bytes, ResponseError> {
        if (! started_ || failed_) {
            failed_ = true;
            return Err(ResponseError::InvalidState);
        }
        return body_.encode(rstd::move(frame));
    }
    auto finish() -> Result<Bytes, ResponseError> {
        if (! started_ || failed_) {
            failed_ = true;
            return Err(ResponseError::InvalidState);
        }
        return body_.finish();
    }
};
class PreparedRequest {
    RequestParts   parts_;
    RequestEncoder encoder_;
    PreparedRequest(RequestParts parts, RequestEncoder encoder)
        : parts_(rstd::move(parts)), encoder_(rstd::move(encoder)) {}

public:
    static auto make(RequestParts   parts,
                     Option<u64>    length   = Some(u64()),
                     usize          limit    = usize(65536),
                     const Headers* trailers = nullptr) -> Result<PreparedRequest, ResponseError> {
        auto encoder = RequestEncoder::make(parts, length, limit, trailers);
        if (encoder.is_err()) return Err(rstd::move(encoder).unwrap_err());
        return Ok(PreparedRequest(rstd::move(parts), rstd::move(encoder).unwrap()));
    }
    auto parts() const -> const RequestParts& { return parts_; }
    auto encoder() const -> RequestEncoder { return encoder_.restart().unwrap(); }
    auto clone() const -> PreparedRequest { return PreparedRequest(parts_.clone(), encoder()); }
};
} // namespace lihttpto
