module;
#include <rstd/enum.hpp>
#include <rstd/macro.hpp>
export module lihttpto:http2;
export import :hpack;
export import :request_encoder;
export import :response;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
namespace lihttpto::h2
{
auto word(slice<u8> bytes, usize at = usize()) -> unsigned {
    return (unsigned(bytes[at].to_primitive()) << 24) |
           (unsigned(bytes[at + usize(1)].to_primitive()) << 16) |
           (unsigned(bytes[at + usize(2)].to_primitive()) << 8) |
           bytes[at + usize(3)].to_primitive();
}
void word(Vec<u8>& bytes, unsigned value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push(u8((value >> shift) & 255));
}
export auto frame(u8 type, u8 flags, u32 stream, Bytes payload) -> Bytes {
    Vec<u8> bytes;
    auto    n = payload.len().to_primitive();
    bytes.push(u8((n >> 16) & 255));
    bytes.push(u8((n >> 8) & 255));
    bytes.push(u8(n & 255));
    bytes.push(u8(type));
    bytes.push(u8(flags));
    word(bytes, stream.to_primitive() & 0x7fffffff);
    bytes.extend_from_slice(payload.as_slice());
    return Bytes::from_vec(rstd::move(bytes));
}
export struct Frame {
    u8    type, flags;
    u32   stream;
    Bytes payload;
};
export class FrameDecoder {
    Vec<u8> pending_;

public:
    auto feed(Bytes bytes) -> Result<Vec<Frame>> {
        if (pending_.len() + bytes.len() > usize(1048576)) return Err(Error::TooLarge());
        pending_.extend_from_slice(bytes.as_slice());
        Vec<Frame> frames;
        usize      at;
        while (pending_.len() - at >= usize(9)) {
            auto     b = pending_.as_slice();
            unsigned n = (unsigned(b[at].to_primitive()) << 16) |
                         (unsigned(b[at + usize(1)].to_primitive()) << 8) |
                         b[at + usize(2)].to_primitive();
            if (n > 16384) return Err(Error::Connection(u32(6)));
            if (pending_.len() - at < usize(n + 9)) break;
            auto payload = Bytes::copy_from_slice(
                slice<u8>::from_raw_parts(b.as_raw_ptr() + at.to_primitive() + 9, usize(n)));
            frames.push({ u8(b[at + usize(3)]),
                          u8(b[at + usize(4)]),
                          u32(word(b, at + usize(5)) & 0x7fffffff),
                          rstd::move(payload) });
            at += usize(n + 9);
        }
        if (at != usize()) {
            auto b   = pending_.as_slice();
            pending_ = Vec<u8>::from(
                slice<u8>::from_raw_parts(b.as_raw_ptr() + at.to_primitive(), b.len() - at));
        }
        return Ok(rstd::move(frames));
    }
};
export class Event {
    RSTD_ENUM(Event,
              (Head, (u32 stream; ResponseHead head; bool informational;)),
              (Data, (u32 stream; Bytes bytes;)),
              (Trailers, (u32 stream; Headers headers;)),
              (End, (u32 stream;)),
              (Reset, (u32 stream; u32 code;)),
              (Goaway, (u32 last_stream; u32 code;)))
};
export struct Limits {
    usize header_bytes { 65536 };
    usize maximum_streams { 128 };
    usize receive_window { 65536 };
    usize maximum_informational { 16 };
};
struct Stream {
    unsigned    id;
    long long   send_window;
    unsigned    receive_window;
    unsigned    outstanding {};
    Option<u64> expected;
    u64         received {};
    bool        head {}, final {}, local_end {}, remote_end {}, reset {};
    usize       informational {};
};
export struct Outbound {
    Bytes bytes;
    u32   stream;
    usize payload_offset {}, payload_length {};
};
export class ClientConnection {
    Limits        limits_;
    FrameDecoder  frames_;
    HpackDecoder  decoder_;
    Vec<Stream>   streams_;
    Vec<Outbound> output_;
    Vec<Event>    events_;
    Bytes         block_;
    unsigned continuation_ {}, next_ { 1 }, peer_maximum_ { 0xffffffff }, peer_window_ { 65535 };
    unsigned connection_receive_ { 65535 }, connection_outstanding_ {}, peer_frame_ { 16384 },
        goaway_last_ { 0x7fffffff };
    long long connection_send_ { 65535 };
    bool      block_end_ {}, settings_ {}, settings_ack_ {}, draining_ {}, local_shutdown_ {};
    auto      find(unsigned id) -> Stream* {
        for (auto& s : streams_)
            if (s.id == id) return &s;
        return nullptr;
    }
    void emit(unsigned type, unsigned flags, unsigned id, Bytes bytes = {}) {
        if (local_shutdown_) return;
        auto size = type == 0 ? bytes.len() : usize();
        output_.push({ frame(u8(type), u8(flags), u32(id), rstd::move(bytes)),
                       u32(id),
                       type == 0 ? usize(9) : usize(),
                       size });
    }
    void update(unsigned id, unsigned n) {
        if (! n) return;
        Vec<u8> p;
        word(p, n);
        emit(8, 0, id, Bytes::from_vec(rstd::move(p)));
    }
    void release_received(Stream& s) {
        if (s.outstanding) {
            connection_receive_ += s.outstanding;
            connection_outstanding_ -= s.outstanding;
            update(0, s.outstanding);
            s.outstanding = 0;
        }
    }
    void reset_stream(Stream& s, unsigned code) {
        if (s.reset) return;
        release_received(s);
        Vec<u8> p;
        word(p, code);
        emit(3, 0, s.id, Bytes::from_vec(rstd::move(p)));
        s.reset     = true;
        s.local_end = s.remote_end = true;
        events_.push(Event::Reset(u32(s.id), u32(code)));
    }
    auto end(Stream& s) -> Result<empty> {
        if (s.expected.is_some() && ! s.head && s.received != *s.expected) {
            reset_stream(s, 1);
            return Ok(empty {});
        }
        s.remote_end = true;
        events_.push(Event::End(u32(s.id)));
        return Ok(empty {});
    }
    auto headers(unsigned id) -> Result<empty> {
        auto fields   = rstd_try(decoder_.decode(rstd::move(block_), limits_.header_bytes));
        block_        = {};
        continuation_ = 0;
        auto* s       = find(id);
        if (! s || s->reset || s->remote_end) {
            if (s) reset_stream(*s, 5);
            return Ok(empty {});
        }
        Headers            headers;
        Option<StatusCode> status;
        Option<u64>        length;
        bool               ordinary = false, invalid = false;
        for (auto& f : fields) {
            auto name = f.name.as_str();
            for (auto b : name.as_bytes())
                if (b >= u8('A') && b <= u8('Z')) invalid = true;
            if (name == ":status"_str) {
                if (ordinary || status.is_some() || s->final) {
                    invalid = true;
                    continue;
                }
                auto text   = rstd::str_::from_utf8(f.value.as_slice());
                auto parsed = text.is_ok() ? StatusCode::parse(*text) : StatusCode::make(u16());
                if (parsed.is_err() || f.value.len() != usize(3))
                    invalid = true;
                else
                    status = Some(*parsed);
                continue;
            }
            ordinary = true;
            if (name.is_empty() || name.as_bytes()[usize()] == u8(':')) {
                invalid = true;
                continue;
            }
            if (name == "connection"_str || name == "proxy-connection"_str ||
                name == "keep-alive"_str || name == "transfer-encoding"_str ||
                name == "upgrade"_str)
                invalid = true;
            auto value = f.value.as_slice();
            if (! value.is_empty() && (value[usize()] == u8(' ') || value[usize()] == u8('\t') ||
                                       value[value.len() - usize(1)] == u8(' ') ||
                                       value[value.len() - usize(1)] == u8('\t')))
                invalid = true;
            for (auto b : value)
                if (b == u8() || b == u8('\r') || b == u8('\n')) invalid = true;
            if (name == "te"_str) invalid = true;
            if (name == "content-length"_str) {
                auto parsed = parse_length(value);
                if (s->final || length.is_some() || parsed.is_err())
                    invalid = true;
                else
                    length = Some(*parsed);
            }
            auto added = Header::make(name, value);
            if (added.is_err())
                invalid = true;
            else
                headers.push(rstd::move(*added));
        }
        if (! s->final && status.is_none()) invalid = true;
        if (s->final && (! block_end_ || status.is_some() || validate_trailers(headers).is_err()))
            invalid = true;
        if (invalid) {
            reset_stream(*s, 1);
            return Ok(empty {});
        }
        if (s->final) {
            events_.push(Event::Trailers(u32(id), rstd::move(headers)));
            return end(*s);
        }
        auto code = status->value().to_primitive();
        bool info = code < 200;
        if (code < 100 || code == 101 || ((info || code == 204) && length.is_some()) ||
            (info && (block_end_ || ++s->informational > limits_.maximum_informational))) {
            reset_stream(*s, 1);
            return Ok(empty {});
        }
        if (! info) {
            s->final    = true;
            s->expected = length;
            s->head     = s->head || code == 204 || code == 304;
        }
        events_.push(Event::Head(
            u32(id),
            ResponseHead {
                *status, rstd::move(headers), Some(MessageVersion(u8(2), u8())), None() },
            info));
        if (block_end_) return end(*s);
        return Ok(empty {});
    }
    auto receive(Frame f) -> Result<empty> {
        unsigned type = f.type.to_primitive(), flags = f.flags.to_primitive(),
                 id    = f.stream.to_primitive();
        auto     bytes = f.payload.as_slice();
        auto     n     = bytes.len().to_primitive();
        if (continuation_ && (type != 9 || id != continuation_))
            return Err(Error::Connection(u32(1)));
        if (! settings_ && (type != 4 || id != 0 || (flags & 1)))
            return Err(Error::Connection(u32(1)));
        if (type == 4) {
            if (id || ((flags & 1) && n) || (! (flags & 1) && n % 6))
                return Err(Error::Connection(u32(id ? 1 : 6)));
            if (flags & 1) {
                settings_ack_ = true;
                return Ok(empty {});
            }
            settings_ = true;
            for (usize at; at < bytes.len(); at += usize(6)) {
                unsigned key =
                    (unsigned(bytes[at].to_primitive()) << 8) | bytes[at + usize(1)].to_primitive();
                unsigned value = word(bytes, at + usize(2));
                if (key == 2 && value != 0) return Err(Error::Connection(u32(1)));
                if (key == 3) peer_maximum_ = value;
                if (key == 4) {
                    if (value > 0x7fffffff) return Err(Error::Connection(u32(3)));
                    auto delta = static_cast<long long>(value) - peer_window_;
                    for (auto& s : streams_) {
                        s.send_window += delta;
                        if (s.send_window > 0x7fffffff) return Err(Error::Connection(u32(3)));
                    }
                    peer_window_ = value;
                }
                if (key == 5) {
                    if (value < 16384 || value > 16777215) return Err(Error::Connection(u32(1)));
                    peer_frame_ = value;
                }
            }
            emit(4, 1, 0);
            return Ok(empty {});
        }
        if (type == 6) {
            if (id || n != 8) return Err(Error::Connection(u32(id ? 1 : 6)));
            if (! (flags & 1)) emit(6, 1, 0, rstd::move(f.payload));
            return Ok(empty {});
        }
        if (type == 7) {
            if (id || n < 8) return Err(Error::Connection(u32(id ? 1 : 6)));
            auto last = word(bytes) & 0x7fffffff, code = word(bytes, usize(4));
            if (last > goaway_last_) return Err(Error::Connection(u32(1)));
            goaway_last_ = last;
            draining_    = true;
            events_.push(Event::Goaway(u32(last), u32(code)));
            for (auto& s : streams_)
                if (s.id > last && ! s.reset) {
                    release_received(s);
                    s.reset     = true;
                    s.local_end = s.remote_end = true;
                    events_.push(Event::Reset(u32(s.id), u32(7)));
                }
            return Ok(empty {});
        }
        if (type == 8) {
            if (n != 4) return Err(Error::Connection(u32(6)));
            unsigned credit = word(bytes) & 0x7fffffff;
            if (! id) {
                if (! credit) return Err(Error::Connection(u32(1)));
                connection_send_ += credit;
                if (connection_send_ > 0x7fffffff) return Err(Error::Connection(u32(3)));
            } else {
                if (! (id & 1)) return Err(Error::Connection(u32(1)));
                auto* s = find(id);
                if (! s) {
                    if (id >= next_) return Err(Error::Connection(u32(1)));
                    return Ok(empty {});
                }
                if (! credit)
                    reset_stream(*s, 1);
                else if (! s->reset && ! s->local_end) {
                    s->send_window += credit;
                    if (s->send_window > 0x7fffffff) reset_stream(*s, 3);
                }
            }
            return Ok(empty {});
        }
        if (type == 5) return Err(Error::Connection(u32(1)));
        if (type == 2) {
            if (! id || n != 5) return Err(Error::Connection(u32(! id ? 1 : 6)));
            if ((word(bytes) & 0x7fffffff) == id) {
                auto* s = find(id);
                if (s) reset_stream(*s, 1);
            }
            return Ok(empty {});
        }
        if (type != 0 && type != 1 && type != 3 && type != 9) return Ok(empty {});
        if (! id || ! (id & 1) || id >= next_) return Err(Error::Connection(u32(1)));
        auto* s = find(id);
        if (type == 3) {
            if (n != 4) return Err(Error::Connection(u32(6)));
            if (s && ! s->reset) {
                release_received(*s);
                s->reset     = true;
                s->local_end = s->remote_end = true;
                events_.push(Event::Reset(u32(id), u32(word(bytes))));
            }
            return Ok(empty {});
        }
        if (type == 9) {
            if (! continuation_) return Err(Error::Connection(u32(1)));
            if (f.payload.len() > limits_.header_bytes - block_.len())
                return Err(Error::TooLarge());
            auto copy = Vec<u8>::from(block_.as_slice());
            copy.extend_from_slice(bytes);
            block_ = Bytes::from_vec(rstd::move(copy));
            if (flags & 4) return headers(id);
            return Ok(empty {});
        }
        unsigned start = 0, padding = 0;
        if (flags & 8) {
            if (! n) return Err(Error::Connection(u32(1)));
            padding = bytes[usize()].to_primitive() + 1;
            start   = 1;
            if (padding > n) return Err(Error::Connection(u32(1)));
        }
        unsigned end_at = n - (padding ? padding - 1 : 0);
        if (type == 1) {
            if (flags & 32) {
                if (end_at - start < 5) return Err(Error::Connection(u32(6)));
                if ((word(bytes, usize(start)) & 0x7fffffff) == id) {
                    if (s) reset_stream(*s, 1);
                }
                start += 5;
            }
            if (end_at < start) return Err(Error::Connection(u32(1)));
            block_        = f.payload.slice(usize(start), usize(end_at));
            block_end_    = (flags & 1) != 0;
            continuation_ = id;
            if (block_.len() > limits_.header_bytes) return Err(Error::TooLarge());
            if (flags & 4) return headers(id);
            return Ok(empty {});
        }
        if (n > connection_receive_) return Err(Error::Connection(u32(3)));
        connection_receive_ -= n;
        connection_outstanding_ += n;
        if (! s || s->reset || s->remote_end) {
            connection_receive_ += n;
            connection_outstanding_ -= n;
            update(0, n);
            if (s && ! s->reset) reset_stream(*s, 5);
            return Ok(empty {});
        }
        if (! s->final || n > s->receive_window) {
            reset_stream(*s, ! s->final ? 1 : 3);
            connection_receive_ += n;
            connection_outstanding_ -= n;
            update(0, n);
            return Ok(empty {});
        }
        s->receive_window -= n;
        s->outstanding += n;
        if (padding) consume(u32(id), usize(padding));
        auto data = f.payload.slice(usize(start), usize(end_at));
        if (s->head && ! data.is_empty()) {
            reset_stream(*s, 1);
            return Ok(empty {});
        }
        s->received += u64(data.len().to_primitive());
        if (s->expected.is_some() && ! s->head && s->received > *s->expected) {
            reset_stream(*s, 1);
            return Ok(empty {});
        }
        if (! data.is_empty()) events_.push(Event::Data(u32(id), rstd::move(data)));
        if (flags & 1) return end(*s);
        return Ok(empty {});
    }
    explicit ClientConnection(Limits limits): limits_(limits) {
        output_.push({ Bytes::copy_from_slice("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"_bytes), u32() });
        Vec<u8> p;
        auto    setting = [&](unsigned key, unsigned value) {
            p.push(u8(key >> 8));
            p.push(u8(key & 255));
            word(p, value);
        };
        setting(2, 0);
        setting(4, static_cast<unsigned>(limits.receive_window.to_primitive()));
        setting(6, static_cast<unsigned>(limits.header_bytes.to_primitive()));
        emit(4, 0, 0, Bytes::from_vec(rstd::move(p)));
        auto capacity =
            u64(limits.receive_window.to_primitive()) * u64(limits.maximum_streams.to_primitive());
        auto receive = capacity > u64(0x7fffffff) ? 0x7fffffffu
                                                  : static_cast<unsigned>(capacity.to_primitive());
        if (receive > 65535) {
            connection_receive_ = receive;
            update(0, receive - 65535);
        }
    }

public:
    ClientConnection(): ClientConnection(Limits {}) {}
    static auto make(Limits limits) -> Result<ClientConnection> {
        if (limits.receive_window > usize(0x7fffffff) || limits.header_bytes > usize(0xffffffff) ||
            limits.maximum_streams > usize(0x7fffffff))
            return Err(Error::TooLarge());
        return Ok(ClientConnection(limits));
    }
    auto established() const -> bool { return settings_ && settings_ack_; }
    auto accepting() const -> bool { return ! draining_ && next_ <= 0x7fffffff; }
    auto capacity() const -> usize {
        return rstd::cmp::min(limits_.maximum_streams, usize(peer_maximum_));
    }
    auto open(const Request& request) -> Result<u32> {
        usize active;
        for (const auto& s : streams_)
            if (! s.reset && ! (s.local_end && s.remote_end)) ++active;
        if (! accepting() || active >= capacity()) return Err(Error::Stream(u32(), u32(7)));
        auto     block = rstd_try(encode_fields(request.fields.as_slice(), limits_.header_bytes));
        unsigned id    = next_;
        next_ += 2;
        streams_.push(Stream { id,
                               peer_window_,
                               static_cast<unsigned>(limits_.receive_window.to_primitive()),
                               0,
                               None(),
                               u64(),
                               request.head });
        send_headers(id, rstd::move(block), false);
        return Ok(u32(id));
    }

private:
    void send_headers(unsigned id, Bytes block, bool end_stream) {
        bool first = true;
        do {
            auto n    = rstd::cmp::min(usize(peer_frame_), block.len());
            auto part = block.slice(usize(), n);
            block.advance(n);
            emit(first ? 1 : 9,
                 (block.is_empty() ? 4 : 0) | (first && end_stream ? 1 : 0),
                 id,
                 rstd::move(part));
            first = false;
        } while (! block.is_empty());
    }

public:
    auto writable(u32 id) -> usize {
        auto* s = find(id.to_primitive());
        if (local_shutdown_ || ! s || s->reset || s->local_end) return usize();
        auto n = s->send_window < connection_send_ ? s->send_window : connection_send_;
        return n > 0 ? usize(static_cast<unsigned long long>(n < 16384 ? n : 16384)) : usize();
    }
    auto send(u32 id, Bytes bytes, bool end_stream = false) -> Result<empty> {
        auto* s = find(id.to_primitive());
        if (local_shutdown_ || ! s || s->reset || s->local_end || bytes.len() > writable(id))
            return Err(Error::Stream(u32(id), u32(5)));
        auto n = bytes.len().to_primitive();
        s->send_window -= n;
        connection_send_ -= n;
        emit(0, end_stream ? 1 : 0, id.to_primitive(), rstd::move(bytes));
        s->local_end = end_stream;
        return Ok(empty {});
    }
    auto trailers(u32 id, const Headers& headers) -> Result<empty> {
        auto* s = find(id.to_primitive());
        if (local_shutdown_ || ! s || s->reset || s->local_end)
            return Err(Error::Stream(u32(id), u32(5)));
        if (validate_trailers(headers).is_err()) return Err(Error::Stream(u32(id), u32(1)));
        Vec<Field> fields;
        for (const auto& f : headers) {
            auto value = f.value.as_slice();
            if (! value.is_empty() && (ows(value[usize()]) || ows(value[value.len() - usize(1)])))
                return Err(Error::Stream(u32(id), u32(1)));
            auto name = String::make(f.name.as_str());
            name.as_mut_str().make_ascii_lowercase();
            fields.push({ rstd::move(name), Bytes::copy_from_slice(f.value.as_slice()), true });
        }
        auto block = rstd_try(encode_fields(fields.as_slice(), limits_.header_bytes));
        send_headers(id.to_primitive(), rstd::move(block), true);
        s->local_end = true;
        return Ok(empty {});
    }
    void consume(u32 id, usize count) {
        auto* s = find(id.to_primitive());
        if (! s || s->reset) return;
        unsigned n = static_cast<unsigned>(count.to_primitive());
        if (n > s->outstanding) n = s->outstanding;
        s->outstanding -= n;
        connection_outstanding_ -= n;
        connection_receive_ += n;
        update(0, n);
        s->receive_window += n;
        if (! s->remote_end) update(s->id, n);
    }
    void reset(u32 id, u32 code = u32(8)) {
        auto* s = find(id.to_primitive());
        if (s) reset_stream(*s, code.to_primitive());
    }
    void forget(u32 id) {
        for (usize i; i < streams_.len(); ++i)
            if (streams_[i].id == id.to_primitive()) {
                consume(id, usize(streams_[i].outstanding));
                streams_.remove(i);
                break;
            }
    }
    auto feed(Bytes bytes) -> Result<empty> {
        auto frames = rstd_try(frames_.feed(rstd::move(bytes)));
        for (auto& f : frames) rstd_try(receive(rstd::move(f)));
        return Ok(empty {});
    }
    auto take_events() -> Vec<Event> { return rstd::exchange(events_, Vec<Event>()); }
    auto take_output() -> Vec<Outbound> { return rstd::exchange(output_, Vec<Outbound>()); }
    void shutdown(u32 code = u32()) {
        if (local_shutdown_) return;
        draining_ = true;
        Vec<u8> p;
        word(p, 0);
        word(p, code.to_primitive());
        emit(7, 0, 0, Bytes::from_vec(rstd::move(p)));
        local_shutdown_ = true;
    }
};
} // namespace lihttpto::h2
