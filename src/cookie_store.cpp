module lihttpto;
import :cookie_store;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace lihttpto
{
auto domain_matches(ref<str> host, ref<str> domain, bool numeric = false) -> bool {
    return host == domain || (! numeric && host.len() > domain.len() && host.ends_with(domain) &&
                              host.as_bytes()[host.len() - domain.len() - usize(1)] == u8('.'));
}
auto path_matches(ref<str> target, ref<str> path) -> bool {
    return target == path ||
           (target.starts_with(path) &&
            (path.ends_with("/"_str) || target.as_bytes()[path.len()] == u8('/')));
}
auto default_cookie_path(const Url& url) -> String {
    auto path = url.path();
    if (! path.starts_with("/"_str)) return "/"_Str;
    usize last {};
    for (usize i {}; i < path.len(); ++i)
        if (path.as_bytes()[i] == u8('/')) last = i;
    return last == usize() ? "/"_Str : String::make(path.get(usize(), last).unwrap());
}
auto canonical_cookie_domain(ref<str> domain) -> Option<String> {
    if (domain.starts_with("."_str)) domain = domain.get(usize(1), domain.len()).unwrap();
    if (domain.is_empty() || domain.ends_with("."_str)) return None();
    auto lower = String::make(domain);
    lower.as_mut_str().make_ascii_lowercase();
    auto parsed = Url::parse_http(rstd::format("http://{}/", lower).as_str());
    if (parsed.is_err() || parsed->userinfo().is_some() || parsed->port().is_some() ||
        parsed->host().is_none() || *parsed->host() != lower.as_str() ||
        parsed->path() != "/"_str || parsed->query().is_some() || parsed->fragment().is_some())
        return None();
    return Some(rstd::move(lower));
}
auto cookie_age(ref<str> value) -> Option<i64> {
    bool negative = value.starts_with("-"_str);
    if (negative) value = value.get(usize(1), value.len()).unwrap();
    if (value.is_empty()) return None();
    i64 number {};
    for (auto ch : value.as_bytes()) {
        if (ch < u8('0') || ch > u8('9')) return None();
        auto digit = i64((ch - u8('0')).to_primitive());
        number     = number > (i64::MAX - digit) / i64(10) ? i64::MAX : number * i64(10) + digit;
    }
    return Some(negative ? -number : number);
}
auto CookieStorage::allowed_domain(ref<str> domain) const -> bool {
    return options_.suffixes.is_some() && ! (*options_.suffixes)->is_public_suffix(domain);
}
void CookieStorage::expire(i64 now) {
    for (usize i {}; i < entries_.len();) {
        if (entries_[i].expires.is_some() && *entries_[i].expires <= now)
            entries_.remove(i);
        else
            ++i;
    }
}
auto CookieStorage::erase(ref<str> domain, ref<str> path, ref<str> name) -> bool {
    for (usize i {}; i < entries_.len(); ++i)
        if (entries_[i].domain.as_str() == domain && entries_[i].path.as_str() == path &&
            entries_[i].cookie.name() == name) {
            entries_.remove(i);
            return true;
        }
    return false;
}
void CookieStorage::insert(StoredCookie cookie, i64 now) {
    expire(now);
    if (cookie.expires.is_some() && *cookie.expires <= now) {
        (void)erase(cookie.domain.as_str(), cookie.path.as_str(), cookie.cookie.name());
        return;
    }
    for (auto& old : entries_)
        if (old.domain == cookie.domain && old.path == cookie.path &&
            old.cookie.name() == cookie.cookie.name()) {
            old = rstd::move(cookie);
            return;
        }
    if (options_.max_entries == usize()) return;
    if (entries_.len() >= options_.max_entries) entries_.remove(usize());
    entries_.push(rstd::move(cookie));
}
auto CookieStorage::store(const SetCookie& input, const Url& origin, i64 now) -> bool {
    auto http = origin.http_origin();
    if (http.is_none() || input.encode().len() > options_.max_cookie_bytes) return false;
    StoredCookie cookie {
        input.cookie().clone(), http->host.clone(), default_cookie_path(origin), {}, true,
        input.secure(),         input.http_only()
    };
    Option<i64>    age;
    Option<String> domain_attribute;
    bool           invalid_domain {};
    auto           attributes = input.attributes();
    for (;;) {
        auto next = attributes.next();
        if (next.is_none()) break;
        const auto& attribute = **next;
        auto        value     = attribute.value();
        if (value.is_none()) continue;
        if (attribute.name_equals("domain"_str)) {
            if (value->is_empty()) continue;
            auto domain      = canonical_cookie_domain(*value);
            invalid_domain   = domain.is_none();
            domain_attribute = rstd::move(domain);
        } else if (attribute.name_equals("path"_str))
            cookie.path =
                value->starts_with("/"_str) ? String::make(*value) : default_cookie_path(origin);
        else if (attribute.name_equals("max-age"_str)) {
            auto parsed = cookie_age(*value);
            if (parsed.is_some()) age = parsed;
        } else if (attribute.name_equals("expires"_str)) {
            auto parsed = cookie_date(*value);
            if (parsed.is_some()) cookie.expires = parsed;
        }
    }
    if (invalid_domain) return false;
    if (domain_attribute.is_some()) {
        auto& domain = *domain_attribute;
        if (! domain_matches(
                http->host.as_str(), domain.as_str(), origin.numeric_address().is_some()))
            return false;
        bool accepted = allowed_domain(domain.as_str()) && origin.numeric_address().is_none();
        if (! accepted && domain.as_str() != http->host.as_str()) return false;
        cookie.domain    = rstd::move(domain);
        cookie.host_only = ! accepted;
    }
    if (cookie.secure && http->scheme.as_str() != "https"_str) return false;
    if (age.is_some())
        cookie.expires = Some(i64(*age <= i64()           ? i64::MIN
                                  : now > i64::MAX - *age ? i64::MAX
                                                          : now + *age));
    insert(rstd::move(cookie), now);
    return true;
}
auto CookieStorage::select(const Url& target, i64 now) -> CookieHeader {
    expire(now);
    CookieHeader result;
    auto         origin = target.http_origin();
    if (origin.is_none()) return result;
    Vec<usize> selected;
    for (usize i {}; i < entries_.len(); ++i) {
        auto& cookie = entries_[i];
        if ((cookie.secure && origin->scheme.as_str() != "https"_str) ||
            (cookie.host_only ? cookie.domain != origin->host
                              : ! domain_matches(origin->host.as_str(),
                                                 cookie.domain.as_str(),
                                                 target.numeric_address().is_some())) ||
            ! path_matches(target.path(), cookie.path.as_str()))
            continue;
        usize position {};
        while (position < selected.len() &&
               entries_[selected[position]].path.len() >= cookie.path.len())
            ++position;
        selected.insert(position, usize(i));
    }
    for (auto index : selected) result.add(entries_[index].cookie.clone());
    return result;
}
auto CookieStorage::snapshot(i64 now) -> Vec<StoredCookie> {
    expire(now);
    Vec<StoredCookie> copy;
    for (const auto& cookie : entries_) copy.push(cookie.clone());
    return copy;
}
auto CookieStorage::export_netscape(i64 now) -> String {
    expire(now);
    auto output = "# Netscape HTTP Cookie File\n"_Str;
    for (const auto& cookie : entries_) {
        auto value     = cookie.cookie.encode();
        auto separator = value.as_str().find("="_str).unwrap();
        output.push_str(rstd::format("{}{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                                     cookie.http_only ? "#HttpOnly_"_str : ""_str,
                                     cookie.domain,
                                     cookie.host_only ? "FALSE"_str : "TRUE"_str,
                                     cookie.path,
                                     cookie.secure ? "TRUE"_str : "FALSE"_str,
                                     cookie.expires.is_some() ? *cookie.expires : i64(),
                                     cookie.cookie.name(),
                                     value.as_str().get(separator + usize(1), value.len()).unwrap())
                            .as_str());
    }
    return output;
}
auto CookieStorage::import_netscape(ref<str> input, i64 now) -> Result<empty, CookieFileError> {
    for (auto ch : input.as_bytes())
        if (ch == u8()) return Err(CookieFileError(usize()));
    Vec<StoredCookie> parsed;
    usize             start {}, line_number {};
    while (start < input.len()) {
        ++line_number;
        usize end = start;
        while (end < input.len() && input.as_bytes()[end] != u8('\n')) ++end;
        auto line = input.get(start, end).unwrap();
        start     = end == input.len() ? end : end + usize(1);
        if (line.ends_with("\r"_str)) line = line.get(usize(), line.len() - usize(1)).unwrap();
        bool http_only = line.starts_with("#HttpOnly_"_str);
        if (http_only) line = line.get(usize(10), line.len()).unwrap();
        if (line.is_empty() || (! http_only && line.starts_with("#"_str))) continue;
        if (line.len() > options_.max_cookie_bytes + usize(1024) ||
            parsed.len() >= options_.max_entries)
            return Err(CookieFileError(line_number));
        Vec<ref<str>> fields;
        usize         field_start {};
        for (usize i {}; i <= line.len(); ++i)
            if (i == line.len() || line.as_bytes()[i] == u8('\t')) {
                fields.push(line.get(field_start, i).unwrap());
                field_start = i + usize(1);
            }
        if (fields.len() != usize(7)) return Err(CookieFileError(line_number));
        auto domain = canonical_cookie_domain(fields[usize()]);
        auto tail = fields[usize(1)], secure = fields[usize(3)], expiry = fields[usize(4)];
        auto expires = cookie_age(expiry);
        if (domain.is_none() || (tail != "TRUE"_str && tail != "FALSE"_str) ||
            (secure != "TRUE"_str && secure != "FALSE"_str) || expires.is_none() ||
            *expires < i64() || ! fields[usize(2)].starts_with("/"_str) ||
            (tail == "TRUE"_str && ! allowed_domain(domain->as_str())))
            return Err(CookieFileError(line_number));
        for (auto field : fields)
            for (auto ch : field.as_bytes())
                if (ch < u8(32) || ch == u8(127)) return Err(CookieFileError(line_number));
        auto pair =
            Cookie::parse(rstd::format("{}={}", fields[usize(5)], fields[usize(6)]).as_str());
        if (pair.is_err() || pair->encode().len() > options_.max_cookie_bytes)
            return Err(CookieFileError(line_number));
        parsed.push({ rstd::move(pair).unwrap(),
                      domain.take().unwrap(),
                      String::make(fields[usize(2)]),
                      *expires == i64() ? None<i64>() : Some(*expires),
                      tail == "FALSE"_str,
                      secure == "TRUE"_str,
                      http_only });
    }
    for (auto& cookie : parsed) insert(rstd::move(cookie), now);
    return Ok(empty {});
}
auto cookie_date(ref<str> text) -> Option<i64> {
    using namespace rstd::time;
    if (text.len() > usize(256)) return None();
    auto delimiter = [](u8 ch) {
        auto value = ch.to_primitive();
        return value == 9 || (value >= 0x20 && value <= 0x2f) || (value >= 0x3b && value <= 0x40) ||
               (value >= 0x5b && value <= 0x60) || (value >= 0x7b && value <= 0x7e);
    };
    auto number = [](ref<str> token, usize minimum, usize maximum) -> Option<int> {
        usize digits {};
        int   value {};
        while (digits < token.len() && token.as_bytes()[digits] >= u8('0') &&
               token.as_bytes()[digits] <= u8('9')) {
            if (++digits > maximum) return None();
            value = value * 10 + (token.as_bytes()[digits - usize(1)] - u8('0')).to_primitive();
        }
        return digits >= minimum ? Some(value) : None<int>();
    };
    auto exact_number = [&](ref<str> token) -> Option<int> {
        for (auto ch : token.as_bytes())
            if (ch < u8('0') || ch > u8('9')) return None();
        return number(token, usize(1), usize(2));
    };
    Option<int>           day, month, year;
    Option<array<int, 3>> clock;
    array<ref<str>, 12> months { "jan"_str, "feb"_str, "mar"_str, "apr"_str, "may"_str, "jun"_str,
                                 "jul"_str, "aug"_str, "sep"_str, "oct"_str, "nov"_str, "dec"_str };
    usize               start {};
    while (start < text.len()) {
        while (start < text.len() && delimiter(text.as_bytes()[start])) ++start;
        usize end = start;
        while (end < text.len() && ! delimiter(text.as_bytes()[end])) ++end;
        if (start == end) break;
        auto token = text.get(start, end).unwrap();
        start      = end;
        if (clock.is_none()) {
            auto first = token.find(":"_str);
            if (first.is_some()) {
                auto tail   = token.get(*first + usize(1), token.len()).unwrap();
                auto second = tail.find(":"_str);
                if (second.is_some()) {
                    auto h = exact_number(token.get(usize(), *first).unwrap());
                    auto m = exact_number(tail.get(usize(), *second).unwrap());
                    auto s = number(
                        tail.get(*second + usize(1), tail.len()).unwrap(), usize(1), usize(2));
                    if (h.is_some() && m.is_some() && s.is_some()) {
                        clock = Some(array<int, 3> { *h, *m, *s });
                        continue;
                    }
                }
            }
        }
        if (day.is_none()) {
            auto value = number(token, usize(1), usize(2));
            if (value.is_some()) {
                day = value;
                continue;
            }
        }
        if (month.is_none() && token.len() >= usize(3) &&
            (token.len() == usize(3) || token.as_bytes()[usize(3)] < u8('0') ||
             token.as_bytes()[usize(3)] > u8('9'))) {
            auto prefix = String::make(token.get(usize(), usize(3)).unwrap());
            prefix.as_mut_str().make_ascii_lowercase();
            for (usize i {}; i < months.len(); ++i)
                if (prefix.as_str() == months[i]) month = Some(int(i.to_primitive()) + 1);
            if (month.is_some()) continue;
        }
        if (year.is_none()) year = number(token, usize(2), usize(4));
    }
    if (day.is_none() || month.is_none() || year.is_none() || clock.is_none()) return None();
    if (*year >= 70 && *year <= 99)
        *year += 1900;
    else if (*year <= 69)
        *year += 2000;
    if (*year < 1601 || *day < 1 || *day > 31 || (*clock)[usize()] > 23 ||
        (*clock)[usize(1)] > 59 || (*clock)[usize(2)] > 59)
        return None();
    auto date =
        Date::from_calendar_date(i32(*year), Month::from_number(u8(*month)).unwrap(), u8(*day));
    auto time = Time::from_hms_nano(
        u8((*clock)[usize()]), u8((*clock)[usize(1)]), u8((*clock)[usize(2)]), u32());
    if (date.is_err() || time.is_err()) return None();
    return Some(PlainDateTime::make(*date, *time).assume_utc().unix_time().seconds);
}
} // namespace lihttpto
