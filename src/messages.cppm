export module lihttpto:messages;
export import :message;
export import :url;
using namespace rstd::prelude;
export namespace lihttpto
{
struct RequestParts {
    Url     url;
    Method  method;
    Headers headers;
    auto clone() const -> RequestParts { return { url.clone(), method.clone(), headers.clone() }; }
};
template<class Body>
struct RequestMessage {
    RequestParts parts;
    Body         body;
};
template<class Body>
struct ResponseMessage {
    ResponseHead head;
    Body         body;
};
} // namespace lihttpto
