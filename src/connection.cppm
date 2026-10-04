module;
#include <rstd/macro.hpp>
export module lihttpto:connection;
export import :request_body;
export import :response;

import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;
using IoError = rstd::io::error::Error;

export namespace lihttpto
{
enum class ConnectionErrorKind
{
    State,
    Decode,
    Io,
    Timeout,
    Expectation,
    Response
};
struct ConnectionError {
    ConnectionErrorKind   kind;
    Option<DecodeError>   decode;
    Option<ResponseError> response;
    Option<IoError>       cause;
};
struct ConnectionLimits {
    HeadLimits           head;
    BodyLimits           body;
    rstd::time::Duration head_timeout { rstd::time::Duration::from_secs(u64(15)) };
    rstd::time::Duration body_timeout { rstd::time::Duration::from_secs(u64(60)) };
    rstd::time::Duration write_timeout { rstd::time::Duration::from_secs(u64(30)) };
    rstd::time::Duration close_timeout { rstd::time::Duration::from_millis(u64(100)) };
    usize                close_bytes { 65536 };
    usize                requests { 100 };
    // When set, each streamed body write has its own budget instead of the response-wide deadline.
    Option<rstd::time::Duration> stream_write_timeout;
};
} // namespace lihttpto

namespace lihttpto
{
auto connection_error(ConnectionErrorKind kind) -> ConnectionError {
    return ConnectionError { kind, None(), None(), None() };
}
auto decode_error(DecodeError error) -> ConnectionError {
    return ConnectionError { ConnectionErrorKind::Decode, Some(error), None(), None() };
}
auto response_error(ResponseWriteError error) -> ConnectionError {
    return ConnectionError { error.kind == ResponseError::Timeout ? ConnectionErrorKind::Timeout
                                                                  : ConnectionErrorKind::Response,
                             None(),
                             Some(error.kind),
                             rstd::move(error.cause) };
}
} // namespace lihttpto

export namespace lihttpto
{
class Connection {
    enum class State
    {
        Idle,
        Reading,
        Body,
        ReadingBody,
        Ready,
        Writing,
        Streaming,
        Closed
    };
    State                        state_ { State::Idle };
    Option<rstd::net::TcpStream> stream_;
    ConnectionLimits             limits_;
    rstd::bytes::BytesMut        buffer_ { rstd::bytes::BytesMut::with_capacity(usize(8192)) };
    Option<alloc::boxed::Box<RequestBodyDecoder>> body_;
    String                                        method_;
    Version                                       version_ { Version::Http11 };
    bool                                          keep_alive_ {};
    bool                                          continue_pending_ {};
    bool                                          stream_body_allowed_ {};
    rstd::time::Instant                           stream_started_ { rstd::time::Instant::now() };
    usize                                         requests_ {};
    rstd::time::Instant                           body_started_ { rstd::time::Instant::now() };

    struct Operation {
        Connection& owner;
        bool        complete { false };
        ~Operation() {
            if (! complete) owner.close();
        }
    };

    auto refill(rstd::time::Instant started, rstd::time::Duration budget)
        -> rstd::async::coro<Result<bool, ConnectionError>> {
        auto elapsed = started.elapsed();
        if (elapsed >= budget) co_return Err(connection_error(ConnectionErrorKind::Timeout));
        buffer_.clear();
        auto timed = co_await rstd::async::timeout(rstd::async::io::read(*stream_, buffer_),
                                                   budget - elapsed);
        if (timed.is_err()) co_return Err(connection_error(ConnectionErrorKind::Timeout));
        auto read = rstd::move(timed).unwrap();
        if (read.is_err())
            co_return Err(ConnectionError {
                ConnectionErrorKind::Io, None(), None(), Some(rstd::move(read).unwrap_err()) });
        co_return Ok(*read != usize());
    }

    auto finish_rejection() -> rstd::async::coro<empty> {
        if (stream_->shutdown().is_ok()) {
            auto started   = rstd::time::Instant::now();
            auto discarded = buffer_.len();
            while (discarded < limits_.close_bytes) {
                auto read = co_await refill(started, limits_.close_timeout);
                if (read.is_err() || ! *read) break;
                if (buffer_.len() >= limits_.close_bytes - discarded) break;
                discarded += buffer_.len();
            }
        }
        close();
        co_return empty {};
    }

public:
    explicit Connection(rstd::net::TcpStream stream, ConnectionLimits limits = {})
        : stream_(Some(rstd::move(stream))), limits_(limits) {}
    Connection(const Connection&)                    = delete;
    Connection(Connection&&)                         = delete;
    auto operator=(const Connection&) -> Connection& = delete;
    auto operator=(Connection&&) -> Connection&      = delete;

