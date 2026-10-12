export module lihttpto:conditions;
export import :validator;
export import :date;
export import :message;
using namespace rstd::prelude;

export namespace lihttpto
{
enum class RepresentationKind
{
    Full,
    Partial,
    NotModified,
    PreconditionFailed,
    Unsatisfiable
};
struct ByteRange {
    u64 offset {}, length {};
};
struct RepresentationDecision {
    RepresentationKind kind { RepresentationKind::Full };
    ByteRange          range;
    auto               status() const -> StatusCode;
    auto               content_range(u64 total_length) const -> Option<String>;
};
struct RepresentationMetadata {
    u64               length {};
    Option<EntityTag> etag;
    Option<i64>       last_modified;
    bool              exists { true };
    bool              accept_ranges { true };
    bool              last_modified_strong {};
};
class RequestConditions {
    Option<EntityTagCondition> if_match_, if_none_match_;
    Option<i64>                if_unmodified_since_, if_modified_since_, if_range_date_;
    Option<EntityTag>          if_range_tag_;
    bool                       if_range_present_ {}, range_valid_ {}, suffix_ {};
    u64                        first_ {};
    Option<u64>                last_;

public:
    static auto parse(const Headers& headers) -> Result<RequestConditions, ConditionError>;
    // Evaluate after authorization, resource selection and other normal request checks.
    auto evaluate(ref<str> method, const RepresentationMetadata& representation) const
        -> RepresentationDecision;
};
} // namespace lihttpto
