module;
#include <rstd/enum.hpp>
#include <rstd/macro.hpp>
export module lihttpto:hpack;
export import :messages;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::bytes::Bytes;
export namespace lihttpto::h2
{
class Error {
    RSTD_ENUM(Error,
              (Connection, (u32 code;)),
              (Stream, (u32 id; u32 code;)),
              (Compression),
              (TooLarge))
public:
    auto clone() const -> Error {
        if (is_Connection()) return Connection(u32(as_Connection().code));
        if (is_Stream()) return Stream(u32(as_Stream().id), u32(as_Stream().code));
        if (is_Compression()) return Compression();
        return TooLarge();
    }
};
template<class T>
using Result = rstd::Result<T, Error>;
struct Field {
    String name;
    Bytes  value;
    bool   sensitive {};
    auto   clone() const -> Field { return { name.clone(), value.clone(), sensitive }; }
};
} // namespace lihttpto::h2
export namespace rstd
{
template<>
struct Impl<fmt::Display, lihttpto::h2::Error> : ImplBase<lihttpto::h2::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& e = this->self();
        if (e.is_Connection())
            return f.write_fmt(
                fmt::Arguments::make("HTTP/2 connection error {}", e.as_Connection().code));
        if (e.is_Stream())
            return f.write_fmt(fmt::Arguments::make(
                "HTTP/2 stream {} error {}", e.as_Stream().id, e.as_Stream().code));
        return f.write_str(e.is_Compression() ? "invalid HPACK block"_str
                                              : "HTTP/2 limit exceeded"_str);
    }
};
template<>
struct Impl<fmt::Debug, lihttpto::h2::Error> : Impl<fmt::Display, lihttpto::h2::Error> {};
template<>
struct Impl<error::Error, lihttpto::h2::Error> : DefaultInImpl<error::Error, lihttpto::h2::Error> {
};
} // namespace rstd
namespace lihttpto::h2
{
#include "hpack_huffman.inc"
struct StaticField {
    ref<str> name, value;
};
static const StaticField static_fields[] { { ":authority"_str, ""_str },
                                           { ":method"_str, "GET"_str },
                                           { ":method"_str, "POST"_str },
                                           { ":path"_str, "/"_str },
                                           { ":path"_str, "/index.html"_str },
                                           { ":scheme"_str, "http"_str },
                                           { ":scheme"_str, "https"_str },
                                           { ":status"_str, "200"_str },
                                           { ":status"_str, "204"_str },
                                           { ":status"_str, "206"_str },
                                           { ":status"_str, "304"_str },
                                           { ":status"_str, "400"_str },
                                           { ":status"_str, "404"_str },
                                           { ":status"_str, "500"_str },
                                           { "accept-charset"_str, ""_str },
                                           { "accept-encoding"_str, "gzip, deflate"_str },
                                           { "accept-language"_str, ""_str },
                                           { "accept-ranges"_str, ""_str },
                                           { "accept"_str, ""_str },
                                           { "access-control-allow-origin"_str, ""_str },
                                           { "age"_str, ""_str },
                                           { "allow"_str, ""_str },
                                           { "authorization"_str, ""_str },
                                           { "cache-control"_str, ""_str },
                                           { "content-disposition"_str, ""_str },
                                           { "content-encoding"_str, ""_str },
                                           { "content-language"_str, ""_str },
                                           { "content-length"_str, ""_str },
                                           { "content-location"_str, ""_str },
                                           { "content-range"_str, ""_str },
                                           { "content-type"_str, ""_str },
                                           { "cookie"_str, ""_str },
                                           { "date"_str, ""_str },
                                           { "etag"_str, ""_str },
                                           { "expect"_str, ""_str },
                                           { "expires"_str, ""_str },
                                           { "from"_str, ""_str },
                                           { "host"_str, ""_str },
                                           { "if-match"_str, ""_str },
                                           { "if-modified-since"_str, ""_str },
                                           { "if-none-match"_str, ""_str },
                                           { "if-range"_str, ""_str },
                                           { "if-unmodified-since"_str, ""_str },
                                           { "last-modified"_str, ""_str },
                                           { "link"_str, ""_str },
                                           { "location"_str, ""_str },
                                           { "max-forwards"_str, ""_str },
                                           { "proxy-authenticate"_str, ""_str },
                                           { "proxy-authorization"_str, ""_str },
                                           { "range"_str, ""_str },
                                           { "referer"_str, ""_str },
                                           { "refresh"_str, ""_str },
                                           { "retry-after"_str, ""_str },
                                           { "server"_str, ""_str },
                                           { "set-cookie"_str, ""_str },
                                           { "strict-transport-security"_str, ""_str },
                                           { "transfer-encoding"_str, ""_str },
                                           { "user-agent"_str, ""_str },
                                           { "vary"_str, ""_str },
                                           { "via"_str, ""_str },
                                           { "www-authenticate"_str, ""_str } };
auto                     hpack_integer(slice<u8> bytes, usize& at, unsigned prefix) -> Result<u64> {
    if (at >= bytes.len()) return Err(Error::Compression());
    auto               mask = (1u << prefix) - 1;
    unsigned long long n    = bytes[at++].to_primitive() & mask;
    if (n != mask) return Ok(u64(n));
    unsigned shift = 0;
    for (;;) {
        if (at >= bytes.len() || shift >= 63) return Err(Error::Compression());
        unsigned           byte = bytes[at++].to_primitive();
        unsigned long long tail = byte & 127;
        if (tail > ((~0ull - n) >> shift)) return Err(Error::Compression());
        n += tail << shift;
        if (! (byte & 128)) return Ok(u64(n));
        shift += 7;
    }
}
void hpack_integer(Vec<u8>& out, unsigned long long n, unsigned prefix, unsigned tag) {
    unsigned mask = (1u << prefix) - 1;
    if (n < mask) {
        out.push(u8(tag | static_cast<unsigned>(n)));
        return;
    }
    out.push(u8(tag | mask));
    n -= mask;
    while (n >= 128) {
        out.push(u8((n & 127) | 128));
        n >>= 7;
    }
    out.push(u8(n));
}
auto hpack_string(slice<u8> bytes, usize& at, usize limit) -> Result<Bytes> {
    if (at >= bytes.len()) return Err(Error::Compression());
    bool huffman = (bytes[at] & u8(128)) != u8();
    auto count   = rstd_try(hpack_integer(bytes, at, 7));
    if (count > u64((bytes.len() - at).to_primitive())) return Err(Error::Compression());
    auto    end = at + usize(count.to_primitive());
    Vec<u8> output;
    if (! huffman) {
        if (count > u64(limit.to_primitive())) return Err(Error::TooLarge());
        while (at < end) output.push(u8(bytes[at++]));
    } else {
        int      node     = 0;
        unsigned trailing = 0, pattern = 0;
        while (at < end) {
            auto byte = bytes[at++].to_primitive();
            for (int bit = 7; bit >= 0; --bit) {
                unsigned value = (byte >> bit) & 1;
                node           = huffman_nodes[node][value];
                if (node < 0) return Err(Error::Compression());
                ++trailing;
                pattern    = (pattern << 1) | value;
                int symbol = huffman_nodes[node][2];
                if (symbol >= 0) {
                    if (symbol == 256) return Err(Error::Compression());
                    if (output.len() == limit) return Err(Error::TooLarge());
                    output.push(u8(symbol));
                    node     = 0;
                    trailing = pattern = 0;
                }
            }
        }
        if (trailing > 7 || (trailing && pattern != ((1u << trailing) - 1)))
            return Err(Error::Compression());
    }
    return Ok(Bytes::from_vec(rstd::move(output)));
}
void hpack_string(Vec<u8>& out, slice<u8> value) {
    hpack_integer(out, value.len().to_primitive(), 7, 0);
    out.extend_from_slice(value);
}
export class HpackDecoder {
    Vec<Field> dynamic_;
    usize      bytes_ {}, capacity_ { 4096 }, maximum_ { 4096 };
    void       resize(usize size) {
        capacity_ = size;
        while (bytes_ > capacity_) {
            auto old = dynamic_.remove(usize());
            bytes_ -= old.name.len() + old.value.len() + usize(32);
        }
    }
    auto indexed(u64 index) const -> Result<Field> {
        if (index == u64()) return Err(Error::Compression());
        if (index <= u64(61)) {
            const auto& row = static_fields[index.to_primitive() - 1];
            return Ok(Field {
                String::make(row.name), Bytes::copy_from_slice(row.value.as_bytes()), false });
        }
        auto n = index - u64(62);
        if (n >= u64(dynamic_.len().to_primitive())) return Err(Error::Compression());
        return Ok(dynamic_[dynamic_.len() - usize(1) - usize(n.to_primitive())].clone());
    }

public:
    auto decode(Bytes block, usize limit = usize(65536)) -> Result<Vec<Field>> {
        Vec<Field> fields;
        usize      at, total;
        bool       started = false;
        auto       bytes   = block.as_slice();
        while (at < bytes.len()) {
            unsigned tag = bytes[at].to_primitive();
            Field    field;
            if (tag & 128) {
                auto index = rstd_try(hpack_integer(bytes, at, 7));
                field      = rstd_try(indexed(index));
            } else if ((tag & 224) == 32) {
                if (started) return Err(Error::Compression());
                auto size = rstd_try(hpack_integer(bytes, at, 5));
                if (size > u64(maximum_.to_primitive())) return Err(Error::Compression());
                resize(usize(size.to_primitive()));
                continue;
            } else {
                bool insert = (tag & 192) == 64;
                auto index  = rstd_try(hpack_integer(bytes, at, insert ? 6 : 4));
                if (index != u64())
                    field.name = rstd_try(indexed(index)).name;
                else {
                    auto name = rstd_try(hpack_string(bytes, at, limit));
                    auto text = rstd::str_::from_utf8(name.as_slice());
                    if (text.is_err()) return Err(Error::Compression());
                    field.name = String::make(*text);
                }
                field.value     = rstd_try(hpack_string(bytes, at, limit));
                field.sensitive = (tag & 240) == 16;
                if (insert) {
                    auto size = field.name.len() + field.value.len() + usize(32);
                    if (size > capacity_) {
                        dynamic_.clear();
                        bytes_ = usize();
                    } else {
                        while (bytes_ > capacity_ - size) {
                            auto old = dynamic_.remove(usize());
                            bytes_ -= old.name.len() + old.value.len() + usize(32);
                        }
                        dynamic_.push(field.clone());
                        bytes_ += size;
                    }
                }
            }
            started   = true;
            auto size = field.name.len() + field.value.len() + usize(32);
            if (size > limit - total) return Err(Error::TooLarge());
            total += size;
            fields.push(rstd::move(field));
        }
        return Ok(rstd::move(fields));
    }
};
export auto encode_fields(slice<Field> fields, usize limit = usize(65536)) -> Result<Bytes> {
    Vec<u8> out;
    usize   size;
    for (const auto& field : fields) {
        auto count = field.name.len() + field.value.len() + usize(32);
        if (count > limit - size) return Err(Error::TooLarge());
        size += count;
        unsigned name = 0, exact = 0;
        for (unsigned i = 0; i < 61; ++i) {
            if (field.name != static_fields[i].name) continue;
            if (! name) name = i + 1;
            if (field.value.as_slice() == static_fields[i].value.as_bytes()) exact = i + 1;
        }
        if (exact && ! field.sensitive)
            hpack_integer(out, exact, 7, 128);
        else {
            hpack_integer(out, name, 4, 16);
            if (! name) hpack_string(out, field.name.as_str().as_bytes());
            hpack_string(out, field.value.as_slice());
        }
    }
    return Ok(Bytes::from_vec(rstd::move(out)));
}
} // namespace lihttpto::h2