    void close() {
        state_  = State::Closed;
        stream_ = None();
    }
    auto is_closed() const -> bool { return state_ == State::Closed; }
    auto ready_for_request() const -> bool { return state_ == State::Idle; }

    // Operations are sequential; cancelling an active operation closes this
    // connection.
    auto read_request() -> rstd::async::coro<Result<Option<RequestHead>, ConnectionError>> {
        if (state_ != State::Idle) co_return Err(connection_error(ConnectionErrorKind::State));
        state_ = State::Reading;
        Operation operation { *this };
        if (requests_ >= limits_.requests) {
            close();
            co_return Ok<Option<RequestHead>>(None());
        }
        RequestHeadDecoder decoder(limits_.head);
        auto               started  = rstd::time::Instant::now();
        bool               received = false;
        for (;;) {
            if (started.elapsed() >= limits_.head_timeout)
                co_return Err(connection_error(ConnectionErrorKind::Timeout));
            if (buffer_.is_empty()) {
                auto read = co_await refill(started, limits_.head_timeout);
                if (read.is_err()) co_return Err(rstd::move(read).unwrap_err());
                if (! *read) {
                    if (received) co_return Err(decode_error(DecodeError::Truncated));
                    close();
                    co_return Ok<Option<RequestHead>>(None());
                }
            }
            received      = true;
            auto progress = decoder.feed(buffer_.as_slice());
            if (progress.is_err()) co_return Err(decode_error(progress.unwrap_err()));
            buffer_.advance(progress->consumed);
            if (progress->status != DecodeStatus::Complete) continue;
            auto head   = decoder.take().unwrap();
            bool expect = false;
            for (const auto& header : head.headers) {
                if (! ascii_equal(header.name.as_str().as_bytes(), "expect"_str)) continue;
                if (expect || head.line.version != Version::Http11 ||
                    ! ascii_equal(header.value.as_slice(), "100-continue"_str)) {
                    Response rejection;
                    rejection.status = u16(417);
                    auto written     = co_await write_response(*stream_,
                                                               rejection,
                                                               head.line.method.as_str(),
                                                               head.line.version,
                                                               false,
                                                               limits_.write_timeout);
                    if (written.is_err())
                        co_return Err(response_error(rstd::move(written).unwrap_err()));
                    co_await finish_rejection();
                    co_return Err(connection_error(ConnectionErrorKind::Expectation));
                }
                expect = true;
            }
            body_ = Some(alloc::boxed::Box<RequestBodyDecoder>::make(head.body, limits_.body));
            auto initial = (*body_)->feed({});
            if (initial.is_err()) co_return Err(decode_error(initial.unwrap_err()));
            method_  = head.line.method.clone();
            version_ = head.line.version;
            ++requests_;
            keep_alive_        = head.keep_alive && requests_ < limits_.requests;
            body_started_      = rstd::time::Instant::now();
            bool complete      = initial->status == DecodeStatus::Complete;
            continue_pending_  = expect && ! complete;
            state_             = complete ? State::Ready : State::Body;
            operation.complete = true;
            co_return Ok(Some(rstd::move(head)));
        }
    }

    auto accept_body() -> rstd::async::coro<Result<empty, ConnectionError>> {
        if (state_ != State::Body && state_ != State::Ready)
            co_return Err(connection_error(ConnectionErrorKind::State));
        if (! continue_pending_) co_return Ok(empty {});
        state_ = State::ReadingBody;
        Operation operation { *this };
        if (body_started_.elapsed() >= limits_.body_timeout)
            co_return Err(connection_error(ConnectionErrorKind::Timeout));
        auto interim =
            rstd::bytes::Bytes::copy_from_slice("HTTP/1.1 100 Continue\r\n\r\n"_str.as_bytes());
        rstd_co_try(co_await write_with_deadline(
                        *stream_, interim, rstd::time::Instant::now(), limits_.write_timeout),
                    response_error);
        continue_pending_  = false;
        state_             = State::Body;
        operation.complete = true;
        co_return Ok(empty {});
    }

