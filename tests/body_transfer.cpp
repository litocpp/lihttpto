#include <rstd/test/gtest.hpp>
import lihttpto;
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace lihttpto;
using Bytes = rstd::bytes::Bytes;

struct TransferSource {
    using Error = i32;
    usize reads {};
    usize writes {};
    bool  fail {};
    bool  ordered { true };
    auto  next() -> rstd::async::coro<Result<Option<Bytes>, Error>> {
        if (reads != writes) ordered = false;
        if (fail) co_return Err(i32(11));
        if (reads == usize(3)) co_return Ok<Option<Bytes>>(None());
        ++reads;
        co_return Ok(Some(Bytes::copy_from_slice("abc"_str.as_bytes())));
    }
};
struct TransferSink {
    using Error = i32;
    TransferSource& source;
    bool            fail {};
    auto            write(Bytes bytes) -> rstd::async::coro<Result<empty, Error>> {
        co_await rstd::async::sleep(rstd::time::Duration::from_millis(u64(1)));
        if (fail) co_return Err(i32(12));
        EXPECT_EQ(bytes.len(), usize(3));
        ++source.writes;
        co_return Ok(empty {});
    }
};
static_assert(BodySource<TransferSource>);
static_assert(BodySink<TransferSink>);
static_assert(! BodySource<i32>);
static_assert(! BodySink<i32>);

TEST(BodyTransfer, AwaitsEachWriteBeforeReadingAgain) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    TransferSource source;
    TransferSink   sink { source };
    EXPECT_EQ(runtime.block_on(transfer_body(source, sink)).unwrap(), u64(9));
    EXPECT_TRUE(source.ordered);
    EXPECT_EQ(source.writes, usize(3));
}
TEST(BodyTransfer, PreservesSourceAndSinkErrorsWithoutReadingAhead) {
    auto runtime = rstd::async::RuntimeBuilder::current_thread().enable_all().build().unwrap();
    TransferSource source;
    TransferSink   sink { source, true };
    auto           write_error = runtime.block_on(transfer_body(source, sink)).unwrap_err();
    EXPECT_EQ(write_error.as_Sink().error, i32(12));
    EXPECT_EQ(source.reads, usize(1));
    source.reads    = usize();
    source.fail     = true;
    auto read_error = runtime.block_on(transfer_body(source, sink)).unwrap_err();
    EXPECT_EQ(read_error.as_Source().error, i32(11));
    EXPECT_EQ(source.reads, usize());
}
