/*------------------------------------
  ///\  Plywood C++ Framework
  \\\/  https://plywood.arc80.com/
------------------------------------*/
#include <pylon-reflect/Core.h>
#include <pylon-reflect/Import.h>
#include <ply-reflect/SavedTypedPtr.h>
#include <ply-reflect/TypeDescriptorOwner.h>
#include <ply-reflect/TypedArray.h>

namespace pylon {

struct PylonTypeImporter {
    TypeDescriptorOwner* typeOwner = nullptr;
    const Functor<TypeFromName>& typeFromName;

    PylonTypeImporter(const Functor<TypeFromName>& typeFromName) : typeFromName{typeFromName} {
    }

    PLY_NO_INLINE TypeDescriptor* convertType(const Node* aNode) {
        if (aNode->isText()) {
            // It's a primitive type, represented by a string in Pylon.
            // FIXME: This could use a hash table.
            // Note: TypeKey_SavedTypedPtr/TypeArray::read could use the same hash table if we
            // ever want them to resolve built-in types.
            StringView str = aNode->text();
            if (str == "u16") {
                return TypeResolver<u16>::get();
            } else if (str == "u16_2") {
                return TypeResolver<u16[2]>::get();
            } else if (str == "u16_3") {
                return TypeResolver<u16[3]>::get();
            } else if (str == "u16_4") {
                return TypeResolver<u16[4]>::get();
            } else if (str == "float") {
                return TypeResolver<float>::get();
            } else {
                TypeDescriptor* typeDesc = nullptr;
                if (typeFromName.isValid()) {
                    typeDesc = typeFromName.call(str);
                }
                PLY_ASSERT(typeDesc); // Unrecognized primitive type
                return typeDesc;
            }
        } else if (aNode->isObject()) {
            // It's either a struct or an enum (not supported yet).
            StringView key = aNode->get("key")->text();
            if (key == "struct") {
                PLY_ASSERT(typeOwner); // Must provide an owner for synthesized structs
                // Synthesize a struct
                const Node* aName = aNode->get("name");
                PLY_ASSERT(aName->isText());
                TypeDescriptor_Struct* structType = new TypeDescriptor_Struct{0, aName->text()};
                auto appendMember = [&](StringView memberName, TypeDescriptor* memberType) {
                    structType->appendMember(memberName, memberType);

                    // FIXME: Different structs will have different alignment requirements (eg.
                    // uniform buffers have different alignment requirements from vertex
                    // attributes). This code only assumes iOS vertex attributes:
                    u32 alignment = structType->fixedSize % 4;
                    if (alignment > 0) {
                        PLY_ASSERT(alignment == 2); // only case currently handled
                        structType->appendMember("padding", TypeResolver<u16>::get());
                    }
                };
                const Node* aMembers = aNode->get("members");
                if (aMembers->isObject()) {
                    for (const Node::Object::Item& item : aMembers->object().items) {
                        appendMember(item.key, convertType(item.value));
                    }
                } else if (aMembers->isArray()) {
                    for (const Node* aMember : aMembers->arrayView()) {
                        PLY_ASSERT(aMember->isArray());
                        PLY_ASSERT(aMember->arrayView().numItems == 2);
                        appendMember(aMember->arrayView()[0]->text(),
                                     convertType(aMember->arrayView()[1]));
                    }
                } else {
                    PLY_ASSERT(0);
                }
                typeOwner->adoptType(structType);
                return structType;
            } else {
                PLY_ASSERT(0); // Unrecognized or missing type key
            }
        } else {
            // This Pylon node cannot be converted to a TypeDescriptor
            PLY_ASSERT(0);
        }
        return nullptr;
    }
};

PLY_NO_INLINE TypeDescriptorOwner* convertTypeFrom(const Node* aNode,
                                                   const Functor<TypeFromName>& typeFromName) {
    PylonTypeImporter importer{typeFromName};
    importer.typeOwner = new TypeDescriptorOwner;
    importer.typeOwner->setRootType(importer.convertType(aNode));
    return importer.typeOwner;
}

PLY_NO_INLINE void convertFrom(TypedPtr obj, const Node* aNode,
                               const Functor<TypeFromName>& typeFromName) {
    auto error = [&] {}; // FIXME: Decide where these go

    PLY_ASSERT(aNode->isValid());
    // FIXME: Handle errors gracefully by logging a message, returning false and marking the
    // cook as failed (instead of asserting).
    if (obj.type->typeKey == &TypeKey_Struct) {
        PLY_ASSERT(aNode->isObject());
        auto* structDesc = obj.type->cast<TypeDescriptor_Struct>();
        for (const TypeDescriptor_Struct::Member& member : structDesc->members) {
            const Node* aMember = aNode->get(member.name);
            if (aMember->isValid()) {
                TypedPtr m{PLY_PTR_OFFSET(obj.ptr, member.offset), member.type};
                convertFrom(m, aMember, typeFromName);
            }
        }
    } else if (obj.type->typeKey == &TypeKey_Float) {
        Tuple<bool, double> pair = aNode->numeric();
        PLY_ASSERT(pair.first);
        *(float*) obj.ptr = (float) pair.second;
    } else if (obj.type->typeKey == &TypeKey_U8) {
        Tuple<bool, double> pair = aNode->numeric();
        PLY_ASSERT(pair.first);
        *(u8*) obj.ptr = (u8) pair.second;
    } else if (obj.type->typeKey == &TypeKey_U16) {
        Tuple<bool, double> pair = aNode->numeric();
        PLY_ASSERT(pair.first);
        *(u16*) obj.ptr = (u16) pair.second;
    } else if (obj.type->typeKey == &TypeKey_Bool) {
        *(bool*) obj.ptr = aNode->text() == "true";
    } else if (obj.type->typeKey == &TypeKey_U32) {
        Tuple<bool, double> pair = aNode->numeric();
        PLY_ASSERT(pair.first);
        *(u32*) obj.ptr = (u32) pair.second;
    } else if (obj.type->typeKey == &TypeKey_S32) {
        Tuple<bool, double> pair = aNode->numeric();
        PLY_ASSERT(pair.first);
        *(s32*) obj.ptr = (s32) pair.second;
    } else if (obj.type->typeKey == &TypeKey_FixedArray) {
        PLY_ASSERT(aNode->isArray());
        auto* fixedArrType = obj.type->cast<TypeDescriptor_FixedArray>();
        u32 itemSize = fixedArrType->itemType->fixedSize;
        for (u32 i = 0; i < fixedArrType->numItems; i++) {
            TypedPtr elem{PLY_PTR_OFFSET(obj.ptr, itemSize * i), fixedArrType->itemType};
            convertFrom(elem, aNode->get(i), typeFromName);
        }
    } else if (obj.type->typeKey == &TypeKey_String) {
        if (aNode->isText()) {
            *(String*) obj.ptr = aNode->text();
        } else {
            error();
        }
    } else if (obj.type->typeKey == &TypeKey_Array) {
        PLY_ASSERT(aNode->isArray());
        ArrayView<const Node* const> aNodeArr = aNode->arrayView();
        auto* arrType = static_cast<TypeDescriptor_Array*>(obj.type);
        details::BaseArray* arr = (details::BaseArray*) obj.ptr;
        u32 oldArrSize = arr->m_numItems;
        u32 newArrSize = aNodeArr.numItems;
        u32 itemSize = arrType->itemType->fixedSize;
        for (u32 i = newArrSize; i < oldArrSize; i++) {
            TypedPtr{PLY_PTR_OFFSET(arr->m_items, itemSize * i), arrType->itemType}.destruct();
        }
        arr->realloc(newArrSize, itemSize);
        for (u32 i = oldArrSize; i < newArrSize; i++) {
            TypedPtr{PLY_PTR_OFFSET(arr->m_items, itemSize * i), arrType->itemType}.construct();
        }
        for (u32 i = 0; i < newArrSize; i++) {
            TypedPtr elem{PLY_PTR_OFFSET(arr->m_items, itemSize * i), arrType->itemType};
            convertFrom(elem, aNodeArr[i], typeFromName);
        }
    } else if (obj.type->typeKey == &TypeKey_EnumIndexedArray) {
        PLY_ASSERT(aNode->isObject());
        auto* arrayDesc = obj.type->cast<TypeDescriptor_EnumIndexedArray>();
        for (const TypeDescriptor_Enum::Identifier& identifier : arrayDesc->enumType->identifiers) {
            const Node* aMember = aNode->get(identifier.name);
            if (aMember->isValid()) {
                TypedPtr m{
                    PLY_PTR_OFFSET(obj.ptr, arrayDesc->itemType->fixedSize * identifier.value),
                    arrayDesc->itemType};
                convertFrom(m, aMember, typeFromName);
            }
        }
    } else if (obj.type->typeKey == &TypeKey_Enum) {
        PLY_ASSERT(aNode->isText());
        auto* enumDesc = obj.type->cast<TypeDescriptor_Enum>();
        bool found = false;
        for (const auto& identifier : enumDesc->identifiers) {
            if (identifier.name == aNode->text()) {
                if (enumDesc->fixedSize == 1) {
                    PLY_ASSERT(identifier.value <= UINT8_MAX);
                    *(u8*) obj.ptr = (u8) identifier.value;
                } else if (enumDesc->fixedSize == 2) {
                    PLY_ASSERT(identifier.value <= UINT16_MAX);
                    *(u16*) obj.ptr = (u16) identifier.value;
                } else if (enumDesc->fixedSize == 4) {
                    *(u32*) obj.ptr = identifier.value;
                } else {
                    PLY_ASSERT(0);
                }
                found = true;
                break;
            }
        }
        PLY_ASSERT(found);
        PLY_UNUSED(found);
    } else if (obj.type->typeKey == &TypeKey_SavedTypedPtr) {
        PLY_ASSERT(aNode->isObject());
        TypeDescriptorOwner* targetTypeOwner = convertTypeFrom(aNode->get("type"), typeFromName);
        SavedTypedPtr* savedTypedPtr = (SavedTypedPtr*) obj.ptr;
        savedTypedPtr->typeOwner = targetTypeOwner;
        savedTypedPtr->owned = TypedPtr::create(targetTypeOwner->getRootType());
        convertFrom(savedTypedPtr->owned, aNode->get("value"), typeFromName);
    } else if (obj.type->typeKey == &TypeKey_TypedArray) {
        PLY_ASSERT(aNode->isObject());
        TypeDescriptorOwner* itemTypeOwner = convertTypeFrom(aNode->get("type"), typeFromName);
        const pylon::Node* aData = aNode->get("data");
        ArrayView<const pylon::Node* const> aDataArr = aData->arrayView();
        TypedArray* arr = (TypedArray*) obj.ptr;
        arr->create(itemTypeOwner, aDataArr.numItems);
        TypedPtr item = {arr->m_array.m_items, itemTypeOwner->getRootType()};
        for (u32 i = 0; i < aDataArr.numItems; i++) {
            convertFrom(item, aDataArr[i], typeFromName);
            item.ptr = PLY_PTR_OFFSET(item.ptr, itemTypeOwner->getRootType()->fixedSize);
        }
    } else if (obj.type->typeKey == &TypeKey_Switch) {
        PLY_ASSERT(aNode->isObject());
        auto* switchDesc = obj.type->cast<TypeDescriptor_Switch>();
        PLY_ASSERT(aNode->object().items.numItems() == 1);
        auto iter = aNode->object().items.begin();
        const StringView& stateName = iter->key;
        bool found = false;
        for (u32 i = 0; i < switchDesc->states.numItems(); i++) {
            const TypeDescriptor_Switch::State& state = switchDesc->states[i];
            if (state.name == stateName) {
                switchDesc->ensureStateIs(obj, (u16) i);
                TypedPtr m{PLY_PTR_OFFSET(obj.ptr, switchDesc->storageOffset), state.structType};
                convertFrom(m, iter->value, typeFromName);
                found = true;
                break;
            }
        }
        PLY_ASSERT(found);
        PLY_UNUSED(found);
    } else if (obj.type->typeKey == &TypeKey_Owned) {
        auto* ownedDesc = obj.type->cast<TypeDescriptor_Owned>();
        TypedPtr created = TypedPtr::create(ownedDesc->targetType);
        *(void**) obj.ptr = created.ptr;
        convertFrom(created, aNode, typeFromName);
    } else {
        PLY_ASSERT(0); // Unsupported member type
    }
}

PLY_NO_INLINE OwnTypedPtr import(TypeDescriptor* typeDesc, const Node* aRoot,
                                 const Functor<TypeFromName>& typeFromName) {
    OwnTypedPtr result = TypedPtr::create(typeDesc);
    convertFrom(result, aRoot, typeFromName);
    return result;
}

PLY_NO_INLINE void importInto(TypedPtr obj, const Node* aRoot,
                              const Functor<TypeFromName>& typeFromName) {
    convertFrom(obj, aRoot, typeFromName);
}

} // namespace pylon
