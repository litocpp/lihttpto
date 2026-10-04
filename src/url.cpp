module;
#include <rstd/macro.hpp>
module lihttpto;
import :url;

namespace lihttpto
{

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::string::String;

auto url_parts(ref<str> input) -> Vec<ref<str>> {
    Vec<ref<str>> result;
    while (true) {
        auto parts = input.split_once("."_str);
        if (parts.is_none()) {
            result.push(rstd::move(input));
            return result;
        }
        result.push(ref<str>(parts->get<0>()));
        input = parts->get<1>();
    }
}

auto url_forbidden_host_byte(u8 byte) -> bool {
    for (auto c : " #/:<>?@[\\]^|"_str.as_bytes())
        if (byte == c) return true;
    return false;
}

auto url_default_port(ref<str> scheme) -> Option<u16> {
    if (scheme == "http"_str || scheme == "ws"_str) return Some(u16(80));
    if (scheme == "https"_str || scheme == "wss"_str) return Some(u16(443));
    if (scheme == "ftp"_str) return Some(u16(21));
    return None();
}

enum class UrlEncode
{
    C0,
    Fragment,
    Query,
    SpecialQuery,
    Path,
    Userinfo
};

auto url_encode(ref<str> input, UrlEncode set) -> String {
    auto           result = String::make();
    constexpr char hex[]  = "0123456789ABCDEF";
    for (auto byte : input.as_bytes()) {
        auto c      = byte.to_primitive();
        bool encode = c < 0x20 || c > 0x7e;
        if (set != UrlEncode::C0) encode |= c == ' ' || c == '"' || c == '<' || c == '>';
        if (set == UrlEncode::Fragment) encode |= c == '`';
        if (set >= UrlEncode::Query) encode |= c == '#';
        if (set == UrlEncode::SpecialQuery) encode |= c == '\'';
        if (set >= UrlEncode::Path)
            encode |= c == '?' || c == '^' || c == '`' || c == '{' || c == '}';
        if (set == UrlEncode::Userinfo)
            encode |= c == '/' || c == ':' || c == ';' || c == '=' || c == '@' || c == '[' ||
                      c == '\\' || c == ']' || c == '|';
        if (encode) {
            result.push_ascii(u8('%'));
            result.push_ascii(u8(hex[c >> 4]));
            result.push_ascii(u8(hex[c & 15]));
        } else {
            result.push_ascii(byte);
        }
    }
    return result;
}

auto url_hex(u8 byte) -> int {
    auto c = byte.to_primitive();
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

auto Url::Host::clone() const -> Host {
    Host result;
    result.kind = kind;
    result.text = text.clone();
    result.ipv4 = ipv4;
    for (unsigned i = 0; i < 8; ++i) result.ipv6[i] = ipv6[i];
    return result;
}

auto Url::Host::serialize() const -> String {
    if (kind == Kind::Domain || kind == Kind::Opaque) return text.clone();
    if (kind == Kind::Ipv4) {
        auto value = ipv4.to_primitive();
        return rstd::format("{}.{}.{}.{}",
                            u32(value >> 24),
                            u32((value >> 16) & 255),
                            u32((value >> 8) & 255),
                            u32(value & 255));
    }
    unsigned start = 8, length = 1;
    for (unsigned i = 0; i < 8;) {
        if (ipv6[i] != u16()) {
            ++i;
            continue;
        }
        auto begin = i;
        while (i < 8 && ipv6[i] == u16()) ++i;
        if (i - begin > length) {
            start  = begin;
            length = i - begin;
        }
    }
    auto result = String::make("["_str);
    for (unsigned i = 0; i < 8;) {
        if (i == start) {
            result.push_str("::"_str);
            i += length;
            continue;
        }
        if (i != 0 && i != start + length) result.push_ascii(u8(':'));
        result.push_str(rstd::format("{:x}", ipv6[i]).as_str());
        ++i;
    }
    result.push_ascii(u8(']'));
    return result;
}

auto Url::Record::clone() const -> Record {
    Record result;
    result.scheme   = scheme.clone();
    result.username = username.clone();
    result.password = password.clone();
    if (host.is_some()) result.host = Some(host->clone());
    result.port = port;
    for (const auto& segment : segments) result.segments.push(segment.clone());
    result.opaque      = opaque;
    result.opaque_path = opaque_path.clone();
    if (query.is_some()) result.query = Some(query->clone());
    if (fragment.is_some()) result.fragment = Some(fragment->clone());
    return result;
}

class UrlParser {
    String      input_;
    Vec<usize>  offsets_;
    usize       end_ {};
    Url::Record record_;

    auto at(usize i) const -> u8 { return input_.as_str()[i]; }
    auto size() const -> usize { return input_.size(); }
    auto slice(usize begin, usize end) const -> ref<str> {
        return *input_.as_str().get(begin, end);
    }
    auto file() const -> bool { return record_.scheme == "file"_str; }
    auto special() const -> bool {
        return file() || url_default_port(record_.scheme.as_str()).is_some();
    }
    auto slash(usize i) const -> bool {
        return i < size() && (at(i) == u8('/') || (special() && at(i) == u8('\\')));
    }
    auto error(UrlErrorKind kind, usize offset) const -> UrlError {
        return UrlError(rstd::move(kind), offset < offsets_.len() ? offsets_[offset] : end_);
    }

    auto ipv6(ref<str> text, usize offset) -> Result<Url::Host, UrlError> {
        auto invalid = [&] {
            return error(UrlErrorKind::InvalidIpAddress(), offset);
        };
        Url::Host host;
        host.kind      = Url::Host::Kind::Ipv6;
        unsigned count = 0, compress = 8;
        usize    pos {};
        if (text.starts_with(":"_str)) {
            if (! text.starts_with("::"_str)) return Err(invalid());
            compress = 0;
            pos      = usize(2);
        }
        while (pos < text.size()) {
            if (count == 8) return Err(invalid());
            auto begin = pos;
            while (pos < text.size() && text[pos] != u8(':')) ++pos;
            auto part = *text.get(begin, pos);
            if (part.is_empty()) return Err(invalid());
            if (part.contains("."_str)) {
                if (pos != text.size() || count > 6) return Err(invalid());
                unsigned octets[4] {}, n = 0;
                for (auto item : url_parts(part)) {
                    if (n == 4 || item.is_empty() || item.size() > usize(3) ||
                        (item.size() > usize(1) && item[usize()] == u8('0')))
                        return Err(invalid());
                    unsigned value = 0;
                    for (auto c : item.as_bytes()) {
                        if (! rstd::ascii::is_digit(c)) return Err(invalid());
                        value = value * 10 + c.to_primitive() - '0';
                    }
                    if (value > 255) return Err(invalid());
                    octets[n++] = value;
                }
                if (n != 4) return Err(invalid());
                host.ipv6[count++] = u16((octets[0] << 8) | octets[1]);
                host.ipv6[count++] = u16((octets[2] << 8) | octets[3]);
                break;
            }
            if (part.size() > usize(4)) return Err(invalid());
            unsigned value = 0;
            for (auto c : part.as_bytes()) {
                auto digit = url_hex(c);
                if (digit < 0) return Err(invalid());
                value = value * 16 + static_cast<unsigned>(digit);
            }
            host.ipv6[count++] = u16(value);
            if (pos == text.size()) break;
            ++pos;
            if (pos < text.size() && text[pos] == u8(':')) {
                if (compress != 8) return Err(invalid());
                compress = count;
                ++pos;
            } else if (pos == text.size())
                return Err(invalid());
        }
        if (compress != 8) {
            if (count == 8) return Err(invalid());
            auto gap = 8 - count;
            for (unsigned i = count; i > compress; --i) {
                host.ipv6[i - 1 + gap] = host.ipv6[i - 1];
                host.ipv6[i - 1]       = u16();
            }
        } else if (count != 8)
            return Err(invalid());
        return Ok(rstd::move(host));
    }

    static auto number(ref<str> text) -> Option<u64> {
        unsigned radix = 10;
        usize    pos {};
        if (text.is_empty()) return None();
        if (text.starts_with("0x"_str) || text.starts_with("0X"_str)) {
            radix = 16;
            pos   = usize(2);
        } else if (text.size() >= usize(2) && text[usize()] == u8('0')) {
            radix = 8;
            pos   = usize(1);
        }
        u64 value {};
        for (; pos < text.size(); ++pos) {
            auto digit = url_hex(text[pos]);
            if (digit < 0 || static_cast<unsigned>(digit) >= radix) return None();
            // Saturate without losing syntactic recognition of an IPv4 number.
            if (value <= u64(0xffffffff)) value = value * u64(radix) + u64(digit);
        }
        return Some(value);
    }

    auto host(ref<str> text, usize offset) -> Result<Url::Host, UrlError> {
        if (text.starts_with("["_str)) {
            if (! text.ends_with("]"_str))
                return Err(error(UrlErrorKind::InvalidIpAddress(), offset));
            return ipv6(*text.get(usize(1), text.size() - usize(1)), offset);
        }
        Url::Host result;
        if (! special()) {
            for (auto c : text.as_bytes()) {
                if (c == u8() || c == u8('\t') || c == u8('\n') || c == u8('\r') ||
                    url_forbidden_host_byte(c))
                    return Err(error(UrlErrorKind::InvalidCharacter(), offset));
            }
            result.kind = Url::Host::Kind::Opaque;
            result.text = url_encode(text, UrlEncode::C0);
            return Ok(rstd::move(result));
        }
        if (text.is_empty()) return Err(error(UrlErrorKind::MissingHost(), offset));
        Vec<u8> decoded;
        for (usize i {}; i < text.size(); ++i) {
            auto c = text[i];
            if (c == u8('%') && i + usize(2) < text.size()) {
                auto a = url_hex(text[i + usize(1)]), b = url_hex(text[i + usize(2)]);
                if (a >= 0 && b >= 0) {
                    c = u8(a * 16 + b);
                    i += usize(2);
                }
            }
            if (c <= u8(32) || c == u8(127) || c == u8('%') || url_forbidden_host_byte(c))
                return Err(error(UrlErrorKind::InvalidCharacter(), offset + i));
            if (c >= u8(128)) return Err(error(UrlErrorKind::UnsupportedHost(), offset + i));
            decoded.push(rstd::move(c));
        }
        result.text = String::from_utf8(rstd::move(decoded)).unwrap();
        result.text.as_mut_str().make_ascii_lowercase();
        auto parts = url_parts(result.text.as_str());
        for (auto part : parts) {
            if (part.starts_with("xn--"_str))
                return Err(error(UrlErrorKind::UnsupportedHost(), offset));
        }
        if (parts.len() > usize(1) && parts[parts.len() - usize(1)].is_empty()) parts.pop();
        auto last   = parts[parts.len() - usize(1)];
        bool digits = ! last.is_empty();
        for (auto c : last.as_bytes()) digits &= rstd::ascii::is_digit(c);
        if (! digits && number(last).is_none()) return Ok(rstd::move(result));
        auto invalid = [&] {
            return error(UrlErrorKind::InvalidIpAddress(), offset);
        };
        if (parts.len() > usize(4)) return Err(invalid());
        u64 value {};
        for (usize i {}; i < parts.len(); ++i) {
            auto parsed = number(parts[i]);
            if (parsed.is_none()) return Err(invalid());
            if (i + usize(1) < parts.len()) {
                if (*parsed > u64(255)) return Err(invalid());
                value += *parsed << u64(8 * (3 - i.to_primitive()));
            } else {
                auto limit = u64(1) << u64(8 * (5 - parts.len().to_primitive()));
                if (*parsed >= limit) return Err(invalid());
                value += *parsed;
            }
        }
        result.kind = Url::Host::Kind::Ipv4;
        result.ipv4 = u32(value.to_primitive());
        result.text.clear();
        return Ok(rstd::move(result));
    }

    auto authority(usize& pos) -> Result<empty, UrlError> {
        auto begin = pos;
        while (pos < size() && ! slash(pos) && at(pos) != u8('?') && at(pos) != u8('#')) ++pos;
        auto end         = pos;
        auto host_begin  = begin;
        bool credentials = false;
        for (auto i = begin; i < end; ++i) {
            if (at(i) == u8('@')) {
                credentials = true;
                host_begin  = i + usize(1);
            }
        }
        record_.username.clear();
        record_.password.clear();
        record_.port = None();
        if (credentials) {
            auto info  = slice(begin, host_begin - usize(1));
            auto parts = info.split_once(":"_str);
            record_.username =
                url_encode(parts.is_some() ? parts->get<0>() : info, UrlEncode::Userinfo);
            if (parts.is_some())
                record_.password = url_encode(parts->get<1>(), UrlEncode::Userinfo);
        }
        auto host_end = host_begin;
        if (host_begin < end && at(host_begin) == u8('[')) {
            while (host_end < end && at(host_end) != u8(']')) ++host_end;
            if (host_end == end) return Err(error(UrlErrorKind::InvalidIpAddress(), host_begin));
            ++host_end;
            if (host_end < end && at(host_end) != u8(':'))
                return Err(error(UrlErrorKind::InvalidPort(), host_end));
        } else {
            while (host_end < end && at(host_end) != u8(':')) ++host_end;
        }
        if (host_begin == host_end && (credentials || host_end < end))
            return Err(error(UrlErrorKind::MissingHost(), host_begin));
        record_.host = Some(rstd_try(host(slice(host_begin, host_end), host_begin)));
        if (host_end < end) {
            u32 value {};
            for (auto i = host_end + usize(1); i < end; ++i) {
                if (! rstd::ascii::is_digit(at(i)))
                    return Err(error(UrlErrorKind::InvalidPort(), i));
                value = value * u32(10) + u32(at(i).to_primitive() - '0');
                if (value > u32(65535)) return Err(error(UrlErrorKind::InvalidPort(), i));
            }
            auto port = u16(value.to_primitive());
            if (host_end + usize(1) != end &&
                url_default_port(record_.scheme.as_str()) != Some(port))
                record_.port = Some(port);
        }
        return Ok(empty {});
    }

    void tail(usize pos) {
        if (pos < size() && at(pos) == u8('?')) {
            auto begin = ++pos;
            while (pos < size() && at(pos) != u8('#')) ++pos;
            record_.query = Some(url_encode(
                slice(begin, pos), special() ? UrlEncode::SpecialQuery : UrlEncode::Query));
        }
        if (pos < size() && at(pos) == u8('#'))
            record_.fragment = Some(url_encode(slice(pos + usize(1), size()), UrlEncode::Fragment));
    }

    static auto drive_letter(ref<str> text) -> bool {
        return text.size() == usize(2) && rstd::ascii::is_alpha(text[usize()]) &&
               (text[usize(1)] == u8(':') || text[usize(1)] == u8('|'));
    }
    auto starts_drive(usize pos) const -> bool {
        if (pos + usize(2) > size() || ! rstd::ascii::is_alpha(at(pos)) ||
            (at(pos + usize(1)) != u8(':') && at(pos + usize(1)) != u8('|')))
            return false;
        return pos + usize(2) == size() || slash(pos + usize(2)) || at(pos + usize(2)) == u8('?') ||
               at(pos + usize(2)) == u8('#');
    }
    void shorten() {
        if (file() && record_.segments.len() == usize(1) &&
            drive_letter(record_.segments[usize()].as_str()))
            return;
        record_.segments.pop();
    }
    void path(usize pos, bool consume_slash = true) {
        if (pos == size() || at(pos) == u8('?') || at(pos) == u8('#')) {
            if (special() && (record_.segments.is_empty() || ! consume_slash))
                record_.segments.push(String());
            tail(pos);
            return;
        }
        if (consume_slash && slash(pos)) ++pos;
        while (true) {
            auto begin = pos;
            while (pos < size() && ! slash(pos) && at(pos) != u8('?') && at(pos) != u8('#')) ++pos;
            auto segment = url_encode(slice(begin, pos), UrlEncode::Path);
            auto lower   = segment.clone();
            lower.as_mut_str().make_ascii_lowercase();
            bool one = lower == "."_str || lower == "%2e"_str;
            bool two = lower == ".."_str || lower == ".%2e"_str || lower == "%2e."_str ||
                       lower == "%2e%2e"_str;
            if (two) shorten();
            if (! one && ! two) {
                if (file() && record_.segments.is_empty() && drive_letter(segment.as_str())) {
                    segment.truncate(usize(1));
                    segment.push_ascii(u8(':'));
                }
                record_.segments.push(rstd::move(segment));
            } else if (! slash(pos))
                record_.segments.push(String());
            if (! slash(pos)) break;
            ++pos;
        }
        tail(pos);
    }

    auto file_url(usize pos, const Url::Record* base) -> Result<empty, UrlError> {
        record_.scheme = String::make("file"_str);
        record_.host   = Some(Url::Host {});
        if (base && base->scheme != "file"_str) base = nullptr;
        if (slash(pos)) {
            ++pos;
            if (slash(pos)) {
                auto begin = ++pos;
                while (pos < size() && ! slash(pos) && at(pos) != u8('?') && at(pos) != u8('#'))
                    ++pos;
                auto text = slice(begin, pos);
                if (drive_letter(text)) {
                    path(begin, false);
                    return Ok(empty {});
                }
                if (! text.is_empty()) {
                    auto parsed = rstd_try(host(text, begin));
                    if (parsed.kind == Url::Host::Kind::Domain && parsed.text == "localhost"_str)
                        parsed.text.clear();
                    record_.host = Some(rstd::move(parsed));
                }
                path(pos);
            } else {
                if (base) {
                    record_.host = Some(base->host->clone());
                    if (! starts_drive(pos) && ! base->segments.is_empty() &&
                        drive_letter(base->segments[usize()].as_str()))
                        record_.segments.push(base->segments[usize()].clone());
                }
                path(pos, false);
            }
        } else if (base) {
            record_          = base->clone();
            record_.fragment = None();
            if (pos == size() || at(pos) == u8('?') || at(pos) == u8('#'))
                tail(pos);
            else {
                record_.query = None();
                if (starts_drive(pos))
                    record_.segments.clear();
                else
                    shorten();
                path(pos, false);
            }
        } else
            path(pos, false);
        return Ok(empty {});
    }

    auto relative(usize pos, const Url::Record& base) -> Result<empty, UrlError> {
        record_          = base.clone();
        record_.fragment = None();
        if (base.opaque) {
            if (pos == size() || at(pos) != u8('#'))
                return Err(error(UrlErrorKind::InvalidSyntax(), pos));
            tail(pos);
            return Ok(empty {});
        }
        if (slash(pos)) {
            record_.query = None();
            record_.segments.clear();
            if (slash(pos + usize(1))) {
                pos += usize(2);
                if (special())
                    while (slash(pos)) ++pos;
                rstd_try(authority(pos));
            }
            path(pos);
        } else if (pos == size() || at(pos) == u8('?') || at(pos) == u8('#')) {
            tail(pos);
        } else {
            record_.query = None();
            record_.segments.pop();
            path(pos);
        }
        return Ok(empty {});
    }

public:
    explicit UrlParser(ref<str> input) {
        usize begin {}, end = input.size();
        while (begin < end && input[begin] <= u8(32)) ++begin;
        while (end > begin && input[end - usize(1)] <= u8(32)) --end;
        end_       = end;
        auto chunk = begin;
        for (auto i = begin; i < end; ++i) {
            auto c = input[i];
            if (c == u8('\t') || c == u8('\r') || c == u8('\n')) {
                input_.push_str(*input.get(chunk, i));
                chunk = i + usize(1);
            } else
                offsets_.push(usize(i));
        }
        input_.push_str(*input.get(chunk, end));
    }

    auto parse(const Url* base) -> Result<Url, UrlError> {
        usize pos {};
        if (size() > usize() && rstd::ascii::is_alpha(at(pos))) {
            ++pos;
            while (pos < size() && (rstd::ascii::is_alnum(at(pos)) || at(pos) == u8('+') ||
                                    at(pos) == u8('-') || at(pos) == u8('.')))
                ++pos;
        }
        if (pos == usize() || pos == size() || at(pos) != u8(':')) {
            if (! base || base->record_.scheme.is_empty())
                return Err(error(UrlErrorKind::MissingScheme(), usize()));
            if (base->record_.scheme == "file"_str)
                rstd_try(file_url(usize(), &base->record_));
            else
                rstd_try(relative(usize(), base->record_));
            return Ok(Url(rstd::move(record_)));
        }
        record_.scheme = String::make(slice(usize(), pos));
        record_.scheme.as_mut_str().make_ascii_lowercase();
        ++pos;
        if (file()) {
            rstd_try(file_url(pos, base ? &base->record_ : nullptr));
        } else if (special()) {
            if (base && base->record_.scheme == record_.scheme &&
                ! (pos + usize(1) < size() && at(pos) == u8('/') &&
                   at(pos + usize(1)) == u8('/'))) {
                rstd_try(relative(pos, base->record_));
            } else {
                while (slash(pos)) ++pos;
                rstd_try(authority(pos));
                path(pos);
            }
        } else if (slash(pos)) {
            if (slash(pos + usize(1))) {
                pos += usize(2);
                rstd_try(authority(pos));
            }
            path(pos);
        } else {
            record_.opaque = true;
            auto begin     = pos;
            while (pos < size() && at(pos) != u8('?') && at(pos) != u8('#')) ++pos;
            record_.opaque_path = url_encode(slice(begin, pos), UrlEncode::C0);
            if (pos < size() && record_.opaque_path.as_str().ends_with(" "_str)) {
                record_.opaque_path.truncate(record_.opaque_path.size() - usize(1));
                record_.opaque_path.push_str("%20"_str);
            }
            tail(pos);
        }
        return Ok(Url(rstd::move(record_)));
    }
};

Url::Url(Record record): record_(rstd::move(record)) {
    serialize();
}

void Url::serialize() {
    auto append = [&](Component& part, ref<str> value) {
        part = { .offset = source_.size(), .size = value.size(), .present = true };
        source_.push_str(value);
    };
    append(scheme_, record_.scheme.as_str());
    source_.push_ascii(u8(':'));
    if (record_.host.is_some()) {
        source_.push_str("//"_str);
        authority_ = { .offset = source_.size(), .present = true };
        if (! record_.username.is_empty() || ! record_.password.is_empty()) {
            auto info = record_.username.clone();
            if (! record_.password.is_empty()) {
                info.push_ascii(u8(':'));
                info.push_str(record_.password.as_str());
            }
            append(userinfo_, info.as_str());
            source_.push_ascii(u8('@'));
        }
        append(host_, record_.host->serialize().as_str());
        if (record_.port.is_some()) {
            source_.push_ascii(u8(':'));
            append(port_, rstd::format("{}", *record_.port).as_str());
        }
        authority_.size = source_.size() - authority_.offset;
    } else if (! record_.opaque && record_.segments.len() > usize(1) &&
               record_.segments[usize()].is_empty()) {
        source_.push_str("/."_str);
    }
    path_ = { .offset = source_.size(), .present = true };
    if (record_.opaque)
        source_.push_str(record_.opaque_path.as_str());
    else
        for (const auto& segment : record_.segments) {
            source_.push_ascii(u8('/'));
            source_.push_str(segment.as_str());
        }
    path_.size = source_.size() - path_.offset;
    if (record_.query.is_some()) {
        source_.push_ascii(u8('?'));
        append(query_, record_.query->as_str());
    }
    if (record_.fragment.is_some()) {
        source_.push_ascii(u8('#'));
        append(fragment_, record_.fragment->as_str());
    }
}

auto Url::parse(ref<str> input) -> Result<Url, UrlError> {
    return UrlParser(input).parse(nullptr);
}
auto Url::parse(ref<str> input, const Url& base) -> Result<Url, UrlError> {
    return UrlParser(input).parse(&base);
}
auto Url::resolve(ref<str> input) const -> Result<Url, UrlError> {
    return parse(input, *this);
}
auto Url::parse_http(ref<str> input) -> Result<Url, UrlError> {
    auto url = rstd_try(parse(input));
    if (url.record_.scheme != "http"_str && url.record_.scheme != "https"_str)
        return Err(UrlError(UrlErrorKind::UnsupportedScheme(), usize()));
    return Ok(rstd::move(url));
}

auto Url::as_ref() const noexcept -> ref<str> {
    return source_.as_str();
}
auto Url::component(Component value) const noexcept -> Option<ref<str>> {
    if (! value.present) return None();
    return source_.as_str().get(value.offset, value.offset + value.size);
}
auto Url::scheme() const noexcept -> Option<ref<str>> {
    return component(scheme_);
}
auto Url::authority() const noexcept -> Option<ref<str>> {
    return component(authority_);
}
auto Url::userinfo() const noexcept -> Option<ref<str>> {
    return component(userinfo_);
}
auto Url::username() const noexcept -> ref<str> {
    return record_.username.as_str();
}
auto Url::password() const noexcept -> ref<str> {
    return record_.password.as_str();
}
auto Url::host() const noexcept -> Option<ref<str>> {
    return component(host_);
}
auto Url::port() const noexcept -> Option<ref<str>> {
    return component(port_);
}
auto Url::effective_port() const -> Option<u16> {
    return record_.port.is_some() ? record_.port : url_default_port(record_.scheme.as_str());
}
auto Url::path() const noexcept -> ref<str> {
    return *component(path_);
}
auto Url::query() const noexcept -> Option<ref<str>> {
    return component(query_);
}
auto Url::fragment() const noexcept -> Option<ref<str>> {
    return component(fragment_);
}
auto Url::request_target() const -> String {
    auto target = String::make(path().is_empty() && record_.host.is_some() ? "/"_str : path());
    if (record_.query.is_some()) {
        target.push_ascii(u8('?'));
        target.push_str(record_.query->as_str());
    }
    return target;
}
auto Url::clone() const -> Url {
    if (record_.scheme.is_empty()) return Url();
    return Url(record_.clone());
}
auto HttpOrigin::authority() const -> String {
    if (url_default_port(scheme.as_str()) == Some(port)) return host.clone();
    return rstd::format("{}:{}", host, port);
}
auto HttpOrigin::serialize() const -> String {
    return rstd::format("{}://{}", scheme, authority());
}
auto Url::http_origin() const -> Option<HttpOrigin> {
    if (record_.host.is_none() || (record_.scheme != "http"_str && record_.scheme != "https"_str))
        return None();
    return Some(
        HttpOrigin { record_.scheme.clone(), record_.host->serialize(), *effective_port() });
}
auto Url::same_http_origin(const Url& other) const -> bool {
    auto left = http_origin(), right = other.http_origin();
    return left.is_some() && right.is_some() && left->scheme == right->scheme &&
           left->host == right->host && left->port == right->port;
}

UrlOrigin::UrlOrigin(): identity_(Some(Arc<OpaqueIdentity>::make())) {
}
UrlOrigin::UrlOrigin(Tuple tuple): tuple_(Some(rstd::move(tuple))) {
}
UrlOrigin::UrlOrigin(Arc<OpaqueIdentity> identity): identity_(Some(rstd::move(identity))) {
}
auto UrlOrigin::is_opaque() const -> bool {
    return tuple_.is_none();
}
auto UrlOrigin::tuple() const -> Option<ref<Tuple>> {
    if (tuple_.is_none()) return None();
    return Some(ref<Tuple>::from_raw_parts(&*tuple_));
}
auto UrlOrigin::serialize() const -> String {
    if (is_opaque()) return String::make("null"_str);
    auto value = rstd::format("{}://{}", tuple_->scheme, tuple_->host);
    if (tuple_->port.is_some()) value.push_str(rstd::format(":{}", *tuple_->port).as_str());
    return value;
}
auto UrlOrigin::same_origin(const UrlOrigin& other) const -> bool {
    if (is_opaque() != other.is_opaque()) return false;
    if (is_opaque()) return Arc<OpaqueIdentity>::ptr_eq(*identity_, *other.identity_);
    return tuple_->scheme == other.tuple_->scheme && tuple_->host == other.tuple_->host &&
           tuple_->port == other.tuple_->port;
}
auto UrlOrigin::clone() const -> UrlOrigin {
    if (is_opaque()) return UrlOrigin(identity_->clone());
    return UrlOrigin(Tuple { tuple_->scheme.clone(), tuple_->host.clone(), tuple_->port });
}
auto Url::origin() const -> Result<UrlOrigin, UrlError> {
    if (record_.host.is_some() && url_default_port(record_.scheme.as_str()).is_some())
        return Ok(UrlOrigin(
            UrlOrigin::Tuple { record_.scheme.clone(), record_.host->serialize(), record_.port }));
    if (record_.scheme == "blob"_str) {
        auto embedded = Url::parse(path());
        if (embedded.is_err() && embedded.unwrap_err().kind().is_UnsupportedHost())
            return Err(rstd::move(embedded).unwrap_err());
        if (embedded.is_ok() &&
            (embedded->record_.scheme == "http"_str || embedded->record_.scheme == "https"_str))
            return embedded->origin();
    }
    return Ok(UrlOrigin());
}

} // namespace lihttpto
