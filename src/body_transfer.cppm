export module lihttpto:body_transfer;
export import :body;
export import :connection;

using namespace rstd::prelude;
using Bytes = rstd::bytes::Bytes;
export namespace lihttpto
{
class RequestBodySource {
    Connection& connection_;

public:
    using Error = ConnectionError;
    explicit RequestBodySource(Connection& connection): connection_(connection) {}
    auto next() { return connection_.read_body(); }
};
class ResponseBodySink {
    Connection& connection_;

public:
    using Error = ConnectionError;
    explicit ResponseBodySink(Connection& connection): connection_(connection) {}
    auto write(Bytes bytes) -> rstd::async::coro<Result<empty, Error>> {
        co_return co_await connection_.write_body(bytes);
    }
};
} // namespace lihttpto
