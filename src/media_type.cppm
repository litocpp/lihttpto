export module lihttpto:media_type;
export import :header;

using namespace rstd::prelude;
using namespace rstd::literals;
using alloc::vec::Vec;
using alloc::string::String;
using alloc::collections::BTreeMap;

export namespace lihttpto
{
struct MediaParameter {
    ref<str> name;
    Vec<u8>  value;
};
// Type and parameter names borrow the input; quoted parameter values are decoded.
class MediaType {
    ref<str>            type_;
    ref<str>            subtype_;
    Vec<MediaParameter> parameters_;
    MediaType(ref<str> type, ref<str> subtype, Vec<MediaParameter> parameters)
        : type_(type), subtype_(subtype), parameters_(rstd::move(parameters)) {}

public:
    static auto parse(ref<str> value) -> Result<MediaType, HeaderError> {
        usize pos {};
        auto  ows = [&] {
            while (pos < value.size() && (value[pos] == u8(' ') || value[pos] == u8('\t'))) ++pos;
        };
        auto token = [&] {
            auto begin = pos;
            while (pos < value.size() && token_byte(value[pos])) ++pos;
            return value.get(begin, pos).unwrap();
        };
        auto invalid = [&] {
            return Err(HeaderError::InvalidValue().at(pos));
        };
        ows();
        auto type = token();
        if (type.is_empty() || pos == value.size() || value[pos++] != u8('/')) return invalid();
        auto subtype = token();
        if (subtype.is_empty() || type.contains("*"_str) || subtype.contains("*"_str))
            return invalid();
        ows();
        Vec<MediaParameter>    parameters;
        BTreeMap<String, bool> names;
        while (pos < value.size()) {
            if (value[pos++] != u8(';')) return invalid();
            ows();
            auto name = token();
            if (name.is_empty() || pos == value.size() || value[pos++] != u8('=')) return invalid();
            String normalized;
            for (auto byte : name.as_bytes()) normalized.push_ascii(ascii_lower(byte));
            if (names.contains_key(normalized.as_str())) return invalid();
            names.insert(rstd::move(normalized), true);
            if (pos == value.size()) return invalid();
            Vec<u8> decoded;
            if (value[pos] == u8('"')) {
                ++pos;
                bool closed = false;
                while (pos < value.size()) {
                    auto byte = value[pos++];
                    if (byte == u8('"')) {
                        closed = true;
                        break;
                    }
                    if (byte == u8('\\')) {
                        if (pos == value.size()) return invalid();
                        byte = value[pos++];
                    }
                    if (byte != u8('\t') && (byte < u8(32) || byte == u8(127))) return invalid();
                    decoded.push(u8(byte));
                }
                if (! closed) return invalid();
            } else {
                auto text = token();
                if (text.is_empty()) return invalid();
                for (auto byte : text.as_bytes()) decoded.push(u8(byte));
            }
            parameters.push({ name, rstd::move(decoded) });
            ows();
        }
        return Ok(MediaType(type, subtype, rstd::move(parameters)));
    }
    auto type() const -> ref<str> { return type_; }
    auto subtype() const -> ref<str> { return subtype_; }
    auto parameter(ref<str> name) const -> Option<slice<u8>> {
        for (const auto& parameter : parameters_)
            if (ascii_equal(parameter.name.as_bytes(), name))
                return Some(parameter.value.as_slice());
        return None<slice<u8>>();
    }
    auto matches(ref<str> type, ref<str> subtype) const -> bool {
        return ascii_equal(type_.as_bytes(), type) && ascii_equal(subtype_.as_bytes(), subtype);
    }
    auto parameter_eq_ignore_ascii_case(ref<str> name, ref<str> expected) const -> bool {
        auto value = parameter(name);
        return value.is_some() && ascii_equal(*value, expected);
    }
};
} // namespace lihttpto
