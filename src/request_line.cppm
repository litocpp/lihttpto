module;
#include <rstd/macro.hpp>
export module lihttpto:request_line;
export import :request_target;

import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace lihttpto
{

enum class Version
{
    Http10,
    Http11
};

struct RequestLine {
    String        method;
    String        target;
    Version       version;
    RequestTarget resource;
};

} // namespace lihttpto

namespace lihttpto
{

auto parse_request_line(slice<u8> bytes) -> Result<RequestLine, DecodeError> {
    rstd::parse::TextCursor cursor { rstd::parse::Input<u8>(bytes) };
    auto                    method = rstd::parse::consume_while_one(cursor, token_byte);
    if (method.is_none() || rstd::parse::consume_literal(cursor, " "_str).is_none())
        return Err(DecodeError::InvalidLine);
    auto target = rstd::parse::consume_while_one(cursor, [](u8 byte) {
        return byte >= u8(0x21) && byte <= u8(0x7e);
    });
    if (target.is_none() || rstd::parse::consume_literal(cursor, " "_str).is_none())
        return Err(DecodeError::InvalidLine);
    Version version;
    if (rstd::parse::consume_literal(cursor, "HTTP/1.1"_str).is_some())
        version = Version::Http11;
    else if (rstd::parse::consume_literal(cursor, "HTTP/1.0"_str).is_some())
        version = Version::Http10;
    else
        return Err(DecodeError::UnsupportedVersion);
    if (! cursor.is_eof()) return Err(DecodeError::InvalidLine);
    auto resource =
        rstd_try(parse_request_target(cursor.text(*method).unwrap(), cursor.view(*target)));
    return Ok(RequestLine { String::make(cursor.text(*method).unwrap()),
                            String::make(cursor.text(*target).unwrap()),
                            version,
                            rstd::move(resource) });
}

} // namespace lihttpto

export namespace lihttpto
{

class RequestLineDecoder {
    enum class State
    {
        Reading,
        Complete,
        Failed,
        Taken
    };
    State               state_ { State::Reading };
    CrlfLineReader      reader_;
    Option<RequestLine> line_;
    DecodeError         error_ { DecodeError::InvalidState };

    auto fail(DecodeError error) -> Result<DecodeProgress, DecodeError> {
        state_ = State::Failed;
        error_ = error;
        return Err(error);
    }

public:
    // The limit includes CRLF; no caller-owned bytes survive feed().
    explicit RequestLineDecoder(usize limit = usize(8192)): reader_(limit) {}

    auto feed(slice<u8> input, bool final = false) -> Result<DecodeProgress, DecodeError> {
        if (state_ == State::Failed) return Err(error_);
        if (state_ == State::Taken) return Err(DecodeError::InvalidState);
        if (state_ == State::Complete)
            return Ok(DecodeProgress { usize(), DecodeStatus::Complete });
        auto progress = reader_.feed(input, final);
        if (progress.is_err()) return fail(progress.unwrap_err());
        if (progress->status == DecodeStatus::Complete) {
            auto parsed = parse_request_line(reader_.line());
            if (parsed.is_err()) return fail(parsed.unwrap_err());
            line_  = Some(rstd::move(parsed).unwrap());
            state_ = State::Complete;
        }
        return progress;
    }

    auto take() -> Option<RequestLine> {
        if (state_ != State::Complete) return None();
        state_ = State::Taken;
        return line_.take();
    }

    void reset() {
        reader_.reset();
        line_  = None();
        state_ = State::Reading;
    }
};

} // namespace lihttpto
