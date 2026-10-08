export module lihttpto:upgrade;
export import :message;
export import :request_body;
export import :request_head;
import :response;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
enum class UpgradeError
{
    InvalidRequest,
    ReservedHeader,
    TooLarge,
    RejectedStatus,
    InvalidVersion,
    InvalidConnection,
    InvalidProtocol,
    InvalidFraming
};
enum class UpgradeDisposition
{
    Informational,
    Switched,
    Rejected
};

auto response_body_framing(const ResponseHead& response, const Method& method)
    -> Result<BodyFraming, DecodeError> {
    Option<u64> length;
    bool        chunked = false;
    auto        status  = response.status.value();
    for (const auto& header : response.headers) {
        if (header.name.matches("content-length"_str)) {
            if (length.is_some()) return Err(DecodeError::AmbiguousFraming);
            auto parsed = parse_length(header.value.as_slice());
            if (parsed.is_err()) return Err(parsed.unwrap_err());
            length = Some(*parsed);
        } else if (header.name.matches("transfer-encoding"_str)) {
            if (chunked || ! ascii_equal(header.value.as_slice(), "chunked"_str))
                return Err(DecodeError::UnsupportedTransferEncoding);
            chunked = true;
        }
    }
    if (chunked && length.is_some()) return Err(DecodeError::AmbiguousFraming);
    if (status < u16(200) || status == u16(204)) {
        if (length.is_some() || chunked) return Err(DecodeError::AmbiguousFraming);
        return Ok(BodyFraming {});
    }
    if (chunked && (response.version.is_none() || response.version->major() != u8(1) ||
                    response.version->minor() != u8(1)))
        return Err(DecodeError::UnsupportedTransferEncoding);
    if (method.as_ref() == "HEAD"_str || status == u16(304)) return Ok(BodyFraming {});
    if (method.as_ref() == "CONNECT"_str && status < u16(300))
        return Err(DecodeError::UnsupportedTarget);
    if (chunked) return Ok(BodyFraming { BodyKind::Chunked });
    if (length.is_some()) return Ok(BodyFraming { BodyKind::FixedLength, *length });
    return Ok(BodyFraming { BodyKind::UntilEof });
}

class UpgradeProtocol {
    HeaderName         name_;
    Option<HeaderName> version_;
    UpgradeProtocol(HeaderName name, Option<HeaderName> version)
        : name_(rstd::move(name)), version_(rstd::move(version)) {}

public:
    static auto make(ref<str> name, Option<ref<str>> version = {})
        -> Result<UpgradeProtocol, UpgradeError> {
        auto parsed = HeaderName::make(name);
        if (parsed.is_err()) return Err(UpgradeError::InvalidProtocol);
        Option<HeaderName> parsed_version;
        if (version.is_some()) {
            auto value = HeaderName::make(*version);
            if (value.is_err()) return Err(UpgradeError::InvalidProtocol);
            parsed_version = Some(rstd::move(value).unwrap());
        }
        return Ok(UpgradeProtocol { rstd::move(parsed).unwrap(), rstd::move(parsed_version) });
    }
    auto encode() const -> String {
        return version_.is_some() ? rstd::format("{}/{}", name_.as_str(), version_->as_str())
                                  : String::make(name_.as_str());
    }
    auto matches(slice<u8> bytes) const -> bool {
        usize begin {}, end = bytes.len();
        while (begin < end && ows(bytes[begin])) ++begin;
        while (end > begin && ows(bytes[end - usize(1)])) --end;
        usize slash = begin;
        while (slash < end && bytes[slash] != u8('/')) ++slash;
        auto name =
            slice<u8>::from_raw_parts(bytes.as_raw_ptr() + begin.to_primitive(), slash - begin);
        if (! ascii_equal(name, name_.as_str())) return false;
        if (slash == end) return version_.is_none();
        if (version_.is_none()) return false;
        auto version = slice<u8>::from_raw_parts(bytes.as_raw_ptr() + slash.to_primitive() + 1,
                                                 end - slash - usize(1));
        return version == version_->as_str().as_bytes();
    }
};

