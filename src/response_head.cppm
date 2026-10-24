export module lihttpto:response_head;
export import :message;
export import :request_head;
using namespace rstd::prelude;
using namespace rstd::literals;
export namespace lihttpto
{
auto response_body_framing(const ResponseHead& response, const Method& method)
    -> Result<BodyFraming, DecodeError> {
    auto parsed_length = content_length(response.headers);
    if (parsed_length.is_err()) return Err(parsed_length.unwrap_err());
    auto length  = *parsed_length;
    bool chunked = false;
    auto status  = response.status.value();
    for (const auto& header : response.headers) {
        if (header.name.matches("transfer-encoding"_str)) {
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

struct DecodedResponseHead {
    ResponseHead head;
    BodyFraming  framing;
    bool         keep_alive;
    bool         informational;
    usize        input_consumed;
};
class ResponseHeadDecoder {
    Method          method_;
    usize           limit_;
    usize           used_ {};
    usize           informational_ {};
    usize           max_informational_;
    Http1HeadParser parser_;
    bool            finished_ {};
    bool            failed_ {};

public:
    explicit ResponseHeadDecoder(Method method,
                                 usize  limit         = usize(65536),
                                 usize  informational = usize(16))
        : method_(rstd::move(method)),
          limit_(limit),
          max_informational_(informational),
          parser_(true, limit) {}
    auto metadata_bytes() const -> usize { return used_; }
    auto feed(slice<u8> bytes) -> Result<Option<DecodedResponseHead>, DecodeError> {
        if (finished_ || failed_) return Err(DecodeError::InvalidState);
        auto parsed = parser_.push(bytes);
        if (parsed.is_err()) {
            failed_ = true;
            return Err(DecodeError::InvalidHeader);
        }
        if (parsed->is_NeedMore()) return Ok(None());
        auto event = rstd::move(parsed->as_Complete());
        used_ += event.consumed;
        auto response = rstd::move(event.head).into_response();
        if (response.is_err()) {
            failed_ = true;
            return Err(DecodeError::InvalidHeader);
        }
        auto head = rstd::move(response).unwrap();
        if (head.version.is_none() || head.version->major() != u8(1) ||
            head.version->minor() > u8(1))
            return Err(DecodeError::UnsupportedVersion);
        auto framing = response_body_framing(head, method_);
        if (framing.is_err()) {
            failed_ = true;
            return Err(framing.unwrap_err());
        }
        bool close = false, persistent = false, upgrade = false;
        for (const auto& field : head.headers)
            if (field.name.matches("connection"_str) &&
                ! parse_connection(field.value.as_slice(), close, persistent, &upgrade))
                return Err(DecodeError::InvalidHeader);
        if (head.status.value() == u16(101) || upgrade) return Err(DecodeError::UnsupportedTarget);
        bool informational = head.status.value() < u16(200);
        bool keep_alive =
            head.version->minor() == u8(1) && ! close && framing->kind != BodyKind::UntilEof;
        if (informational) {
            if (++informational_ > max_informational_ || used_ >= limit_) {
                failed_ = true;
                return Err(DecodeError::TooLarge);
            }
            parser_ = Http1HeadParser(true, limit_ - used_);
        } else
            finished_ = true;
        return Ok(Some(DecodedResponseHead {
            rstd::move(head), *framing, keep_alive, informational, event.input_consumed }));
    }
    auto finish() -> Result<empty, DecodeError> {
        return finished_ ? Ok(empty {}) : Result<empty, DecodeError>(Err(DecodeError::Truncated));
    }
};
} // namespace lihttpto
