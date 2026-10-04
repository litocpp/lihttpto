#include <rstd/test/gtest.hpp>
import lihttpto;
import rstd;
import rstd.json;

using namespace rstd::prelude;
using namespace rstd::literals;
using lihttpto::Url;
using rstd::json::Map;
using rstd::json::Value;
using rstd::path::PathBuf;

void observed_text(Map& output, ref<str> key, ref<str> value) {
    output.insert(String::make(key), Value::String(String::make(value)));
}

auto parse_observed_url(const Value& input) -> Result<Url, lihttpto::UrlError> {
    auto reference = *input.get("input"_str)->get().as_str();
    auto base_text = input.get("base"_str)->get().as_str();
    if (base_text.is_none()) return Url::parse(reference);
    auto base = Url::parse(*base_text);
    if (base.is_err()) return base;
    return base->resolve(reference);
}

auto observe_url(const Value& input) -> Value {
    Map  output;
    auto parsed = parse_observed_url(input);
    output.insert(String::make("failure"_str), Value::Bool(parsed.is_err()));
    if (parsed.is_err()) {
        observed_text(output, "error"_str, rstd::format("{}", parsed.unwrap_err()).as_str());
        return Value::Object(rstd::move(output));
    }
    const auto& url = parsed.unwrap();
    observed_text(output, "href"_str, url.as_ref());
    auto scheme = url.scheme();
    observed_text(
        output, "protocol"_str, scheme.is_some() ? rstd::format("{}:", *scheme).as_str() : ""_str);
    auto host = url.host();
    auto port = url.port();
    observed_text(output, "hostname"_str, host.is_some() ? *host : ""_str);
    observed_text(output, "port"_str, port.is_some() ? *port : ""_str);
    auto host_text = host.is_some() ? String::make(*host) : String();
    if (port.is_some()) {
        host_text.push_str(":"_str);
        host_text.push_str(*port);
    }
    observed_text(output, "host"_str, host_text.as_str());
    observed_text(output, "username"_str, url.username());
    observed_text(output, "password"_str, url.password());
    observed_text(output, "pathname"_str, url.path());
    auto query    = url.query();
    auto fragment = url.fragment();
    observed_text(output,
                  "search"_str,
                  query.is_some() && ! query->is_empty() ? rstd::format("?{}", *query).as_str()
                                                         : ""_str);
    observed_text(output,
                  "hash"_str,
                  fragment.is_some() && ! fragment->is_empty()
                      ? rstd::format("#{}", *fragment).as_str()
                      : ""_str);
    auto origin = url.origin();
    if (origin.is_ok())
        observed_text(output, "origin"_str, origin->serialize().as_str());
    else
        observed_text(output, "origin_error"_str, rstd::format("{}", origin.unwrap_err()).as_str());
    return Value::Object(rstd::move(output));
}

TEST(UrlBaseline, ExportPublicApiObservations) {
    auto input_path = rstd::env::var("LIHTTPTO_URL_INPUT"_str);
    if (input_path.is_err()) GTEST_SKIP();
    auto output_path = rstd::env::var("LIHTTPTO_URL_OUTPUT"_str).unwrap();
    auto input = rstd::fs::read_to_string(PathBuf::from(input_path->as_str()).as_path()).unwrap();
    auto value = rstd::json::from_str(input.as_str()).unwrap();
    Vec<Value> observations;
    for (const auto& item : value.as_array()->get()) observations.push(observe_url(item));
    auto encoded = rstd::json::to_string(Value::Array(rstd::move(observations)));
    auto file    = rstd::fs::File::create(PathBuf::from(output_path.as_str()).as_path()).unwrap();
    ASSERT_TRUE(file.write_all(encoded.as_str().as_bytes()).is_ok());
    ASSERT_TRUE(file.flush().is_ok());
}
