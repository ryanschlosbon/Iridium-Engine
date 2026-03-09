#pragma once
#include "scene/ComponentRegistry.h" // Required for the Auto-Register macro

// The Visitor Pattern Macros
#define REFLECT_BEGIN() \
    template<typename Archive> \
    void reflect(Archive& archive) {

#define PROPERTY(var) \
    archive(#var, var);

#define PROPERTY_READONLY(var) \
    archive.readOnly(#var, var);

#define PROPERTY_PATH(var) \
    archive.filePath(#var, var);

#define BUTTON(label) \
    if (archive.button(label))

#define REFLECT_END() \
    }

// The Auto-Registration Macro
// Forces the C++ compiler to register the component before main() runs!
#define AUTO_REGISTER_COMPONENT(Type) \
    inline static bool Type##_Registered = []() { \
        ComponentRegistry::Register<Type>(#Type); \
        return true; \
    }();