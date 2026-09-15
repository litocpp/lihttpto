#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

namespace
{
struct Echo {
    auto operator()(Connection& connection, const RequestHead&, const Shutdown&) const
        -> rstd::async::coro<Result<empty, ConnectionError>> {
        Response response;
        response.body = rstd::bytes::Bytes::copy_from_slice("ok"_str.as_bytes());
        co_return co_await connection.respond(response, true);
    }
};
struct Gated {
    rstd::async::Notify                  entered;
    rstd::async::Notify                  release;
    rstd::sync::atomic::Atomic<unsigned> dropped { 0 };
    Gated(rstd::async::Notify first, rstd::async::Notify second)
        : entered(rstd::move(first)), release(rstd::move(second)) {}
    struct Guard {
        Gated& owner;
        ~Guard() { (void)owner.dropped.fetch_add(1); }
    };
    auto operator()(Connection& connection, const RequestHead&, const Shutdown&)
        -> rstd::async::coro<Result<empty, ConnectionError>> {
        Guard guard { *this };
        (void)entered.notifier().notify();
        auto ready = co_await release.notified();
        if (ready.is_err())
            co_return Err(ConnectionError {
                ConnectionErrorKind::Io, None(), None(), Some(rstd::move(ready).unwrap_err()) });
        Response response;
        response.body = rstd::bytes::Bytes::copy_from_slice("ok"_str.as_bytes());
        co_return co_await connection.respond(response, true);
    }
};
auto request(rstd::net::TcpStream& stream) -> rstd::async::coro<bool> {
    auto wire = rstd::bytes::Bytes::copy_from_slice(
        "GET /api/health HTTP/1.1\r\nHost: notebook.test\r\n\r\n"_str.as_bytes());
    co_return (co_await rstd::async::io::write_all(stream, wire)).is_ok();
}
auto response(rstd::net::TcpStream& stream) -> rstd::async::coro<bool> {
    auto expected = "HTTP/1.1 200 \r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"_str;
    auto bytes    = rstd::bytes::BytesMut::with_capacity(expected.len());
    auto read     = co_await rstd::async::io::read_exact(stream, bytes, expected.len());
    co_return read.is_ok() && rstd::str_::from_utf8(bytes.as_slice()).unwrap() == expected;
}
auto roundtrip(rstd::net::TcpListener listener, rstd::net::SocketAddr address)
    -> rstd::async::coro<bool> {
    auto stop      = Shutdown::make().unwrap();
    auto task      = rstd::async::AbortOnDropHandle { rstd::async::spawn(
        serve(rstd::move(listener), rstd::sync::Arc<Echo>::make(), stop.clone())) };
    auto connected = co_await rstd::net::TcpStream::connect(address);
    if (connected.is_err()) co_return false;
    auto stream = rstd::move(connected).unwrap();
    if (! (co_await request(stream)) || ! (co_await response(stream))) co_return false;
    if (stop.request_stop().is_err()) co_return false;
    auto report = co_await rstd::move(task);
    if (report.is_err() || report->is_err()) co_return false;
    auto refused = co_await rstd::net::TcpStream::connect(address);
    co_return refused.is_err() && report->unwrap().accepted == usize(1);
}
auto bounded(rstd::net::TcpListener listener, rstd::net::SocketAddr address)
    -> rstd::async::coro<bool> {
    auto         stop = Shutdown::make().unwrap();
    auto         gate = rstd::sync::Arc<Gated>::make(rstd::async::Notify::make().unwrap(),
                                                     rstd::async::Notify::make().unwrap());
    ServerLimits limits;
    limits.connections = usize(1);
    auto task          = rstd::async::AbortOnDropHandle { rstd::async::spawn(
        serve(rstd::move(listener), gate.clone(), stop.clone(), limits)) };
    auto a             = (co_await rstd::net::TcpStream::connect(address)).unwrap();
    if (! (co_await request(a))) co_return false;
    if ((co_await gate->entered.notified()).is_err()) co_return false;
    auto b = (co_await rstd::net::TcpStream::connect(address)).unwrap();
    if (! (co_await request(b))) co_return false;
    auto early = co_await rstd::async::timeout(gate->entered.notified(),
                                               rstd::time::Duration::from_millis(u64(5)));
    if (early.is_ok()) co_return false;
    (void)gate->release.notifier().notify();
    if (! (co_await response(a))) co_return false;
    if ((co_await gate->entered.notified()).is_err()) co_return false;
    (void)stop.request_stop();
    (void)gate->release.notifier().notify();
    if (! (co_await response(b))) co_return false;
    auto report = co_await rstd::move(task);
    if (report.is_err() || report->is_err()) co_return false;
    auto done = rstd::move(*report).unwrap();
    co_return done.accepted == usize(2) && done.cancelled == usize() && done.failed == usize() &&
        gate->dropped.load() == 2;
}
auto forced(rstd::net::TcpListener listener, rstd::net::SocketAddr address)
    -> rstd::async::coro<bool> {
    auto         stop = Shutdown::make().unwrap();
    auto         gate = rstd::sync::Arc<Gated>::make(rstd::async::Notify::make().unwrap(),
                                                     rstd::async::Notify::make().unwrap());
    ServerLimits limits;
    limits.shutdown_timeout = rstd::time::Duration::from_millis(u64(2));
    auto task               = rstd::async::AbortOnDropHandle { rstd::async::spawn(
        serve(rstd::move(listener), gate.clone(), stop.clone(), limits)) };
    auto stream             = (co_await rstd::net::TcpStream::connect(address)).unwrap();
    if (! (co_await request(stream))) co_return false;
    if ((co_await gate->entered.notified()).is_err()) co_return false;
    (void)stop.request_stop();
    auto report = co_await rstd::move(task);
    if (report.is_err() || report->is_err()) co_return false;
    co_return report->unwrap().cancelled == usize(1) && gate->dropped.load() == 1;
}
auto abort_server(rstd::net::TcpListener listener, rstd::net::SocketAddr address)
    -> rstd::async::coro<bool> {
    auto stop   = Shutdown::make().unwrap();
    auto gate   = rstd::sync::Arc<Gated>::make(rstd::async::Notify::make().unwrap(),
                                               rstd::async::Notify::make().unwrap());
    auto task   = rstd::async::AbortOnDropHandle { rstd::async::spawn(
        serve(rstd::move(listener), gate.clone(), stop.clone())) };
    auto stream = (co_await rstd::net::TcpStream::connect(address)).unwrap();
    if (! (co_await request(stream))) co_return false;
    if ((co_await gate->entered.notified()).is_err()) co_return false;
    task.abort();
    auto result = co_await rstd::move(task);
    co_return result.is_err() && stop.requested() && gate->dropped.load() == 1;
}
struct MissingResponse {
    auto operator()(Connection&, const RequestHead&, const Shutdown&) const
        -> rstd::async::coro<Result<empty, ConnectionError>> {
        co_return Ok(empty {});
    }
};
auto missing_response(rstd::net::TcpListener listener, rstd::net::SocketAddr address)
    -> rstd::async::coro<bool> {
    auto stop   = Shutdown::make().unwrap();
    auto task   = rstd::async::AbortOnDropHandle { rstd::async::spawn(
        serve(rstd::move(listener), rstd::sync::Arc<MissingResponse>::make(), stop.clone())) };
    auto stream = (co_await rstd::net::TcpStream::connect(address)).unwrap();
    if (! (co_await request(stream))) co_return false;
    auto buffer = rstd::bytes::BytesMut::with_capacity(usize(1));
    auto eof    = co_await rstd::async::io::read(stream, buffer);
    if (eof.is_err() || *eof != usize()) co_return false;
    (void)stop.request_stop();
    auto report = co_await rstd::move(task);
    co_return report.is_ok() && report->is_ok() && report->unwrap().failed == usize(1);
}
template<typename F>
void check(F scenario, bool multiple = false) {
    auto listener =
        rstd::net::TcpListener::bind(rstd::net::SocketAddr::ipv4_loopback(u16())).unwrap();
    auto address = listener.local_addr().unwrap();
    auto builder = multiple ? rstd::async::RuntimeBuilder::multi_thread()
                            : rstd::async::RuntimeBuilder::current_thread();
    if (multiple) builder.worker_threads(usize(2));
    auto runtime = builder.enable_all().build().unwrap();
    auto task =
        rstd::async::AbortOnDropHandle { runtime.spawn(scenario(rstd::move(listener), address)) };
    auto result = runtime.block_on(
        rstd::async::timeout(rstd::move(task), rstd::time::Duration::from_secs(u64(5))));
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(result->is_ok());
    EXPECT_TRUE(**result);
}
} // namespace
TEST(Server, DispatchesAndStopsListening) {
    check(roundtrip);
}
TEST(Server, BoundsConnectionsAndCompletesActiveResponseDuringShutdown) {
    check(bounded);
}
TEST(Server, CancelsAfterGraceAndWaitsForHandlerCleanup) {
    check(forced);
}
TEST(Server, AbortingServeCancelsOwnedHandlers) {
    check(abort_server);
}
TEST(Server, MissingResponseClosesConnectionAndReportsFailure) {
    check(missing_response);
}
TEST(Server, MultiThreadDispatchAndShutdown) {
    check(roundtrip, true);
}
TEST(Server, MultiThreadGraceCancellation) {
    check(forced, true);
}
