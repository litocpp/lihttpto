export module lihttpto:event_stream;
export import rstd;

namespace lihttpto
{
using namespace rstd::prelude;
using namespace rstd::literals;

export enum class EventStreamError { InvalidUtf8, LimitExceeded, Closed };
export struct EventStreamLimits {
    usize line_bytes = usize(1024 * 1024);
    usize data_bytes = usize(4 * 1024 * 1024);
};
export struct ServerEvent {
    String type;
    String data;
    String id;
};
export struct EventStreamProgress {
    usize               consumed;
    Option<ServerEvent> event;
};

// Strict UTF-8 framing only; transport status, reconnection and JSON belong to callers.
export class EventStreamDecoder {
    EventStreamLimits limits_;
    Vec<u8>           line_;
    String            data_;
    String            type_;
    String            id_;
    Option<u64>       retry_ms_;
    bool              first_line_ = true;
    bool              after_cr_   = false;
    bool              closed_     = false;

    auto process_line() -> Result<Option<ServerEvent>, EventStreamError> {
        auto decoded = rstd::str_::from_utf8(line_.as_slice());
        if (decoded.is_err()) return Err(EventStreamError::InvalidUtf8);
        auto line = *decoded;
        if (first_line_) {
            first_line_   = false;
            auto stripped = line.strip_prefix("\xEF\xBB\xBF"_str);
            if (stripped.is_some()) line = *stripped;
        }
        if (line.is_empty()) {
            if (data_.is_empty()) {
                type_.clear();
                return Ok(None());
            }
            data_.truncate(data_.len() - usize(1));
            ServerEvent event { type_.is_empty() ? String::make("message"_str) : rstd::move(type_),
                                rstd::move(data_),
                                String::make(id_.as_str()) };
            type_ = String();
            data_ = String();
            return Ok(Some(rstd::move(event)));
        }
        if (line.starts_with(":"_str)) return Ok(None());
        auto field = line;
        auto value = ""_str;
        auto pair  = line.split_once(":"_str);
        if (pair.is_some()) {
            field         = pair->template get<0>();
            value         = pair->template get<1>();
            auto stripped = value.strip_prefix(" "_str);
            if (stripped.is_some()) value = *stripped;
        }
        if (field == "data"_str) {
            if (data_.len() >= limits_.data_bytes ||
                value.len() >= limits_.data_bytes - data_.len())
                return Err(EventStreamError::LimitExceeded);
            data_.push_str(value);
            data_.push_ascii('\n');
        } else if (field == "event"_str) {
            type_ = String::make(value);
        } else if (field == "id"_str) {
            if (! value.contains("\0"_str)) id_ = String::make(value);
        } else if (field == "retry"_str && ! value.is_empty()) {
            for (auto byte : value.bytes())
                if (byte < u8('0') || byte > u8('9')) return Ok(None());
            u64 milliseconds {};
            for (auto byte : value.bytes()) {
                auto digit = u64(byte.to_primitive() - '0');
                if (milliseconds > (u64(18446744073709551615ULL) - digit) / u64(10))
                    return Err(EventStreamError::LimitExceeded);
                milliseconds = milliseconds * u64(10) + digit;
            }
            retry_ms_ = Some(milliseconds);
        }
        return Ok(None());
    }

public:
    explicit EventStreamDecoder(EventStreamLimits limits = {}): limits_(limits) {}

    auto retry_ms() const -> Option<u64> { return retry_ms_; }

    // Returns at most one event; the caller retains and feeds the unconsumed suffix.
    auto feed(slice<u8> bytes) -> Result<EventStreamProgress, EventStreamError> {
        if (closed_) return Err(EventStreamError::Closed);
        usize consumed {};
        for (auto byte : bytes) {
            ++consumed;
            if (after_cr_) {
                after_cr_ = false;
                if (byte == u8('\n')) continue;
            }
            if (byte == u8('\r') || byte == u8('\n')) {
                after_cr_  = byte == u8('\r');
                auto event = process_line();
                line_.clear();
                if (event.is_err()) {
                    closed_ = true;
                    return Err(rstd::move(event).unwrap_err());
                }
                if (event->is_some())
                    return Ok(EventStreamProgress { consumed, rstd::move(*event) });
            } else {
                if (line_.len() >= limits_.line_bytes) {
                    closed_ = true;
                    return Err(EventStreamError::LimitExceeded);
                }
                line_.push(u8(byte));
            }
        }
        return Ok(EventStreamProgress { consumed, None() });
    }

    // EOF never dispatches an unterminated event.
    auto finish() -> Result<empty, EventStreamError> {
        if (closed_) return Err(EventStreamError::Closed);
        closed_    = true;
        auto valid = rstd::str_::from_utf8(line_.as_slice()).is_ok();
        line_.clear();
        data_.clear();
        type_.clear();
        if (! valid) return Err(EventStreamError::InvalidUtf8);
        return Ok(empty {});
    }
};
} // namespace lihttpto
