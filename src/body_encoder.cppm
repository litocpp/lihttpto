export module lihttpto:body_encoder;
export import :body_frame;
import :response;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
export namespace lihttpto
{
struct EncodedBody {
    Bytes bytes;
    usize payload_offset {}, payload_length {};
};
class BodyEncoder {
    enum class State
    {
        Body,
        Trailers,
        Finished,
        Failed
    };
    BodyFraming framing_;
    State       state_ { State::Body };
    u64         sent_ {};
    usize       metadata_limit_;
    Option<u64> expected_;
    template<class T = Bytes>
    auto fail(ResponseError error) -> Result<T, ResponseError> {
        state_ = State::Failed;
        return Err(error);
    }

public:
    explicit BodyEncoder(BodyFraming framing, usize limit = usize(65536), Option<u64> expected = {})
        : framing_(framing), metadata_limit_(limit), expected_(expected) {}
    auto remaining() const -> Option<u64> {
        if (expected_.is_some()) return Some(*expected_ - sent_);
        if (framing_.kind == BodyKind::FixedLength) return Some(framing_.length - sent_);
        return None();
    }
    auto encode_piece(BodyFrame frame) -> Result<EncodedBody, ResponseError> {
        if (state_ != State::Body) return fail<EncodedBody>(ResponseError::InvalidState);
        if (frame.is_Trailers()) {
            if (expected_.is_some() && sent_ != *expected_)
                return fail<EncodedBody>(ResponseError::InvalidBody);
            if (framing_.kind != BodyKind::Chunked ||
                validate_trailers(frame.as_Trailers().headers).is_err())
                return fail<EncodedBody>(ResponseError::InvalidTrailer);
            Vec<u8> output;
            if (! append_bytes(output, "0\r\n"_str.as_bytes(), metadata_limit_))
                return fail<EncodedBody>(ResponseError::TooLarge);
            for (const auto& field : frame.as_Trailers().headers) {
                if (! append_bytes(output, field.name.as_str().as_bytes(), metadata_limit_) ||
                    ! append_bytes(output, ": "_str.as_bytes(), metadata_limit_) ||
                    ! append_bytes(output, field.value.as_slice(), metadata_limit_) ||
                    ! append_bytes(output, "\r\n"_str.as_bytes(), metadata_limit_))
                    return fail<EncodedBody>(ResponseError::TooLarge);
            }
            if (! append_bytes(output, "\r\n"_str.as_bytes(), metadata_limit_))
                return fail<EncodedBody>(ResponseError::TooLarge);
            state_ = State::Trailers;
            return Ok(EncodedBody { Bytes::copy_from_slice(output.as_slice()) });
        }
        auto bytes  = rstd::move(frame.as_Data().bytes);
        auto length = u64(bytes.len().to_primitive());
        if (length == u64()) return Ok(EncodedBody {});
        if (framing_.kind == BodyKind::None || length > u64::MAX - sent_ ||
            (framing_.kind == BodyKind::FixedLength && length > framing_.length - sent_) ||
            (expected_.is_some() && (sent_ > *expected_ || length > *expected_ - sent_)))
            return fail<EncodedBody>(ResponseError::InvalidBody);
        sent_ += length;
        if (framing_.kind != BodyKind::Chunked) {
            auto size = bytes.len();
            return Ok(EncodedBody { rstd::move(bytes), usize(), size });
        }
        auto prefix = rstd::format("{:x}\r\n", length);
        if (bytes.len() > usize::MAX - prefix.len() - usize(2))
            return fail<EncodedBody>(ResponseError::TooLarge);
        auto output = Vec<u8>::with_capacity(prefix.len() + bytes.len() + usize(2));
        (void)append_bytes(output, prefix.as_str().as_bytes(), usize::MAX);
        (void)append_bytes(output, bytes.as_slice(), usize::MAX);
        (void)append_bytes(output, "\r\n"_str.as_bytes(), usize::MAX);
        return Ok(EncodedBody { Bytes::copy_from_slice(output.as_slice()),
                                prefix.len(),
                                usize(length.to_primitive()) });
    }
    auto encode(BodyFrame frame) -> Result<Bytes, ResponseError> {
        auto piece = encode_piece(rstd::move(frame));
        if (piece.is_err()) return Err(rstd::move(piece).unwrap_err());
        return Ok(rstd::move(piece->bytes));
    }
    auto finish() -> Result<Bytes, ResponseError> {
        if (state_ != State::Body && state_ != State::Trailers)
            return fail(ResponseError::InvalidState);
        if ((framing_.kind == BodyKind::FixedLength && sent_ != framing_.length) ||
            (expected_.is_some() && sent_ != *expected_))
            return fail(ResponseError::InvalidBody);
        bool terminate = framing_.kind == BodyKind::Chunked && state_ == State::Body;
        state_         = State::Finished;
        return Ok(terminate ? Bytes::copy_from_slice("0\r\n\r\n"_str.as_bytes()) : Bytes {});
    }
};
} // namespace lihttpto
