export module lihttpto:request_encoder;
export import :messages;
export import :hpack;
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
                     Option<u64>         length      = Some(u64()),
                     usize               limit       = usize(65536),
                     const Headers*      trailers    = nullptr,
                     TargetForm          target_form = TargetForm::Origin)
        -> Result<RequestEncoder, ResponseError> {
        if (trailers && validate_trailers(*trailers).is_err())
            return Err(ResponseError::InvalidTrailer);
        auto origin = parts.url.http_origin();
        if (origin.is_none() || parts.url.userinfo().is_some() ||
            parts.method.as_ref() == "CONNECT"_str)
            return Err(ResponseError::InvalidHeader);
        if (target_form != TargetForm::Origin && target_form != TargetForm::Absolute)
            return Err(ResponseError::InvalidHeader);
        auto target = target_form == TargetForm::Absolute
                          ? rstd::format("{}{}", origin->serialize(), parts.url.request_target())
                          : parts.url.request_target();
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
    auto length() const -> Option<u64> { return expected_; }
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
    auto encode_piece(BodyFrame frame) -> Result<EncodedBody, ResponseError> {
        if (! started_ || failed_) {
            failed_ = true;
            return Err(ResponseError::InvalidState);
        }
        return body_.encode_piece(rstd::move(frame));
    }
    auto encode(BodyFrame frame) -> Result<Bytes, ResponseError> {
        auto piece = encode_piece(rstd::move(frame));
        if (piece.is_err()) return Err(rstd::move(piece).unwrap_err());
        return Ok(rstd::move(piece->bytes));
    }
    auto finish() -> Result<Bytes, ResponseError> {
        if (! started_ || failed_) {
            failed_ = true;
            return Err(ResponseError::InvalidState);
        }
        return body_.finish();
    }
};
namespace h2
{
struct Request {
    Vec<Field>  fields;
    Option<u64> length;
    bool        head {};
    auto        clone() const -> Request {
        Request copy;
        copy.length = length;
        copy.head   = head;
        for (const auto& field : fields) copy.fields.push(field.clone());
        return copy;
    }
};
} // namespace h2
class PreparedRequest {
    RequestParts   parts_;
    RequestEncoder encoder_;
    PreparedRequest(RequestParts parts, RequestEncoder encoder)
        : parts_(rstd::move(parts)), encoder_(rstd::move(encoder)) {}

public:
    static auto make(RequestParts   parts,
                     Option<u64>    length      = Some(u64()),
                     usize          limit       = usize(65536),
                     const Headers* trailers    = nullptr,
                     TargetForm     target_form = TargetForm::Origin)
        -> Result<PreparedRequest, ResponseError> {
        auto encoder = RequestEncoder::make(parts, length, limit, trailers, target_form);
        if (encoder.is_err()) return Err(rstd::move(encoder).unwrap_err());
        return Ok(PreparedRequest(rstd::move(parts), rstd::move(encoder).unwrap()));
    }
    auto http2_request() const -> Result<h2::Request, ResponseError> {
        h2::Request result;
        result.length = encoder_.length();
        result.head   = parts_.method.as_ref() == "HEAD"_str;
        auto add      = [&](ref<str> name, ref<str> value) {
            result.fields.push(
                { String::make(name), Bytes::copy_from_slice(value.as_bytes()), false });
        };
        auto origin = parts_.url.http_origin().unwrap();
        add(":method"_str, parts_.method.as_ref());
        add(":scheme"_str, origin.scheme.as_str());
        auto authority = origin.authority();
        add(":authority"_str, authority.as_str());
        auto target = parts_.url.request_target();
        add(":path"_str, target.as_str());
        for (const auto& field : parts_.headers) {
            if (field.name.matches("host"_str) || field.name.matches("content-length"_str))
                continue;
            if (field.name.matches("connection"_str) || field.name.matches("keep-alive"_str) ||
                field.name.matches("proxy-connection"_str) ||
                field.name.matches("transfer-encoding"_str) || field.name.matches("upgrade"_str))
                return Err(ResponseError::ReservedHeader);
            auto value = field.value.as_slice();
            if (! value.is_empty() && (ows(value[usize()]) || ows(value[value.len() - usize(1)])))
                return Err(ResponseError::InvalidHeader);
            if (field.name.matches("te"_str) && ! ascii_equal(value, "trailers"_str))
                return Err(ResponseError::InvalidHeader);
            auto name = String::make(field.name.as_str());
            name.as_mut_str().make_ascii_lowercase();
            bool sensitive = field.name.matches("authorization"_str) ||
                             field.name.matches("cookie"_str) ||
                             field.name.matches("proxy-authorization"_str);
            result.fields.push(
                { rstd::move(name), Bytes::copy_from_slice(field.value.as_slice()), sensitive });
        }
        if (result.length.is_some())
            add("content-length"_str, rstd::format("{}", *result.length).as_str());
        return Ok(rstd::move(result));
    }
    auto parts() const -> const RequestParts& { return parts_; }
    auto encoder() const -> RequestEncoder { return encoder_.restart().unwrap(); }
    auto clone() const -> PreparedRequest { return PreparedRequest(parts_.clone(), encoder()); }
};
} // namespace lihttpto
