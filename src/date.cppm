export module lihttpto:date;
export import rstd;

using namespace rstd::prelude;

export namespace lihttpto
{
auto http_date(ref<str> text) -> Option<i64>;
auto http_date(ref<str> text, i64 reference_time) -> Option<i64>;
auto format_http_date(i64 seconds) -> Option<String>;
} // namespace lihttpto
