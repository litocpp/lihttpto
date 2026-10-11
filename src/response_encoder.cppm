export module lihttpto:response_encoder;
export import :body_frame;
export import :response;
export import :message;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;

export namespace lihttpto
{
// Owns framing state only; transport, deadlines and stream errors belong to the driver.
class ResponseEncoder {
    enum class State
    {
        Head,
        Body,
        Trailers,
        Finished,
        Failed
    };
    Bytes       head_;
    BodyFraming framing_;
    State       state_ { State::Head };
    bool        keep_alive_ {};
    u64         sent_ {};
    usize       metadata_limit_;

    ResponseEncoder(Bytes head, BodyFraming framing, bool keep_alive, usize limit)
        : head_(rstd::move(head)),
          framing_(framing),
          keep_alive_(keep_alive),
          metadata_limit_(limit) {}
    auto fail(ResponseError error) -> Result<Bytes, ResponseError> {
        state_ = State::Failed;
        return Err(error);
    }

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
        auto bytes = encode_response_metadata(status,
                                              head.headers.as_slice(),
                                              length,
                                              method.as_ref(),
                                              version,
                                              keep_alive,
                                              metadata_limit,
                                              framing.kind == BodyKind::Chunked);
        if (bytes.is_err()) return Err(bytes.unwrap_err());
        return Ok(ResponseEncoder(rstd::move(bytes).unwrap(), framing, keep_alive, metadata_limit));
    }
    auto framing() const -> BodyFraming { return framing_; }
    auto keep_alive() const -> bool { return keep_alive_; }
    auto take_head() -> Result<Bytes, ResponseError> {
        if (state_ != State::Head) return fail(ResponseError::InvalidState);
        state_ = State::Body;
        return Ok(rstd::exchange(head_, Bytes {}));
    }
    auto encode(BodyFrame frame) -> Result<Bytes, ResponseError> {
        if (state_ != State::Body) return fail(ResponseError::InvalidState);
        if (frame.is_Trailers()) {
            if (framing_.kind != BodyKind::Chunked ||
                validate_trailers(frame.as_Trailers().headers).is_err())
                return fail(ResponseError::InvalidTrailer);
            Vec<u8> output;
            if (! append_bytes(output, "0\r\n"_str.as_bytes(), metadata_limit_))
                return fail(ResponseError::TooLarge);
            for (const auto& field : frame.as_Trailers().headers) {
                if (! append_bytes(output, field.name.as_str().as_bytes(), metadata_limit_) ||
                    ! append_bytes(output, ": "_str.as_bytes(), metadata_limit_) ||
                    ! append_bytes(output, field.value.as_slice(), metadata_limit_) ||
                    ! append_bytes(output, "\r\n"_str.as_bytes(), metadata_limit_))
                    return fail(ResponseError::TooLarge);
            }
            if (! append_bytes(output, "\r\n"_str.as_bytes(), metadata_limit_))
                return fail(ResponseError::TooLarge);
            state_ = State::Trailers;
            return Ok(Bytes::copy_from_slice(output.as_slice()));
        }
        auto bytes  = rstd::move(frame.as_Data().bytes);
        auto length = u64(bytes.len().to_primitive());
        if (length == u64()) return Ok(Bytes {});
        if (framing_.kind == BodyKind::None || length > u64::MAX - sent_ ||
            (framing_.kind == BodyKind::FixedLength && length > framing_.length - sent_))
            return fail(ResponseError::InvalidBody);
        sent_ += length;
        if (framing_.kind != BodyKind::Chunked) return Ok(rstd::move(bytes));
        auto prefix = rstd::format("{:x}\r\n", length);
        if (bytes.len() > usize::MAX - prefix.len() - usize(2))
            return fail(ResponseError::TooLarge);
        auto output = Vec<u8>::with_capacity(prefix.len() + bytes.len() + usize(2));
        (void)append_bytes(output, prefix.as_str().as_bytes(), usize::MAX);
        (void)append_bytes(output, bytes.as_slice(), usize::MAX);
        (void)append_bytes(output, "\r\n"_str.as_bytes(), usize::MAX);
        return Ok(Bytes::copy_from_slice(output.as_slice()));
    }
    auto finish() -> Result<Bytes, ResponseError> {
        if (state_ != State::Body && state_ != State::Trailers)
            return fail(ResponseError::InvalidState);
        if (framing_.kind == BodyKind::FixedLength && sent_ != framing_.length)
            return fail(ResponseError::InvalidBody);
        bool terminate = framing_.kind == BodyKind::Chunked && state_ == State::Body;
        state_         = State::Finished;
        return Ok(terminate ? Bytes::copy_from_slice("0\r\n\r\n"_str.as_bytes()) : Bytes {});
    }
};
} // namespace lihttpto
