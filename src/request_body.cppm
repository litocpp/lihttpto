export module lihttpto:request_body;
export import :request_head;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
struct BodyLimits {
    u64   data_bytes { 16 * 1024 * 1024 };
    usize chunk_line { 8192 };
    usize trailer_line { 8192 };
    usize metadata_bytes { 1024 * 1024 };
    usize trailer_fields { 100 };
};
struct BodyProgress {
    usize consumed;
    // Valid only while the caller keeps the input storage unchanged and alive.
    slice<u8>    data;
    DecodeStatus status;
};
} // namespace lihttpto

namespace lihttpto
{
auto chunk_size(slice<u8> bytes) -> Result<u64, DecodeError> {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    auto digits = rstd::parse::consume_while_one(cursor, rstd::parse::ascii::hex_digit);
    if (digits.is_none()) return Err(DecodeError::InvalidLine);
    u64 size {};
    for (auto byte : cursor.view(*digits)) {
        auto digit = u64(rstd::ascii::digit_value(byte, u8(16))->to_primitive());
        if (size > (u64::MAX - digit) / u64(16)) return Err(DecodeError::TooLarge);
        size = size * u64(16) + digit;
    }
    while (! cursor.is_eof()) {
        (void)rstd::parse::consume_while(cursor, ows);
        if (rstd::parse::consume_literal(cursor, ";"_str).is_none())
            return Err(DecodeError::InvalidLine);
        (void)rstd::parse::consume_while(cursor, ows);
        if (rstd::parse::consume_while_one(cursor, token_byte).is_none())
            return Err(DecodeError::InvalidLine);
        auto after_name = cursor.checkpoint();
        (void)rstd::parse::consume_while(cursor, ows);
        if (rstd::parse::consume_literal(cursor, "="_str).is_none()) {
            cursor.rewind(after_name);
            continue;
        }
        (void)rstd::parse::consume_while(cursor, ows);
        if (rstd::parse::consume_literal(cursor, "\""_str).is_some()) {
            bool closed = false;
            while (auto next = cursor.take()) {
                auto byte = next->get();
                if (byte == u8('"')) {
                    closed = true;
                    break;
                }
                if (byte == u8('\\')) {
                    auto escaped = cursor.take();
                    if (escaped.is_none()) return Err(DecodeError::InvalidLine);
                    byte = escaped->get();
                }
                if (! (byte == u8('\t') || (byte >= u8(32) && byte != u8(127))))
                    return Err(DecodeError::InvalidLine);
            }
            if (! closed) return Err(DecodeError::InvalidLine);
        } else if (rstd::parse::consume_while_one(cursor, token_byte).is_none())
            return Err(DecodeError::InvalidLine);
    }
    return Ok(size);
}

auto forbidden_trailer(ref<str> name) -> bool {
    auto forbidden = array<ref<str>, 13> {
        "host"_str,         "content-length"_str,   "transfer-encoding"_str,   "connection"_str,
        "trailer"_str,      "authorization"_str,    "proxy-authorization"_str, "cookie"_str,
        "content-type"_str, "content-encoding"_str, "content-range"_str,       "expect"_str,
        "upgrade"_str
    };
    for (auto field : forbidden)
        if (ascii_equal(name.as_bytes(), field)) return true;
    return false;
}
} // namespace lihttpto

export namespace lihttpto
{
class RequestBodyDecoder {
    enum class State
    {
        Fixed,
        Size,
        Data,
        DataCr,
        DataLf,
        Trailers,
        Complete,
        Failed
    };
    State          state_;
    BodyLimits     limits_;
    u64            remaining_ {};
    u64            total_ {};
    usize          metadata_ {};
    CrlfLineReader size_line_;
    CrlfLineReader trailer_line_;
    Vec<Header>    trailers_;
    bool           trailers_taken_ { false };
    DecodeError    error_ { DecodeError::InvalidState };

