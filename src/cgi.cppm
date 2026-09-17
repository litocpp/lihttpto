module;
#include <rstd/enum.hpp>
#include <rstd/macro.hpp>
export module lihttpto.cgi;
export import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using Bytes = rstd::bytes::Bytes;

export namespace lihttpto::cgi
{
class DecodeError {
    RSTD_ENUM(DecodeError, (Invalid), (Unsupported), (TooLarge), (Truncated), (State))
};
// Close-delimited CGI headers only; redirects and explicit lengths are unsupported.
class HeaderDecoder {
    Vec<u8>                      line_;
    lihttpto::StreamResponseHead head_;
    usize                        bytes_ {};
    bool                         complete_ {}, status_seen_ {}, failed_ {}, taken_ {};
    auto                         line() -> Result<empty, DecodeError> {
        auto bytes = line_.as_slice();
        if (! bytes.is_empty() && bytes[bytes.len() - usize(1)] == u8('\r'))
            bytes = slice<u8>::from_raw_parts(bytes.as_raw_ptr(), bytes.len() - usize(1));
        if (bytes.is_empty()) {
            if (head_.headers.get_unique("Content-Type"_str).is_err())
                return Err(DecodeError::Invalid());
            auto checked = lihttpto::validate_response_head(head_, "GET"_str);
            if (checked.is_err()) return Err(DecodeError::Invalid());
            complete_ = true;
            return Ok(empty {});
        }
        auto field = lihttpto::Header::parse_line(bytes);
        if (field.is_err()) return Err(DecodeError::Invalid());
        if (field->name.matches("Location"_str) || field->name.matches("Content-Length"_str))
            return Err(DecodeError::Unsupported());
        if (field->name.matches("Status"_str)) {
            auto text = field->value.to_str();
            if (text.is_err()) return Err(DecodeError::Invalid());
            auto body = *text;
            if (status_seen_ || body.len() < usize(3)) return Err(DecodeError::Invalid());
            unsigned code {};
            for (usize i {}; i < usize(3); ++i) {
                auto c = body.as_bytes()[i];
                if (c < u8('0') || c > u8('9')) return Err(DecodeError::Invalid());
                code = code * 10 + (c - u8('0')).to_primitive();
            }
            if (body.len() > usize(3) && body.as_bytes()[usize(3)] != u8(' '))
                return Err(DecodeError::Invalid());
            head_.status = u16(code);
            status_seen_ = true;
        } else
            head_.headers.push(rstd::move(field).unwrap());
        return Ok(empty {});
    }

public:
    auto feed(slice<u8> bytes) -> Result<usize, DecodeError> {
        if (failed_ || taken_) return Err(DecodeError::State());
        usize consumed {};
        while (! complete_ && consumed < bytes.len()) {
            if (++bytes_ > usize(65536)) {
                failed_ = true;
                return Err(DecodeError::TooLarge());
            }
            auto c = bytes[consumed++];
            if (c == u8('\n')) {
                auto parsed = line();
                if (parsed.is_err()) {
                    failed_ = true;
                    return Err(rstd::move(parsed).unwrap_err());
                }
                line_.clear();
            } else
                line_.push(u8(c));
        }
        return Ok(consumed);
    }
    auto complete() const -> bool { return complete_; }
    auto take() -> Result<lihttpto::StreamResponseHead, DecodeError> {
        if (! complete_ || failed_ || taken_) return Err(DecodeError::State());
        taken_ = true;
        return Ok(rstd::move(head_));
    }
    auto finish() -> Result<empty, DecodeError> {
        if (failed_ || taken_) return Err(DecodeError::State());
        if (! complete_) {
            failed_ = true;
            return Err(DecodeError::Truncated());
        }
        return Ok(empty {});
    }
};

template<class SourceError>
class ResponseError {
    RSTD_ENUM(ResponseError, (Source, (SourceError error;)), (Decode, (DecodeError error;)))
};
template<BodySource Source>
class ResponseSource {
    Source&       source_;
    HeaderDecoder decoder_;
    Option<Bytes> pending_;
    bool          ready_ {}, ended_ {};

public:
    using Error = ResponseError<typename Source::Error>;
    explicit ResponseSource(Source& source): source_(source) {}
    auto read_head() -> rstd::async::coro<Result<StreamResponseHead, Error>> {
        if (ready_ || ended_) co_return Err(Error::Decode(DecodeError::State()));
        for (;;) {
            auto part = co_await source_.next();
            if (part.is_err()) {
                ended_ = true;
                co_return Err(Error::Source(rstd::move(part).unwrap_err()));
            }
            if (part->is_none()) {
                ended_ = true;
                co_return Err(Error::Decode(DecodeError::Truncated()));
            }
            auto bytes  = part->take().unwrap();
            auto parsed = decoder_.feed(bytes.as_slice());
            if (parsed.is_err()) {
                ended_ = true;
                co_return Err(Error::Decode(rstd::move(parsed).unwrap_err()));
            }
            if (! decoder_.complete()) continue;
            if (*parsed < bytes.len()) {
                bytes.advance(*parsed);
                pending_ = Some(rstd::move(bytes));
            }
            ready_ = true;
            co_return Ok(decoder_.take().unwrap());
        }
    }
    auto next() -> rstd::async::coro<Result<Option<Bytes>, Error>> {
        if (! ready_) co_return Err(Error::Decode(DecodeError::State()));
        if (pending_.is_some()) co_return Ok(pending_.take());
        if (ended_) co_return Ok<Option<Bytes>>(None());
        auto part = co_await source_.next();
        if (part.is_err()) {
            ended_ = true;
            co_return Err(Error::Source(rstd::move(part).unwrap_err()));
        }
        if (part->is_none()) ended_ = true;
        co_return Ok(rstd::move(part).unwrap());
    }
};
} // namespace lihttpto::cgi