class UpgradeRequest {
    rstd::bytes::Bytes bytes_;
    UpgradeProtocol    protocol_;
    Method             method_;
    UpgradeRequest(rstd::bytes::Bytes bytes, UpgradeProtocol protocol, Method method)
        : bytes_(rstd::move(bytes)), protocol_(rstd::move(protocol)), method_(rstd::move(method)) {}

public:
    // This API offers one protocol and no request body.
    static auto make(Method          method,
                     ref<str>        target,
                     ref<str>        host,
                     UpgradeProtocol protocol,
                     const Headers&  headers = {},
                     usize           limit = usize(65536)) -> Result<UpgradeRequest, UpgradeError> {
        if (target.len() > limit || host.len() > limit) return Err(UpgradeError::TooLarge);
        auto parsed_target = parse_request_target(method.as_ref(), target.as_bytes());
        auto authority     = parse_authority(host.as_bytes());
        if (parsed_target.is_err() || parsed_target->form != TargetForm::Origin ||
            authority.is_err())
            return Err(UpgradeError::InvalidRequest);
        for (const auto& header : headers) {
            for (auto reserved : array<ref<str>, 7> { "host"_str,
                                                      "connection"_str,
                                                      "upgrade"_str,
                                                      "content-length"_str,
                                                      "transfer-encoding"_str,
                                                      "expect"_str,
                                                      "trailer"_str })
                if (header.name.matches(reserved)) return Err(UpgradeError::ReservedHeader);
        }
        Vec<u8> bytes;
        auto    append = [&](ref<str> text) {
            return append_bytes(bytes, text.as_bytes(), limit);
        };
        if (! append(method.as_ref()) || ! append(" "_str) || ! append(target) ||
            ! append(" HTTP/1.1\r\nHost: "_str) || ! append(host) ||
            ! append("\r\nConnection: Upgrade\r\nUpgrade: "_str) ||
            ! append(protocol.encode().as_str()) || ! append("\r\n"_str))
            return Err(UpgradeError::TooLarge);
        if (method.as_ref() == "POST"_str || method.as_ref() == "PUT"_str ||
            method.as_ref() == "PATCH"_str)
            if (! append("Content-Length: 0\r\n"_str)) return Err(UpgradeError::TooLarge);
        for (const auto& header : headers) {
            if (! append(header.name.as_str()) || ! append(": "_str) ||
                ! append_bytes(bytes, header.value.as_slice(), limit) || ! append("\r\n"_str))
                return Err(UpgradeError::TooLarge);
        }
        if (! append("\r\n"_str)) return Err(UpgradeError::TooLarge);
        return Ok(UpgradeRequest { rstd::bytes::Bytes::copy_from_slice(bytes.as_slice()),
                                   rstd::move(protocol),
                                   rstd::move(method) });
    }
    auto bytes() const -> const rstd::bytes::Bytes& { return bytes_; }
    auto body_framing(const ResponseHead& response) const -> Result<BodyFraming, DecodeError> {
        return response_body_framing(response, method_);
    }
    auto classify(const ResponseHead& response) const -> Result<UpgradeDisposition, UpgradeError> {
        auto status = response.status.value();
        if (status == u16(101)) {
            auto result = validate(response);
            if (result.is_err()) return Err(result.unwrap_err());
            return Ok(UpgradeDisposition::Switched);
        }
        if (status < u16(200)) {
            if (response.version.is_none() || response.version->major() != u8(1) ||
                response.version->minor() != u8(1))
                return Err(UpgradeError::InvalidVersion);
            if (body_framing(response).is_err()) return Err(UpgradeError::InvalidFraming);
            return Ok(UpgradeDisposition::Informational);
        }
        return Ok(UpgradeDisposition::Rejected);
    }
    auto validate(const ResponseHead& response) const -> Result<empty, UpgradeError> {
        if (response.status.value() != u16(101)) return Err(UpgradeError::RejectedStatus);
        if (response.version.is_none() || response.version->major() != u8(1) ||
            response.version->minor() != u8(1))
            return Err(UpgradeError::InvalidVersion);
        bool     upgrade = false, close = false, persistent = false;
        unsigned protocols = 0;
        for (const auto& header : response.headers) {
            if (header.name.matches("content-length"_str) ||
                header.name.matches("transfer-encoding"_str))
                return Err(UpgradeError::InvalidFraming);
            if (header.name.matches("connection"_str) &&
                ! parse_connection(header.value.as_slice(), close, persistent, &upgrade))
                return Err(UpgradeError::InvalidConnection);
            if (header.name.matches("upgrade"_str)) {
                if (++protocols > 1 || ! protocol_.matches(header.value.as_slice()))
                    return Err(UpgradeError::InvalidProtocol);
            }
        }
        if (! upgrade || close) return Err(UpgradeError::InvalidConnection);
        if (! protocols) return Err(UpgradeError::InvalidProtocol);
        return Ok(empty {});
    }
};

