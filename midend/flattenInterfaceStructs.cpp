/*
Copyright 2018 VMware, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "flattenInterfaceStructs.h"

namespace P4 {

void StructTypeReplacement::flatten(const P4::TypeMap* typeMap,
                                    cstring prefix,
                                    const IR::Type* type,
                                    IR::IndexedVector<IR::StructField> *fields) {
    if (auto st = type->to<IR::Type_StructLike>()) {
        structFieldMap.emplace(prefix, type);
        for (auto f : st->fields)
            flatten(typeMap, prefix + "." + f->name, f->type, fields);
        return;
    }
    cstring fieldName = prefix.replace(".", "_") +
                        cstring::to_cstring(fieldNameRemap.size());
    fieldNameRemap.emplace(prefix, fieldName);
    fields->push_back(new IR::StructField(IR::ID(fieldName), type));
}

StructTypeReplacement::StructTypeReplacement(
    const P4::TypeMap* typeMap, const IR::Type* type) {
    auto vec = new IR::IndexedVector<IR::StructField>();
    LOG3("StructTypeReplacement:" << type);
    flatten(typeMap, "", type, vec);
    auto tname = type->to<IR::Type_Declaration>()->name;
    if (type->is<IR::Type_Header>()) {
        replacementType = new IR::Type_Header(tname, IR::Annotations::empty, *vec);
    } else if (type->is<IR::Type_Struct>()) {
        replacementType = new IR::Type_Struct(tname, IR::Annotations::empty, *vec);
    } else {
        BUG("Constructor for StructTypeReplacement is not struct/header %1%",
            type);
    }
}

const IR::StructInitializerExpression* StructTypeReplacement::explode(
    const IR::Expression *root, cstring prefix) {
    auto vec = new IR::IndexedVector<IR::NamedExpression>();
    auto fieldType = ::get(structFieldMap, prefix);
    CHECK_NULL(fieldType);
    auto ft = fieldType->to<IR::Type_StructLike>();
    for (auto f : ft->fields) {
        cstring fieldName = prefix + "." + f->name.name;
        auto newFieldname = ::get(fieldNameRemap, fieldName);
        const IR::Expression* expr;
        if (!newFieldname.isNullOrEmpty()) {
            expr = new IR::Member(root, newFieldname);
        } else {
            expr = explode(root, fieldName);
        }
        vec->push_back(new IR::NamedExpression(f->name, expr));
    }
    return new IR::StructInitializerExpression(ft->name, *vec, false);
}

static const IR::Type* isNestedStruct(const P4::TypeMap* typeMap, const IR::Type* type) {
    if (auto st = type->to<IR::Type_Struct>()) {
        for (auto f : st->fields) {
            auto ft = typeMap->getType(f, true);
            if (ft->is<IR::Type_Struct>())
                return type;
        }
    }
    return nullptr;
}

static const IR::Type* isHeaderWithStruct(const P4::TypeMap* typeMap, const IR::Type* type) {
    if (auto st = type->to<IR::Type_Header>()) {
        for (auto f : st->fields) {
            auto ft = typeMap->getType(f, true);
            if (ft->is<IR::Type_Struct>())
                return type;
        }
    }
    return nullptr;
}

void NestedStructMap::createReplacement(const IR::Type* type) {
    auto repl = ::get(replacement, type);
    if (repl != nullptr)
        return;
    repl = new StructTypeReplacement(typeMap, type);
    LOG3("Replacement for " << type << " is " << repl);
    replacement.emplace(type, repl);
}

bool FindTypesToReplace::preorder(const IR::Declaration_Instance* inst) {
    auto type = map->typeMap->getTypeType(inst->type, true);
    auto ts = type->to<IR::Type_SpecializedCanonical>();
    if (ts == nullptr)
        return false;
    if (!ts->baseType->is<IR::Type_Package>())
        return false;
    for (auto t : *ts->arguments) {
        if (auto st = isNestedStruct(map->typeMap, t)) {
            map->createReplacement(st);
        } else if (auto st = t->to<IR::Type_Struct>()) {
            for (auto f : st->fields) {
              auto ft = map->typeMap->getType(f, true);
              if (auto ht = isHeaderWithStruct(map->typeMap, ft))
                  map->createReplacement(ht);
          }
      }
    }
    return false;
}

/////////////////////////////////

const IR::Node* ReplaceStructs::preorder(IR::P4Program* program) {
    if (replacementMap->empty()) {
        // nothing to do
        prune();
    }
    return program;
}

const IR::Node* ReplaceStructs::postorder(IR::Type_Struct* type) {
    auto canon = replacementMap->typeMap->getTypeType(getOriginal(), true);
    auto repl = replacementMap->getReplacement(canon);
    if (repl != nullptr)
        return repl->replacementType;
    return type;
}

const IR::Node* ReplaceStructs::postorder(IR::Member* expression) {
    // Find out if this applies to one of the parameters that are being replaced.
    if (getParent<IR::Member>() != nullptr)
        // We only want to process the outermost Member
        return expression;
    const IR::Expression* e = expression;
    cstring prefix = "";
    while (auto mem = e->to<IR::Member>()) {
        e = mem->expr;
        prefix = cstring(".") + mem->member + prefix;
    }
    auto pe = e->to<IR::PathExpression>();
    if (pe == nullptr)
        return expression;
    // At this point we know that pe is an expression of the form
    // param.field1.etc.fieldN, where param has a type that needs to be replaced.
    auto decl = replacementMap->refMap->getDeclaration(pe->path, true);
    auto param = decl->to<IR::Parameter>();
    if (param == nullptr)
        return expression;
    auto repl = ::get(toReplace, param);
    if (repl == nullptr)
        return expression;
    auto newFieldName = ::get(repl->fieldNameRemap, prefix);
    const IR::Expression* result;
    if (newFieldName.isNullOrEmpty()) {
        // Prefix is a reference to a field of the original struct whose
        // type is actually a struct itself.  We need to replace the field
        // with a struct initializer expression.  (This won't work if the
        // field is being used as a left-value.  Hopefully, all such uses
        // of a struct-valued field as a left-value have been already
        // replaced by the NestedStructs pass.)
        result = repl->explode(pe, prefix);
    } else {
        result = new IR::Member(pe, newFieldName);
    }
    LOG3("Swapping " << expression << " with " << result);
    return result;
}

const IR::Node* ReplaceStructs::preorder(IR::P4Parser* parser) {
    for (auto p : parser->getApplyParameters()->parameters) {
        auto pt = replacementMap->typeMap->getType(p, true);
        auto repl = replacementMap->getReplacement(pt);
        if (repl != nullptr) {
            toReplace.emplace(p, repl);
            LOG3("Replacing parameter " << dbp(p) << " of " << dbp(parser));
        }
    }
    return parser;
}

const IR::Node* ReplaceStructs::preorder(IR::P4Control* control) {
    for (auto p : control->getApplyParameters()->parameters) {
        auto pt = replacementMap->typeMap->getType(p, true);
        auto repl = replacementMap->getReplacement(pt);
        if (repl != nullptr) {
            toReplace.emplace(p, repl);
            LOG3("Replacing parameter " << dbp(p) << " of " << dbp(control));
        }
    }
    return control;
}

/////////////////////////////////

const IR::Node* ReplaceHeaders::preorder(IR::P4Program* program) {
    if (replacementMap->empty()) {
        // nothing to do
        prune();
    }
    return program;
}

const IR::Node* ReplaceHeaders::postorder(IR::Type_Header* type) {
    auto canon = replacementMap->typeMap->getTypeType(getOriginal(), true);
    auto repl = replacementMap->getReplacement(canon);
    if (repl != nullptr)
        return repl->replacementType;
    return type;
}

const IR::Node* ReplaceHeaders::postorder(IR::Member* expression) {
    // Find out if this applies to one of the parameters that are being replaced.
    if (getParent<IR::Member>() != nullptr)
        // We only want to process the outermost Member
        return expression;

    // this is a reference to whole header str.hdr
    if (expression->expr->is<IR::PathExpression>())
        return expression;

    const IR::Expression* e = expression;
    cstring prefix = "";
    // param structs have been flattend, but we can have param.header.struct.field
    while (auto mem = e->to<IR::Member>()) {
        if (mem->expr->is<IR::PathExpression>()) break;
        e = mem->expr;
        prefix = cstring(".") + mem->member + prefix;
    }
    auto repl = ::get(toReplace, e->toString());
    if (repl == nullptr)
        return expression;
    auto newFieldName = ::get(repl->fieldNameRemap, prefix);
    const IR::Expression* result;
    if (newFieldName.isNullOrEmpty()) {
        BUG("cannot find replacement for %s", prefix);
        /*
        std::cout << "expression = " << expression << "\n";
        std::cout << "e = " << e << "\n";
        std::cout << "e->toString = " << e->toString() << "\n";
        std::cout << "repl = " << repl << "\n";
        std::cout << "prefix = " << prefix << "\n";
        std::cout << "newFieldName = " << newFieldName << "\n";
        std::cout << "cannot find replacement for " << prefix << "\n";
        return expression;
        */
        //  result = repl->explode(e, prefix);
    } else {
        result = new IR::Member(e, newFieldName);
    }
    LOG3("Replacing " << expression << " with " << result);
    return result;
}