    auto fail(DecodeError error) -> Result<BodyProgress, DecodeError> {
        state_ = State::Failed;
        error_ = error;
        return Err(error);
    }

public:
    explicit RequestBodyDecoder(BodyFraming framing, BodyLimits limits = {})
        : state_(framing.kind == BodyKind::Chunked ? State::Size : State::Complete),
          limits_(limits),
          size_line_(limits.chunk_line),
          trailer_line_(limits.trailer_line) {
        if (framing.kind == BodyKind::FixedLength) {
            remaining_ = framing.length;
            if (remaining_ > limits.data_bytes) {
                state_ = State::Failed;
                error_ = DecodeError::TooLarge;
            } else if (remaining_ != u64())
                state_ = State::Fixed;
        }
    }

    auto feed(slice<u8> input) -> Result<BodyProgress, DecodeError> {
        if (state_ == State::Failed) return Err(error_);
        if (state_ == State::Complete)
            return Ok(BodyProgress { usize(), {}, DecodeStatus::Complete });
        rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(input) };
        while (! cursor.is_eof()) {
            if (state_ == State::Fixed || state_ == State::Data) {
                auto count = cursor.remaining();
                if (u64(count.to_primitive()) > remaining_)
                    count = usize(remaining_.to_primitive());
                auto begin = cursor.checkpoint();
                (void)cursor.advance(count);
                remaining_ -= u64(count.to_primitive());
                total_ += u64(count.to_primitive());
                if (remaining_ == u64())
                    state_ = state_ == State::Fixed ? State::Complete : State::DataCr;
                return Ok(BodyProgress { cursor.position(),
                                         cursor.consumed(begin),
                                         state_ == State::Complete ? DecodeStatus::Complete
                                                                   : DecodeStatus::NeedMore });
            }
            if (metadata_ >= limits_.metadata_bytes) return fail(DecodeError::TooLarge);
            if (state_ == State::DataCr || state_ == State::DataLf) {
                auto byte = cursor.take()->get();
                ++metadata_;
                if (byte != (state_ == State::DataCr ? u8('\r') : u8('\n')))
                    return fail(DecodeError::InvalidLine);
                state_ = state_ == State::DataCr ? State::DataLf : State::Size;
                continue;
            }
            auto fragment  = cursor.remaining_input();
            auto available = limits_.metadata_bytes - metadata_;
            if (fragment.len() > available)
                fragment = slice<u8>::from_raw_parts(fragment.as_raw_ptr(), available);
            auto progress =
                state_ == State::Size ? size_line_.feed(fragment) : trailer_line_.feed(fragment);
            if (progress.is_err()) return fail(progress.unwrap_err());
            (void)cursor.advance(progress->consumed);
            metadata_ += progress->consumed;
            if (progress->status == DecodeStatus::NeedMore) continue;
            if (state_ == State::Size) {
                auto size = chunk_size(size_line_.line());
                if (size.is_err()) return fail(size.unwrap_err());
                if (*size > limits_.data_bytes - total_) return fail(DecodeError::TooLarge);
                remaining_ = *size;
                size_line_.reset();
                state_ = remaining_ == u64() ? State::Trailers : State::Data;
            } else {
                auto bytes = trailer_line_.line();
                if (bytes.is_empty()) {
                    state_ = State::Complete;
                    return Ok(BodyProgress { cursor.position(), {}, DecodeStatus::Complete });
                }
                if (trailers_.len() >= limits_.trailer_fields) return fail(DecodeError::TooLarge);
                auto field = parse_header(bytes);
                if (field.is_err()) return fail(field.unwrap_err());
                if (forbidden_trailer(field->name.as_str()))
                    return fail(DecodeError::InvalidHeader);
                trailers_.push(rstd::move(field).unwrap());
                trailer_line_.reset();
            }
        }
        return Ok(BodyProgress { cursor.position(), {}, DecodeStatus::NeedMore });
    }

    auto finish() -> Result<DecodeStatus, DecodeError> {
        if (state_ == State::Failed) return Err(error_);
        if (state_ != State::Complete) {
            state_ = State::Failed;
            error_ = DecodeError::Truncated;
            return Err(error_);
        }
        return Ok(DecodeStatus::Complete);
    }

    auto take_trailers() -> Option<Vec<Header>> {
        if (state_ != State::Complete || trailers_taken_) return None();
        trailers_taken_ = true;
        return Some(rstd::move(trailers_));
    }
};
} // namespace lihttpto
