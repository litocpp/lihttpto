export module lihttpto:body_transfer;
export import :body;
export import :connection;

using namespace rstd::prelude;
using Bytes = rstd::bytes::Bytes;
export namespace lihttpto
{
class RequestBodySource {
    Connection& connection_;
    using ReadResult = Result<Option<Bytes>, ConnectionError>;
    Option<rstd::async::CoroFuture<ReadResult>> pending_;
    bool                                        done_ {};

public:
    using Error = ConnectionError;
    using Item  = Result<Bytes, Error>;
    explicit RequestBodySource(Connection& connection): connection_(connection) {}
    auto poll_next(mut_ref<RequestBodySource> self, rstd::task::Context& cx)
        -> rstd::task::Poll<Option<Item>> {
        using Poll = rstd::task::Poll<Option<Item>>;
        if (self->done_) return Poll::Ready(None());
        if (self->pending_.is_none()) {
            self->pending_.insert(rstd::async::into_future(self->connection_.read_body()));
        }
        auto out = rstd::future::poll(*self->pending_, cx);
        if (out.is_pending()) return Poll::Pending();
        auto result    = rstd::move(out).take();
        self->pending_ = None();
        if (result.is_err()) {
            self->done_ = true;
            return Poll::Ready(Some(Item(Err(rstd::move(result).unwrap_err()))));
        }
        auto bytes = rstd::move(result).unwrap();
        if (bytes.is_none()) {
            self->done_ = true;
            return Poll::Ready(None());
        }
        return Poll::Ready(Some(Item(Ok(bytes.take().unwrap()))));
    }
    auto next() -> rstd::async::coro<ReadResult> {
        auto item = co_await rstd::stream::next(*this);
        if (item.is_none()) co_return Ok<Option<Bytes>>(None());
        auto result = item.take().unwrap();
        if (result.is_err()) co_return Err(rstd::move(result).unwrap_err());
        co_return Ok(Some(rstd::move(result).unwrap()));
    }
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
