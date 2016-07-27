#include "convertEnums.h"
#include "frontends/p4/enumInstance.h"

namespace P4 {

const IR::Node* ConvertEnums::preorder(IR::Type_Enum* type) {
    bool convert = policy->convert(type);
    if (!convert)
        return type;
    unsigned count = type->members->size();
    unsigned width = policy->enumSize(count);
    BUG_CHECK(count >= (1U << width),
              "%1%: not enough bits to represent %2%", width, type);
    auto r = new EnumRepresentation(type->srcInfo, width);
    auto canontype = typeMap->getType(getOriginal(), true);
    repr.emplace(canontype->to<IR::Type_Enum>(), r);
    for (auto d : *type->members)
        r->add(d->name.name);
    return nullptr;  // delete the declaration
}

const IR::Node* ConvertEnums::postorder(IR::Type_Name* type) {
    auto canontype = typeMap->getType(getOriginal(), true);
    if (!canontype->is<IR::Type_Enum>())
        return type;
    if (findContext<IR::TypeNameExpression>() != nullptr)
        // This will be resolved by the caller.
        return type;
    auto enumType = canontype->to<IR::Type_Enum>();
    auto r = ::get(repr, enumType);
    if (r == nullptr)
        return type;
    return r->type;
}

const IR::Node* ConvertEnums::postorder(IR::Member* expression) {
    auto ei = EnumInstance::resolve(getOriginal<IR::Member>(), typeMap);
    if (ei == nullptr)
        return expression;
    auto r = ::get(repr, ei->type);
    if (r == nullptr)
        return expression;
    unsigned value = r->get(ei->name.name);
    auto cst = new IR::Constant(expression->srcInfo, r->type, value);
    return cst;
}

}  // namespace P4