const IR::Node* ReplaceHeaders::preorder(IR::P4Parser* parser) {
    for (auto p : parser->getApplyParameters()->parameters) {
        auto pt = replacementMap->typeMap->getType(p, true);
        if (auto st = pt->to<IR::Type_Struct>()) {
            for (auto f : st->fields) {
                auto ftype = replacementMap->typeMap->getType(f, true);
                if (ftype->is<IR::Type_Header>()) {
                    auto repl = replacementMap->getReplacement(ftype);
                    if (repl != nullptr) {
                        toReplace.emplace(p->name + "." + f->name, repl);
                        LOG3("Replacing " << dbp(f) << " in parameter "
                             << dbp(p) << " of " << dbp(parser));
                    }
                }
            }
        }
    }
    return parser;
}

const IR::Node* ReplaceHeaders::preorder(IR::P4Control* control) {
    for (auto p : control->getApplyParameters()->parameters) {
        auto pt = replacementMap->typeMap->getType(p, true);
        if (auto st = pt->to<IR::Type_Struct>()) {
            for (auto f : st->fields) {
                auto ftype = replacementMap->typeMap->getType(f, true);
                if (ftype->is<IR::Type_Header>()) {
                    auto repl = replacementMap->getReplacement(ftype);
                    if (repl != nullptr) {
                        toReplace.emplace(p->name + "." + f->name, repl);
                        LOG3("Replacing " << dbp(f) << " in parameter"
                             << dbp(p) << " of " << dbp(control));
                    }
                }
            }
        }
    }
    return control;
}

}  // namespace P4
