module lihttpto;
import :date;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace rstd::time;

constexpr array<ref<str>, 12> http_months { "Jan"_str, "Feb"_str, "Mar"_str, "Apr"_str,
                                            "May"_str, "Jun"_str, "Jul"_str, "Aug"_str,
                                            "Sep"_str, "Oct"_str, "Nov"_str, "Dec"_str };
constexpr array<ref<str>, 7>  http_weekdays { "Sun"_str, "Mon"_str, "Tue"_str, "Wed"_str,
                                              "Thu"_str, "Fri"_str, "Sat"_str };
constexpr array<ref<str>, 7>  http_long_weekdays { "Sunday"_str,    "Monday"_str,   "Tuesday"_str,
                                                   "Wednesday"_str, "Thursday"_str, "Friday"_str,
                                                   "Saturday"_str };

auto date_digits(ref<str> text) -> Option<int> {
    if (text.is_empty()) return None();
    int value {};
    for (auto byte : text.as_bytes()) {
        if (! rstd::ascii::is_digit(byte)) return None();
        value = value * 10 + (byte - u8('0')).to_primitive();
    }
    return Some(value);
}

auto parse_http_date_parts(ref<str> text, i64 reference_time) -> Option<i64> {
    ref<str> day, month, year, clock;
    bool     obsolete {};
    auto     part = [&](unsigned start, unsigned count) {
        return text.get(usize(start), usize(start + count)).unwrap();
    };
    auto known_weekday = [](ref<str> value, const auto& names) {
        return names.iter().any([&](ref<ref<str>> name) {
            return *name == value;
        });
    };
    if (text.len() == usize(29) && part(3, 2) == ", "_str && part(7, 1) == " "_str &&
        part(11, 1) == " "_str && part(16, 1) == " "_str && part(25, 4) == " GMT"_str &&
        known_weekday(part(0, 3), http_weekdays)) {
        day   = part(5, 2);
        month = part(8, 3);
        year  = part(12, 4);
        clock = part(17, 8);
    } else if (text.len() == usize(24) && part(3, 1) == " "_str && part(7, 1) == " "_str &&
               part(10, 1) == " "_str && part(19, 1) == " "_str &&
               known_weekday(part(0, 3), http_weekdays)) {
        day   = part(8, 2).trim_ascii();
        month = part(4, 3);
        clock = part(11, 8);
        year  = part(20, 4);
    } else {
        usize comma {};
        while (comma < text.len() && text.as_bytes()[comma] != u8(',')) ++comma;
        if (comma + usize(24) != text.len() ||
            ! known_weekday(text.get(usize(), comma).unwrap(), http_long_weekdays))
            return None();
        auto tail = text.get(comma, text.len()).unwrap();
        if (tail.get(usize(), usize(2)).unwrap() != ", "_str ||
            tail.as_bytes()[usize(4)] != u8('-') || tail.as_bytes()[usize(8)] != u8('-') ||
            tail.as_bytes()[usize(11)] != u8(' ') ||
            tail.get(usize(20), usize(24)).unwrap() != " GMT"_str)
            return None();
        day      = tail.get(usize(2), usize(4)).unwrap();
        month    = tail.get(usize(5), usize(8)).unwrap();
        year     = tail.get(usize(9), usize(11)).unwrap();
        clock    = tail.get(usize(12), usize(20)).unwrap();
        obsolete = true;
    }
    if (clock.as_bytes()[usize(2)] != u8(':') || clock.as_bytes()[usize(5)] != u8(':'))
        return None();
    auto d = date_digits(day), y = date_digits(year);
    auto h = date_digits(clock.get(usize(), usize(2)).unwrap());
    auto m = date_digits(clock.get(usize(3), usize(5)).unwrap());
    auto s = date_digits(clock.get(usize(6), usize(8)).unwrap());
    if (d.is_none() || y.is_none() || h.is_none() || m.is_none() || s.is_none() || *s > 60)
        return None();
    unsigned number {};
    for (auto name : http_months) {
        ++number;
        if (name == month) break;
    }
    if (number > 12 || http_months[usize(number - 1)] != month) return None();
    auto time = Time::from_hms_nano(u8(*h), u8(*m), u8(rstd::min(*s, 59)), u32());
    if (time.is_err()) return None();
    if (obsolete) {
        auto reference = OffsetDateTime::from_unix_time(reference_time, u32());
        if (reference.is_err()) return None();
        auto current_year = reference->date().year().to_primitive();
        *y += current_year / 100 * 100;
        if (*y < current_year - 49) *y += 100;
        array<int, 5> candidate { int(number), *d, *h, *m, *s };
        array<int, 5> limit { reference->date().month().number().to_primitive(),
                              reference->date().day().to_primitive(),
                              reference->time().hour().to_primitive(),
                              reference->time().minute().to_primitive(),
                              reference->time().second().to_primitive() };
        bool          later {};
        for (usize index {}; index < candidate.len(); ++index) {
            if (candidate[index] == limit[index]) continue;
            later = candidate[index] > limit[index];
            break;
        }
        if (*y > current_year + 50 || (*y == current_year + 50 && later)) *y -= 100;
    }
    auto date = Date::from_calendar_date(i32(*y), Month::from_number(u8(number)).unwrap(), u8(*d));
    if (date.is_err()) return None();
    return Some(PlainDateTime::make(*date, *time).assume_utc().unix_time().seconds + i64(*s == 60));
}

namespace lihttpto
{
auto http_date(ref<str> text) -> Option<i64> {
    return http_date(text, SystemTime::now().as_unix_time().seconds);
}
auto http_date(ref<str> text, i64 reference_time) -> Option<i64> {
    if (text.len() > usize(128)) return None();
    return parse_http_date_parts(text, reference_time);
}
auto format_http_date(i64 seconds) -> Option<String> {
    auto value = OffsetDateTime::from_unix_time(seconds, u32());
    if (value.is_err() || value->date().year() < i32()) return None();
    auto days = seconds.to_primitive() / 86400;
    if (seconds.to_primitive() % 86400 < 0) --days;
    auto weekday = (days + 4) % 7;
    if (weekday < 0) weekday += 7;
    auto two = [](unsigned value) {
        array<u8, 2> bytes { u8('0' + value / 10), u8('0' + value % 10) };
        return String::make(rstd::str_::from_utf8(bytes.as_slice()).unwrap());
    };
    auto year = value->date().year().to_primitive();
    return Some(rstd::format("{}, {} {} {}{} {}:{}:{} GMT",
                             http_weekdays[usize(weekday)],
                             two(value->date().day().to_primitive()),
                             http_months[usize(value->date().month().number().to_primitive() - 1)],
                             two(unsigned(year / 100)),
                             two(unsigned(year % 100)),
                             two(value->time().hour().to_primitive()),
                             two(value->time().minute().to_primitive()),
                             two(value->time().second().to_primitive())));
}
} // namespace lihttpto
