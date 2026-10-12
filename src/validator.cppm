export module lihttpto:validator;
export import :header;
export import rstd;
using namespace rstd::prelude;

export namespace lihttpto
{
enum class ConditionError
{
    InvalidEntityTag
};

class EntityTag {
    Vec<u8> wire_;
    bool    weak_ {};
    EntityTag(Vec<u8> wire, bool weak): wire_(rstd::move(wire)), weak_(weak) {}

public:
    static auto parse(slice<u8> bytes) -> Result<EntityTag, ConditionError>;
    auto        as_bytes() const -> slice<u8> { return wire_.as_slice(); }
    auto        is_weak() const -> bool { return weak_; }
    auto        clone() const -> EntityTag { return { wire_.clone(), weak_ }; }
    auto        weak_eq(const EntityTag& other) const -> bool;
    auto        strong_eq(const EntityTag& other) const -> bool {
        return ! weak_ && ! other.weak_ && weak_eq(other);
    }
};

class EntityTagCondition {
    bool           any_ {};
    Vec<EntityTag> tags_;

public:
    static auto parse(const Headers& headers, ref<str> name)
        -> Result<Option<EntityTagCondition>, ConditionError>;
    auto matches(bool exists, const Option<EntityTag>& current, bool strong) const -> bool;
};
auto entity_tag_matches(ref<str> condition, ref<str> current) -> bool;
} // namespace lihttpto