    auto read_body() -> rstd::async::coro<Result<Option<rstd::bytes::Bytes>, ConnectionError>> {
        if (state_ == State::Ready) co_return Ok<Option<rstd::bytes::Bytes>>(None());
        if (state_ != State::Body) co_return Err(connection_error(ConnectionErrorKind::State));
        rstd_co_try(co_await accept_body());
        state_ = State::ReadingBody;
        Operation operation { *this };
        for (;;) {
            if (body_started_.elapsed() >= limits_.body_timeout)
                co_return Err(connection_error(ConnectionErrorKind::Timeout));
            if (buffer_.is_empty()) {
                auto read = co_await refill(body_started_, limits_.body_timeout);
                if (read.is_err()) co_return Err(rstd::move(read).unwrap_err());
                if (! *read) co_return Err(decode_error(DecodeError::Truncated));
            }
            auto progress = (*body_)->feed(buffer_.as_slice());
            if (progress.is_err()) co_return Err(decode_error(progress.unwrap_err()));
            auto output = rstd::bytes::Bytes::copy_from_slice(progress->data);
            buffer_.advance(progress->consumed);
            bool complete = progress->status == DecodeStatus::Complete;
            if (complete || ! output.is_empty()) {
                state_             = complete ? State::Ready : State::Body;
                operation.complete = true;
                if (output.is_empty()) co_return Ok<Option<rstd::bytes::Bytes>>(None());
                co_return Ok(Some(rstd::move(output)));
            }
        }
    }

    auto take_trailers() -> Option<Headers> {
        if (state_ != State::Ready) return None();
        return (*body_)->take_trailers();
    }

    auto respond(const Response& response, bool close_after = false)
        -> rstd::async::coro<Result<empty, ConnectionError>> {
        if (state_ != State::Ready && ! (state_ == State::Body && close_after))
            co_return Err(connection_error(ConnectionErrorKind::State));
        bool unread_body  = state_ == State::Body;
        state_            = State::Writing;
        continue_pending_ = false;
        Operation operation { *this };
        bool      reuse   = keep_alive_ && ! close_after;
        auto      written = co_await write_response(
            *stream_, response, method_.as_str(), version_, reuse, limits_.write_timeout);
        if (written.is_err()) co_return Err(response_error(rstd::move(written).unwrap_err()));
        body_ = None();
        if (reuse)
            state_ = State::Idle;
        else if (unread_body)
            co_await finish_rejection();
        else
            close();
        operation.complete = true;
        co_return Ok(empty {});
    }

    // Streamed responses are close-delimited and cannot reuse the connection.
    auto begin_response(const StreamResponseHead& response)
        -> rstd::async::coro<Result<empty, ConnectionError>> {
        if (state_ != State::Ready) co_return Err(connection_error(ConnectionErrorKind::State));
        auto head = encode_response_head(response, method_.as_str(), version_);
        if (head.is_err())
            co_return Err(response_error(ResponseWriteError { head.unwrap_err(), None() }));
        state_ = State::Writing;
        Operation operation { *this };
        stream_started_ = rstd::time::Instant::now();
        rstd_co_try(
            co_await write_with_deadline(*stream_, *head, stream_started_, limits_.write_timeout),
            response_error);
        stream_body_allowed_ = ! omit_response_body(response.status, method_.as_str());
        body_                = None();
        state_               = State::Streaming;
        operation.complete   = true;
        co_return Ok(empty {});
    }

    auto write_body(const rstd::bytes::Bytes& bytes)
        -> rstd::async::coro<Result<empty, ConnectionError>> {
        if (state_ != State::Streaming) co_return Err(connection_error(ConnectionErrorKind::State));
        state_ = State::Writing;
        Operation operation { *this };
        if (! stream_body_allowed_ && ! bytes.is_empty())
            co_return Err(
                response_error(ResponseWriteError { ResponseError::InvalidBody, None() }));
        if (! bytes.is_empty()) {
            auto started = limits_.stream_write_timeout.is_some() ? rstd::time::Instant::now()
                                                                  : stream_started_;
            auto budget  = limits_.stream_write_timeout.is_some() ? *limits_.stream_write_timeout
                                                                  : limits_.write_timeout;
            rstd_co_try(co_await write_with_deadline(*stream_, bytes, started, budget),
                        response_error);
        }
        state_             = State::Streaming;
        operation.complete = true;
        co_return Ok(empty {});
    }

    auto finish_response() -> Result<empty, ConnectionError> {
        if (state_ != State::Streaming) return Err(connection_error(ConnectionErrorKind::State));
        close();
        return Ok(empty {});
    }

    auto reject(const Response& response) -> rstd::async::coro<Result<empty, ConnectionError>> {
        co_return co_await respond(response, true);
    }
};
} // namespace lihttpto
