module;
#include <rstd/enum.hpp>
export module lihttpto:body;
export import rstd;
import rstd;
using namespace rstd::prelude;
using Bytes = rstd::bytes::Bytes;

export namespace lihttpto
{
template<class T>
concept BodySource = requires(T& source) {
    typename T::Error;
    requires rstd::async::AwaitableInput<decltype(source.next())>;
    requires rstd::mtp::same_as<rstd::async::await_output_t<decltype(source.next())>,
                                Result<Option<Bytes>, typename T::Error>>;
};
template<class T>
concept BodySink = requires(T& sink, Bytes bytes) {
    typename T::Error;
    requires rstd::async::AwaitableInput<decltype(sink.write(rstd::move(bytes)))>;
    requires rstd::mtp::same_as<
        rstd::async::await_output_t<decltype(sink.write(rstd::move(bytes)))>,
        Result<empty, typename T::Error>>;
};
template<class ReadError, class WriteError>
class BodyTransferError {
    RSTD_ENUM(BodyTransferError, (Source, (ReadError error;)), (Sink, (WriteError error;)))
};
// EOF does not finalize the sink; its owner controls response or resource completion.
template<BodySource Source, BodySink Sink>
auto transfer_body(Source& source, Sink& sink) -> rstd::async::coro<
    Result<u64, BodyTransferError<typename Source::Error, typename Sink::Error>>> {
    using Error = BodyTransferError<typename Source::Error, typename Sink::Error>;
    u64 count {};
    for (;;) {
        auto part = co_await source.next();
        if (part.is_err()) co_return Err(Error::Source(rstd::move(part).unwrap_err()));
        if (part->is_none()) co_return Ok(count);
        auto bytes   = part->take().unwrap();
        auto length  = u64(bytes.len().to_primitive());
        auto written = co_await sink.write(rstd::move(bytes));
        if (written.is_err()) co_return Err(Error::Sink(rstd::move(written).unwrap_err()));
        count += length;
    }
}
} // namespace lihttpto
