module;
#include <rstd/enum.hpp>
export module lihttpto:body_frame;
export import :request_body;

using namespace rstd::prelude;

export namespace lihttpto
{
class BodyFrame {
    RSTD_ENUM(BodyFrame, (Data, (rstd::bytes::Bytes bytes;)), (Trailers, (Headers headers;)))
};

auto validate_trailers(const Headers& headers) -> Result<empty, DecodeError> {
    for (const auto& field : headers)
        if (forbidden_trailer(field.name.as_str())) return Err(DecodeError::InvalidHeader);
    return Ok(empty {});
}
} // namespace lihttpto
