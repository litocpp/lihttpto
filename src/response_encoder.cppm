export module lihttpto:response_encoder;
export import :body_encoder;
export import :response;
export import :message;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;

export namespace lihttpto
{
// Owns framing state only; transport, deadlines and stream errors belong to the driver.
class ResponseEncoder {
    Bytes       head_;
    BodyFraming framing_;
    bool        keep_alive_;
    bool        started_ {};
    bool        failed_ {};
    BodyEncoder body_;
    ResponseEncoder(Bytes head, BodyFraming framing, bool keep_alive, usize limit)
        : head_(rstd::move(head)),
          framing_(framing),
          keep_alive_(keep_alive),
          body_(framing, limit) {}

public:
    ResponseEncoder(const ResponseEncoder&)                        = delete;
    ResponseEncoder(ResponseEncoder&&) noexcept                    = default;
    auto operator=(ResponseEncoder&&) noexcept -> ResponseEncoder& = default;

    static auto make(const ResponseHead& head,
                     const Method&       method,
                     Version             version,
                     Option<u64>         length         = {},
                     bool                keep_alive     = true,
                     usize               metadata_limit = usize(65536))
        -> Result<ResponseEncoder, ResponseError> {
        if (head.version.is_some() &&
            (head.version->major() != u8(1) ||
             head.version->minor() != (version == Version::Http11 ? u8(1) : u8())))
            return Err(ResponseError::InvalidHeader);
        auto        status  = head.status.value();
        auto        omitted = omit_response_body(status, method.as_ref());
        BodyFraming framing;
        if (! omitted) {
            if (length.is_some())
                framing = { BodyKind::FixedLength, *length };
            else
                framing.kind = version == Version::Http11 ? BodyKind::Chunked : BodyKind::UntilEof;
        }
        if (framing.kind == BodyKind::UntilEof) keep_alive = false;
        auto bytes = encode_response_metadata(
            status,
            head.headers.as_slice(),
            length,
            method.as_ref(),
            version,
            keep_alive,
            metadata_limit,
            framing.kind == BodyKind::Chunked,
            head.reason.is_some()
                ? Some(ref<HeaderValue>::from_raw_parts(rstd::addressof(*head.reason)))
                : None<ref<HeaderValue>>());
        if (bytes.is_err()) return Err(bytes.unwrap_err());
        return Ok(ResponseEncoder(rstd::move(bytes).unwrap(), framing, keep_alive, metadata_limit));
    }
    auto framing() const -> BodyFraming { return framing_; }
    auto keep_alive() const -> bool { return keep_alive_; }
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
} // namespace lihttpto
