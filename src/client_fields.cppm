export module lihttpto:client_fields;
export import :messages;
export import :response;
export import :cache;
using namespace rstd::prelude;
using namespace rstd::literals;
export namespace lihttpto
{
auto redirect_target(const ResponseHead& head, const Url& base)
    -> Result<Option<Url>, ResponseError> {
    auto status = head.status.value();
    if (status != u16(301) && status != u16(302) && status != u16(303) && status != u16(307) &&
        status != u16(308))
        return Ok(None());
    auto location = head.headers.get_unique_text("location"_str);
    if (location.is_err()) return Err(ResponseError::InvalidHeader);
    if (location->is_none()) return Ok(None());
    auto target = base.resolve(**location);
    if (target.is_err()) return Err(ResponseError::InvalidHeader);
    return Ok(Some(rstd::move(target).unwrap()));
}
struct RetryAfter {
    Option<i64> seconds;
    Option<i64> date;
    auto        delay(i64 now) const -> i64 {
        return seconds.is_some() ? *seconds
                                 : (date.is_some() ? rstd::cmp::max(i64(), *date - now) : i64());
    }
};
auto retry_after(const Headers& headers) -> Result<Option<RetryAfter>, ResponseError> {
    auto value = headers.get_unique_text("retry-after"_str);
    if (value.is_err()) return Err(ResponseError::InvalidHeader);
    if (value->is_none()) return Ok(None<RetryAfter>());
    auto text    = (**value)->trim_ascii();
    auto seconds = delta_seconds(text);
    if (seconds.is_some()) return Ok(Some(RetryAfter { seconds, {} }));
    auto date = http_date(text);
    if (date.is_none()) return Err(ResponseError::InvalidHeader);
    return Ok(Some(RetryAfter { {}, date }));
}
} // namespace lihttpto
