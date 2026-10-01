export module lihttpto.parser.ascii;
export import rstd.core;

export namespace lihttpto::parser::ascii
{

using rstd::u8;

constexpr auto unreserved(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return rstd::ascii::is_alnum(value) || raw == '-' || raw == '.' || raw == '_' || raw == '~';
}

constexpr auto sub_delim(u8 value) noexcept -> bool {
    switch (value.to_primitive()) {
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=': return true;
    default: return false;
    }
}

constexpr auto tchar(u8 value) noexcept -> bool {
    auto raw = value.to_primitive();
    return rstd::ascii::is_alnum(value) || raw == '!' || raw == '#' || raw == '$' || raw == '%' ||
           raw == '&' || raw == '\'' || raw == '*' || raw == '+' || raw == '-' || raw == '.' ||
           raw == '^' || raw == '_' || raw == '`' || raw == '|' || raw == '~';
}

} // namespace lihttpto::parser::ascii