class UpgradeOffer {
    Vec<UpgradeProtocol> protocols_;
    explicit UpgradeOffer(Vec<UpgradeProtocol> protocols): protocols_(rstd::move(protocols)) {}

public:
    static auto parse(const RequestHead& request) -> Result<UpgradeOffer, UpgradeError> {
        if (request.line.version != Version::Http11) return Err(UpgradeError::InvalidVersion);
        if (request.body.kind != BodyKind::None &&
            ! (request.body.kind == BodyKind::FixedLength && request.body.length == u64()))
            return Err(UpgradeError::InvalidFraming);
        bool                 upgrade = false, close = false, persistent = false;
        Vec<UpgradeProtocol> protocols;
        for (const auto& header : request.headers) {
            if (header.name.matches("expect"_str)) return Err(UpgradeError::InvalidRequest);
            if (header.name.matches("connection"_str) &&
                ! parse_connection(header.value.as_slice(), close, persistent, &upgrade))
                return Err(UpgradeError::InvalidConnection);
            if (! header.name.matches("upgrade"_str)) continue;
            rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(header.value.as_slice()) };
            while (! cursor.is_eof()) {
                (void)rstd::parse::consume_while(cursor, ows);
                if (cursor.is_eof()) break;
                if (rstd::parse::consume_literal(cursor, ","_str).is_some()) continue;
                auto name = rstd::parse::consume_while_one(cursor, token_byte);
                if (name.is_none()) return Err(UpgradeError::InvalidProtocol);
                Option<ref<str>> version;
                if (rstd::parse::consume_literal(cursor, "/"_str).is_some()) {
                    auto parsed = rstd::parse::consume_while_one(cursor, token_byte);
                    if (parsed.is_none()) return Err(UpgradeError::InvalidProtocol);
                    version = Some(rstd::str_::from_utf8(cursor.view(*parsed)).unwrap());
                }
                auto protocol = UpgradeProtocol::make(
                    rstd::str_::from_utf8(cursor.view(*name)).unwrap(), version);
                if (protocol.is_err()) return Err(protocol.unwrap_err());
                protocols.push(rstd::move(protocol).unwrap());
                (void)rstd::parse::consume_while(cursor, ows);
                if (! cursor.is_eof() && rstd::parse::consume_literal(cursor, ","_str).is_none())
                    return Err(UpgradeError::InvalidProtocol);
            }
        }
        if (! upgrade || close) return Err(UpgradeError::InvalidConnection);
        if (protocols.is_empty()) return Err(UpgradeError::InvalidProtocol);
        return Ok(UpgradeOffer { rstd::move(protocols) });
    }

    auto response(const UpgradeProtocol& selected,
                  const Headers&         headers = {},
                  usize limit = usize(65536)) const -> Result<rstd::bytes::Bytes, UpgradeError> {
        auto protocol = selected.encode();
        bool offered  = false;
        for (const auto& candidate : protocols_)
            offered = offered || candidate.matches(protocol.as_str().as_bytes());
        if (! offered) return Err(UpgradeError::InvalidProtocol);
        for (const auto& header : headers)
            for (auto reserved : array<ref<str>, 5> { "connection"_str,
                                                      "upgrade"_str,
                                                      "content-length"_str,
                                                      "transfer-encoding"_str,
                                                      "trailer"_str })
                if (header.name.matches(reserved)) return Err(UpgradeError::ReservedHeader);
        Vec<u8> bytes;
        auto    append = [&](ref<str> text) {
            return append_bytes(bytes, text.as_bytes(), limit);
        };
        if (! append("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: "_str) ||
            ! append(protocol.as_str()) || ! append("\r\n"_str))
            return Err(UpgradeError::TooLarge);
        for (const auto& header : headers)
            if (! append(header.name.as_str()) || ! append(": "_str) ||
                ! append_bytes(bytes, header.value.as_slice(), limit) || ! append("\r\n"_str))
                return Err(UpgradeError::TooLarge);
        if (! append("\r\n"_str)) return Err(UpgradeError::TooLarge);
        return Ok(rstd::bytes::Bytes::copy_from_slice(bytes.as_slice()));
    }
};
} // namespace lihttpto
