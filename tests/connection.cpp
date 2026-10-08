#include <rstd/test/gtest.hpp>
import lihttpto;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;

namespace
{
auto send(rstd::net::TcpStream& stream, ref<str> text) -> rstd::async::coro<bool> {
    auto bytes = rstd::bytes::Bytes::copy_from_slice(text.as_bytes());
    co_return (co_await rstd::async::io::write_all(stream, bytes)).is_ok();
}
auto receive(rstd::net::TcpStream& stream, ref<str> expected) -> rstd::async::coro<bool> {
    auto buffer = rstd::bytes::BytesMut::with_capacity(expected.len());
    auto read   = co_await rstd::async::io::read_exact(stream, buffer, expected.len());
    if (read.is_err()) co_return false;
    auto text = rstd::str_::from_utf8(buffer.as_slice());
    co_return text.is_ok() && *text == expected;
}
auto pipeline(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(
            client,
            "POST /one HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\noneGET /two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    auto first = co_await connection.read_request();
    if (first.is_err() || first->is_none() || first->unwrap().line.target.as_str() != "/one"_str)
        co_return false;
    Response response;
    if ((co_await connection.respond(response)).is_ok()) co_return false;
    if ((co_await connection.read_request()).is_ok()) co_return false;
    Vec<u8> content;
    for (;;) {
        auto body = co_await connection.read_body();
        if (body.is_err()) co_return false;
        if (body->is_none()) break;
        auto bytes = rstd::move(*body).unwrap();
        for (auto byte : bytes.as_slice()) content.push(u8(byte));
    }
    if (rstd::str_::from_utf8(content.as_slice()).unwrap() != "one"_str) co_return false;
    response.body = rstd::bytes::Bytes::copy_from_slice(content.as_slice());
    if ((co_await connection.respond(response)).is_err()) co_return false;
    if (! (co_await receive(
            client, "HTTP/1.1 200 \r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\none"_str)))
        co_return false;
    auto second = co_await connection.read_request();
    if (second.is_err() || second->is_none() || second->unwrap().line.target.as_str() != "/two"_str)
        co_return false;
    Response empty_response;
    if ((co_await connection.respond(empty_response)).is_err()) co_return false;
    if (! (co_await receive(client,
                            "HTTP/1.1 200 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    co_return connection.is_closed();
}
auto expectation(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(
            client,
            "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n"_str)))
        co_return false;
    auto request = co_await connection.read_request();
    if (request.is_err() || request->is_none()) co_return false;
    if ((co_await connection.accept_body()).is_err() ||
        (co_await connection.accept_body()).is_err())
        co_return false;
    if (! (co_await receive(client, "HTTP/1.1 100 Continue\r\n\r\n"_str))) co_return false;
    if (! (co_await send(client, "1\r\nx\r\n0\r\nX-End: yes\r\n\r\n"_str))) co_return false;
    usize total {};
    for (;;) {
        auto part = co_await connection.read_body();
        if (part.is_err()) co_return false;
        if (part->is_none()) break;
        total += part->unwrap().len();
    }
    auto trailers = connection.take_trailers();
    if (trailers.is_none() || trailers->len() != usize(1) || total != usize(1)) co_return false;
    Response response;
    auto     sent = co_await connection.respond(response, true);
    co_return sent.is_ok() && connection.is_closed() &&
        (co_await receive(client,
                          "HTTP/1.1 200 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str));
}
auto timeout(rstd::net::TcpStream server, rstd::net::TcpStream&) -> rstd::async::coro<bool> {
    ConnectionLimits limits;
    limits.head_timeout = rstd::time::Duration::from_millis(u64(2));
    Connection connection(rstd::move(server), limits);
    auto       result = co_await connection.read_request();
    co_return result.is_err() &&
        result.unwrap_err().kind == ConnectionErrorKind::Timeout&& connection.is_closed();
}
auto cancelled(rstd::net::TcpStream server, rstd::net::TcpStream&) -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    auto       handle = rstd::async::spawn(connection.read_request());
    co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(2)));
    handle.abort();
    auto joined = co_await rstd::move(handle);
    EXPECT_TRUE(joined.is_err());
    EXPECT_TRUE(connection.is_closed());
    co_return joined.is_err() && connection.is_closed();
}
auto truncated(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(client, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nx"_str)))
        co_return false;
    if (client.shutdown().is_err()) co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    for (;;) {
        auto part = co_await connection.read_body();
        if (part.is_err())
            co_return part.unwrap_err().decode.is_some() &&
                *part.unwrap_err().decode == DecodeError::Truncated&& connection.is_closed();
        if (part->is_none()) co_return false;
    }
}
auto body_timeout(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    ConnectionLimits limits;
    limits.body_timeout = rstd::time::Duration::from_millis(u64(2));
    Connection connection(rstd::move(server), limits);
    if (! (co_await send(client, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n"_str)))
        co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    auto body = co_await connection.read_body();
    co_return body.is_err() &&
        body.unwrap_err().kind == ConnectionErrorKind::Timeout&& connection.is_closed();
}
auto write_timeout(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    ConnectionLimits limits;
    limits.write_timeout = rstd::time::Duration::from_secs(u64());
    Connection connection(rstd::move(server), limits);
    if (! (co_await send(client, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"_str))) co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    Response response;
    auto     written = co_await connection.respond(response);
    co_return written.is_err() &&
        written.unwrap_err().kind == ConnectionErrorKind::Timeout&& connection.is_closed();
}
auto unknown_expectation(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(client, "POST / HTTP/1.1\r\nHost: x\r\nExpect: custom\r\n\r\n"_str)))
        co_return false;
    auto head = co_await connection.read_request();
    if (! (co_await receive(client,
                            "HTTP/1.1 417 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    co_return head.is_err() &&
        head.unwrap_err().kind == ConnectionErrorKind::Expectation&& connection.is_closed();
}
auto clean_eof(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (client.shutdown().is_err()) co_return false;
    auto head = co_await connection.read_request();
    co_return head.is_ok() && head->is_none() && connection.is_closed();
}
auto reject_request(rstd::net::TcpStream server, rstd::net::TcpStream& client, ref<str> wire)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(client, wire))) co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    Response response;
    response.status = u16(403);
    auto rejected   = co_await connection.reject(response);
    if (rejected.is_err() || ! connection.is_closed()) co_return false;
    if (! (co_await receive(client,
                            "HTTP/1.1 403 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    auto rest = rstd::bytes::BytesMut::with_capacity(usize(1));
    auto eof  = co_await rstd::async::io::read(client, rest);
    if (eof.is_err() || *eof != usize()) co_return false;
    if ((co_await connection.accept_body()).is_ok() || (co_await connection.read_body()).is_ok())
        co_return false;
    co_return (co_await connection.read_request()).is_err();
}
auto reject_expect(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    co_return co_await reject_request(
        rstd::move(server),
        client,
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n"_str);
}
auto reject_pipeline(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    co_return co_await reject_request(
        rstd::move(server),
        client,
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\noneGET /next HTTP/1.1\r\nHost: x\r\n\r\n"_str);
}
auto continue_client(rstd::net::TcpStream& client) -> rstd::async::coro<bool> {
    if (! (co_await receive(client, "HTTP/1.1 100 Continue\r\n\r\n"_str))) co_return false;
    co_return co_await send(client, "x"_str);
}
auto cancel_rejection(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(client, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n"_str)))
        co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    Response response;
    response.status = u16(403);
    auto rejected   = rstd::async::spawn(connection.reject(response));
    if (! (co_await receive(
            client, "HTTP/1.1 403 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str))) {
        rejected.abort();
        (void)(co_await rstd::move(rejected));
        co_return false;
    }
    rejected.abort();
    auto joined = co_await rstd::move(rejected);
    co_return joined.is_err() && connection.is_closed();
}
auto empty_expect(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(
            client,
            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\n\r\n"_str)))
        co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    if ((co_await connection.accept_body()).is_err()) co_return false;
    auto body = co_await connection.read_body();
    if (body.is_err() || body->is_some()) co_return false;
    Response response;
    if ((co_await connection.respond(response, true)).is_err()) co_return false;
    co_return co_await receive(
        client, "HTTP/1.1 200 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str);
}
auto implicit_continue(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(
            client,
            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\n"_str)))
        co_return false;
    auto head = co_await connection.read_request();
    if (head.is_err() || head->is_none()) co_return false;
    auto sender = rstd::async::AbortOnDropHandle { rstd::async::spawn(continue_client(client)) };
    auto part   = co_await connection.read_body();
    auto sent   = co_await rstd::move(sender);
    if (part.is_err() || part->is_none() || sent.is_err() || ! *sent) co_return false;
    auto bytes = rstd::move(*part).unwrap();
    if (bytes.len() != usize(1) || bytes[usize()] != u8('x')) co_return false;
    Response response;
    if ((co_await connection.respond(response, true)).is_err()) co_return false;
    co_return co_await receive(
        client, "HTTP/1.1 200 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str);
}
template<typename F>
auto scenario(rstd::net::TcpListener& listener, rstd::net::SocketAddr address, F function)
    -> rstd::async::coro<bool> {
    auto connected = co_await rstd::net::TcpStream::connect(address);
    if (connected.is_err()) co_return false;
    auto accepted = co_await listener.accept();
    if (accepted.is_err()) co_return false;
    auto pair   = rstd::move(accepted).unwrap();
    auto client = rstd::move(connected).unwrap();
    co_return co_await function(rstd::move(pair.template get<0>()), client);
}
template<typename F>
void check(F function) {
    auto bound = rstd::net::TcpListener::bind(rstd::net::SocketAddr::ipv4_loopback(u16()));
    ASSERT_TRUE(bound.is_ok());
    auto listener = rstd::move(bound).unwrap();
    auto address  = listener.local_addr().unwrap();
    auto runtime  = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    auto handle =
        rstd::async::AbortOnDropHandle { runtime.spawn(scenario(listener, address, function)) };
    auto result = runtime.block_on(
        rstd::async::timeout(rstd::move(handle), rstd::time::Duration::from_secs(u64(5))));
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(result->is_ok());
    EXPECT_TRUE(**result);
}
} // namespace

TEST(Connection, PipelinedRequestsAndStateOrder) {
    check(pipeline);
}

TEST(ConnectionUpgrade, TransfersStreamAndBufferedBytesOnce) {
    check([](rstd::net::TcpStream server, rstd::net::TcpStream& client) -> rstd::async::coro<bool> {
        Connection connection(rstd::move(server));
        auto       protocol = UpgradeProtocol::make("websocket"_str).unwrap();
        if (connection.prepare_upgrade(protocol).is_ok()) co_return false;
        if (! (co_await send(
                client,
                "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\nfirst-frame"_str)))
            co_return false;
        auto head = co_await connection.read_request();
        if (head.is_err() || head->is_none()) co_return false;
        auto prepared  = connection.prepare_upgrade(protocol);
        auto duplicate = connection.prepare_upgrade(protocol);
        if (prepared.is_err() || duplicate.is_err()) co_return false;
        auto result = co_await connection.upgrade(rstd::move(prepared).unwrap());
        if (result.is_err() || ! connection.is_upgraded()) co_return false;
        if ((co_await connection.upgrade(rstd::move(duplicate).unwrap())).is_ok()) co_return false;
        if ((co_await connection.read_request()).is_ok()) co_return false;
        if (! (co_await receive(
                client,
                "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n"_str)))
            co_return false;
        if (rstd::str_::from_utf8(result->pending.as_slice()).unwrap() != "first-frame"_str)
            co_return false;
        connection.close();
        if (! (co_await send(result->stream, "reply"_str)) ||
            ! (co_await receive(client, "reply"_str)))
            co_return false;
        co_return (co_await send(client, "next"_str)) &&
            (co_await receive(result->stream, "next"_str));
    });
}

TEST(ConnectionUpgrade, PreparedResponseCannotCrossRequests) {
    check([](rstd::net::TcpStream server, rstd::net::TcpStream& client) -> rstd::async::coro<bool> {
        Connection connection(rstd::move(server));
        if (! (co_await send(
                client,
                "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n"_str)))
            co_return false;
        if ((co_await connection.read_request()).is_err()) co_return false;
        auto prepared = connection.prepare_upgrade(UpgradeProtocol::make("websocket"_str).unwrap());
        if (prepared.is_err() || (co_await connection.respond(Response {})).is_err())
            co_return false;
        if ((co_await connection.read_request()).is_err()) co_return false;
        auto stale = co_await connection.upgrade(rstd::move(prepared).unwrap());
        if (stale.is_ok() || stale.unwrap_err().kind != ConnectionErrorKind::State) co_return false;
        co_return (co_await connection.respond(Response {}, true)).is_ok();
    });
}

TEST(ConnectionUpgrade, WriteTimeoutClosesWithoutHandoff) {
    check([](rstd::net::TcpStream server, rstd::net::TcpStream& client) -> rstd::async::coro<bool> {
        ConnectionLimits limits;
        limits.write_timeout = rstd::time::Duration::from_secs(u64());
        Connection connection(rstd::move(server), limits);
        if (! (co_await send(
                client,
                "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n"_str)))
            co_return false;
        if ((co_await connection.read_request()).is_err()) co_return false;
        auto prepared = connection.prepare_upgrade(UpgradeProtocol::make("websocket"_str).unwrap());
        if (prepared.is_err()) co_return false;
        auto result = co_await connection.upgrade(rstd::move(prepared).unwrap());
        co_return result.is_err() &&
            result.unwrap_err().kind == ConnectionErrorKind::Timeout&& connection.is_closed();
    });
}
TEST(Connection, ContinueAndChunkedBody) {
    check(expectation);
}
TEST(Connection, HeaderTimeoutClosesSocket) {
    check(timeout);
}
TEST(Connection, CancellationClosesSocket) {
    check(cancelled);
}
TEST(Connection, TruncatedBodyClosesSocket) {
    check(truncated);
}
TEST(Connection, BodyTimeoutClosesSocket) {
    check(body_timeout);
}
TEST(Connection, WriteDeadlineClosesSocket) {
    check(write_timeout);
}
TEST(Connection, RejectsUnknownExpectation) {
    check(unknown_expectation);
}
TEST(Connection, CleanEofHasNoRequest) {
    check(clean_eof);
}
TEST(Connection, RejectsBeforeSendingContinue) {
    check(reject_expect);
}
TEST(Connection, RejectionDiscardsBufferedPipeline) {
    check(reject_pipeline);
}
TEST(Connection, FirstBodyReadSendsContinue) {
    check(implicit_continue);
}
TEST(Connection, CancelDuringRejectionClosesSocket) {
    check(cancel_rejection);
}
TEST(Connection, EmptyBodyDoesNotSendContinue) {
    check(empty_expect);
}

namespace
{
auto typed_target(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(
            client,
            "GET http://notebook.test/a%2Fb HTTP/1.1\r\nHost: other.test\r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    auto request = co_await connection.read_request();
    if (request.is_err() || request->is_none()) co_return false;
    auto head = rstd::move(*request).unwrap();
    if (head.authority.is_none() || head.authority->name.as_str() != "notebook.test"_str ||
        head.line.resource.segments.len() != usize(1))
        co_return false;
    Response response;
    response.body =
        rstd::bytes::Bytes::copy_from_slice(head.line.resource.segments[usize()].as_slice());
    if ((co_await connection.respond(response)).is_err()) co_return false;
    co_return co_await receive(
        client, "HTTP/1.1 200 \r\nContent-Length: 3\r\nConnection: close\r\n\r\na/b"_str);
}
} // namespace

TEST(Connection, ConsumesTypedTargetWithoutReparsing) {
    check(typed_target);
}

namespace
{
auto streaming(rstd::net::TcpStream server, rstd::net::TcpStream& client)
    -> rstd::async::coro<bool> {
    Connection         connection(rstd::move(server));
    StreamResponseHead response;
    auto               bytes = rstd::bytes::Bytes::copy_from_slice("abc"_str.as_bytes());
    if ((co_await connection.begin_response(response)).is_ok() ||
        (co_await connection.write_body(bytes)).is_ok() || connection.finish_response().is_ok())
        co_return false;
    if (! (co_await send(client, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx"_str)))
        co_return false;
    auto request = co_await connection.read_request();
    if (request.is_err() || request->is_none()) co_return false;
    if ((co_await connection.begin_response(response)).is_ok()) co_return false;
    for (;;) {
        auto part = co_await connection.read_body();
        if (part.is_err()) co_return false;
        if (part->is_none()) break;
    }
    if ((co_await connection.begin_response(response)).is_err()) co_return false;
    if (! (co_await receive(client, "HTTP/1.1 200 \r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    Response complete;
    if (connection.ready_for_request() || (co_await connection.read_request()).is_ok() ||
        (co_await connection.respond(complete)).is_ok() ||
        (co_await connection.begin_response(response)).is_ok())
        co_return false;
    for (usize i {}; i < usize(3); ++i) {
        if ((co_await connection.write_body(bytes)).is_err()) co_return false;
        if (! (co_await receive(client, "abc"_str))) co_return false;
    }
    if (connection.finish_response().is_err() || ! connection.is_closed()) co_return false;
    auto tail = rstd::bytes::BytesMut::with_capacity(usize(1));
    auto eof  = co_await rstd::async::io::read(client, tail);
    co_return eof.is_ok() && *eof == usize() && (co_await connection.write_body(bytes)).is_err() &&
        connection.finish_response().is_err();
}
auto streaming_body_rules(rstd::net::TcpStream  server,
                          rstd::net::TcpStream& client,
                          ref<str>              method,
                          u16                   status) -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    if (! (co_await send(client, method)) ||
        ! (co_await send(client, " / HTTP/1.1\r\nHost: x\r\n\r\n"_str)))
        co_return false;
    auto request = co_await connection.read_request();
    if (request.is_err() || request->is_none()) co_return false;
    StreamResponseHead response;
    response.status = status;
    if ((co_await connection.begin_response(response)).is_err()) co_return false;
    auto bytes   = rstd::bytes::Bytes::copy_from_slice("forbidden"_str.as_bytes());
    auto written = co_await connection.write_body(bytes);
    co_return written.is_err() && written.unwrap_err().response.is_some() &&
        *written.unwrap_err().response == ResponseError::InvalidBody&& connection.is_closed();
}
} // namespace

TEST(Connection, StreamsMultiplePartsThenCloses) {
    check(streaming);
}

namespace
{
auto fixed_stream(rstd::net::TcpStream server, rstd::net::TcpStream& client, int mode)
    -> rstd::async::coro<bool> {
    Connection connection(rstd::move(server));
    auto       wire = mode == 3 ? "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n"_str
                                : "GET / HTTP/1.1\r\nHost: x\r\n\r\n"_str;
    if (! (co_await send(client, wire))) co_return false;
    auto request = co_await connection.read_request();
    if (request.is_err() || request->is_none()) co_return false;
    StreamResponseHead response;
    response.content_length = Some(u64(mode == 4 ? 0 : 3));
    if ((co_await connection.begin_response(response)).is_err()) co_return false;
    if (! (co_await receive(
            client,
            mode == 4 ? "HTTP/1.1 200 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n"_str
                      : "HTTP/1.1 200 \r\nContent-Length: 3\r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    if (mode < 3) {
        auto first = rstd::bytes::Bytes::copy_from_slice("ab"_str.as_bytes());
        if ((co_await connection.write_body(first)).is_err()) co_return false;
        if (! (co_await receive(client, "ab"_str))) co_return false;
        if (mode == 2) {
            auto excessive = co_await connection.write_body(first);
            co_return excessive.is_err() && connection.is_closed() &&
                excessive.as_ref().unwrap_err().response.is_some() &&
                *excessive.as_ref().unwrap_err().response == ResponseError::InvalidBody;
        }
        if (mode == 0) {
            auto last = rstd::bytes::Bytes::copy_from_slice("c"_str.as_bytes());
            if ((co_await connection.write_body(last)).is_err()) co_return false;
            if (! (co_await receive(client, "c"_str))) co_return false;
        }
    }
    auto finished = connection.finish_response();
    if (! connection.is_closed() || (mode == 1 ? finished.is_ok() : finished.is_err()))
        co_return false;
    if (mode == 1 && (! finished.as_ref().unwrap_err().response.is_some() ||
                      *finished.as_ref().unwrap_err().response != ResponseError::InvalidBody))
        co_return false;
    auto tail = rstd::bytes::BytesMut::with_capacity(usize(1));
    auto eof  = co_await rstd::async::io::read(client, tail);
    co_return eof.is_ok() && *eof == usize();
}
} // namespace

TEST(Connection, FixedStreamLengthIsEnforcedAndHeadOmitsBody) {
    for (int mode = 0; mode != 5; ++mode)
        check([mode](auto server, auto& client) {
            return fixed_stream(rstd::move(server), client, mode);
        });
}

namespace
{
auto stream_budget(rstd::net::TcpStream server, rstd::net::TcpStream& client, int mode)
    -> rstd::async::coro<bool> {
    ConnectionLimits limits;
    limits.write_timeout = rstd::time::Duration::from_millis(u64(100));
    if (mode != 0)
        limits.stream_write_timeout =
            Some(rstd::time::Duration::from_millis(u64(mode == 1 ? 100 : 0)));
    Connection connection(rstd::move(server), limits);
    if (! (co_await send(client, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"_str))) co_return false;
    auto request = co_await connection.read_request();
    if (request.is_err() || request->is_none()) co_return false;
    StreamResponseHead response;
    if ((co_await connection.begin_response(response)).is_err()) co_return false;
    if (! (co_await receive(client, "HTTP/1.1 200 \r\nConnection: close\r\n\r\n"_str)))
        co_return false;
    co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(150)));
    auto bytes   = rstd::bytes::Bytes::copy_from_slice("x"_str.as_bytes());
    auto written = co_await connection.write_body(bytes);
    if (mode != 1)
        co_return written.is_err() &&
            written.unwrap_err().kind == ConnectionErrorKind::Timeout&& connection.is_closed();
    if (written.is_err() || ! (co_await receive(client, "x"_str))) co_return false;
    co_return connection.finish_response().is_ok();
}
} // namespace

TEST(Connection, StreamBudgetDefaultsToResponseDeadline) {
    check([](auto server, auto& client) {
        return stream_budget(rstd::move(server), client, 0);
    });
}
TEST(Connection, StreamBudgetCanBoundEachWriteIndependently) {
    check([](auto server, auto& client) {
        return stream_budget(rstd::move(server), client, 1);
    });
}
TEST(Connection, StreamBudgetZeroClosesConnection) {
    check([](auto server, auto& client) {
        return stream_budget(rstd::move(server), client, 2);
    });
}
TEST(Connection, StreamRejectsBodyForHeadAndNoContentStatuses) {
    for (auto status : array<u16, 3> { u16(204), u16(205), u16(304) }) {
        check([status](auto server, auto& client) {
            return streaming_body_rules(rstd::move(server), client, "GET"_str, status);
        });
    }
    check([](auto server, auto& client) {
        return streaming_body_rules(rstd::move(server), client, "HEAD"_str, u16(200));
    });
}
