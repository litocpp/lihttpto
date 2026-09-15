export module lihttpto:syntax;
export import rstd.parse;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{
enum class DecodeError
{
    InvalidLine,
    UnsupportedVersion,
    TooLarge,
    Truncated,
    InvalidState,
    InvalidHeader,
    InvalidHost,
    InvalidTarget,
    UnsupportedTarget,
    InvalidContentLength,
    AmbiguousFraming,
    UnsupportedTransferEncoding
};
enum class DecodeStatus
{
    NeedMore,
    Complete
};
struct DecodeProgress {
    usize        consumed;
    DecodeStatus status;
};
} // namespace lihttpto

namespace lihttpto
{
auto token_byte(u8 byte) -> bool {
    return rstd::ascii::is_alnum(byte) || rstd::parse::one_of(u8('!'),
                                                              u8('#'),
                                                              u8('$'),
                                                              u8('%'),
                                                              u8('&'),
                                                              u8('\''),
                                                              u8('*'),
                                                              u8('+'),
                                                              u8('-'),
                                                              u8('.'),
                                                              u8('^'),
                                                              u8('_'),
                                                              u8('`'),
                                                              u8('|'),
                                                              u8('~'))(byte);
}

auto ascii_lower(u8 byte) -> u8 {
    return byte >= u8('A') && byte <= u8('Z') ? byte + u8(32) : byte;
}

auto ascii_equal(slice<u8> bytes, ref<str> expected) -> bool {
    if (bytes.len() != expected.len()) return false;
    for (usize index {}; index < bytes.len(); ++index)
        if (ascii_lower(bytes[index]) != ascii_lower(expected.as_bytes()[index])) return false;
    return true;
}

class CrlfLineReader {
    enum class State
    {
        Reading,
        CarriageReturn,
        Complete,
        Failed
    };
    State       state_ { State::Reading };
    usize       limit_;
    Vec<u8>     bytes_;
    DecodeError error_ { DecodeError::InvalidState };

    auto fail(DecodeError error) -> Result<DecodeProgress, DecodeError> {
        state_ = State::Failed;
        error_ = error;
        return Err(error);
    }

public:
    explicit CrlfLineReader(usize limit): limit_(limit) {}
    CrlfLineReader(const CrlfLineReader&)                    = delete;
    CrlfLineReader(CrlfLineReader&&)                         = delete;
    auto operator=(const CrlfLineReader&) -> CrlfLineReader& = delete;
    auto operator=(CrlfLineReader&&) -> CrlfLineReader&      = delete;

    auto feed(slice<u8> input, bool final = false) -> Result<DecodeProgress, DecodeError> {
        if (state_ == State::Failed) return Err(error_);
        if (state_ == State::Complete)
            return Ok(DecodeProgress { usize(), DecodeStatus::Complete });
        rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(input) };
        while (auto next = cursor.take()) {
            auto byte = next->get();
            if (bytes_.len() >= limit_) return fail(DecodeError::TooLarge);
            bytes_.push(u8(byte));
            if (state_ == State::CarriageReturn) {
                if (byte != u8('\n')) return fail(DecodeError::InvalidLine);
                state_ = State::Complete;
                return Ok(DecodeProgress { cursor.position(), DecodeStatus::Complete });
            }
            if (byte == u8('\n')) return fail(DecodeError::InvalidLine);
            if (byte == u8('\r')) state_ = State::CarriageReturn;
        }
        if (final) return fail(DecodeError::Truncated);
        return Ok(DecodeProgress { cursor.position(), DecodeStatus::NeedMore });
    }

    auto line() const -> slice<u8> {
        if (state_ != State::Complete) rstd::panic("line is not complete");
        return slice<u8>::from_raw_parts(bytes_.as_slice().as_raw_ptr(), bytes_.len() - usize(2));
    }
    void reset() {
        bytes_.clear();
        state_ = State::Reading;
    }
};
} // namespace lihttpto
