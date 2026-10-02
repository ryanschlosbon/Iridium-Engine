#pragma once

#include "assets/AssetGuid.h"
#include "assets/cooker/CookTypes.h"
#include "ecs/Entity.h"
#include "scene/SceneEntityUuid.h"
#include "scene/runtime/ComponentIdentity.h"
#include "scene/runtime/SceneReferenceState.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kNullCookedSceneIndex = 0xffffffffu;

    struct CookedComponentWriteContext {
        std::function<uint32_t(std::string_view)> internString;
        std::function<std::optional<uint32_t>(Entity)> entityIndex;
        std::function<std::optional<uint32_t>(AssetGuid)> dependencyIndex;
    };

    // Component callbacks write explicit little-endian fields through this class.
    // They never expose C++ object layout, padding, RTTI, or transient ECS indices.
    class CookedComponentWriter {
    public:
        CookedComponentWriter(std::vector<std::byte>& bytes,
            CookedComponentWriteContext context);

        [[nodiscard]] bool writeBoolean(bool value);
        [[nodiscard]] bool writeInt32(int32_t value);
        [[nodiscard]] bool writeUInt32(uint32_t value);
        [[nodiscard]] bool writeFloat32(float value);
        [[nodiscard]] bool writeString(std::string_view value);
        [[nodiscard]] bool writeEntityReference(Entity value);
        [[nodiscard]] bool writeAssetReference(AssetGuid value);

        [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
        [[nodiscard]] const std::string& error() const noexcept { return error_; }

    private:
        void fail(std::string message);

        std::vector<std::byte>* bytes_ = nullptr;
        CookedComponentWriteContext context_;
        std::string error_;
    };

    struct CookedComponentReadContext {
        std::span<const std::string> strings;
        std::span<const SceneEntityUuid> entities;
        std::span<const AssetDependency> dependencies;
        SceneEntityUuid owner;
        ComponentTypeId component;
        SceneReferenceState* references = nullptr;
    };

    class CookedComponentReader {
    public:
        CookedComponentReader(std::span<const std::byte> bytes,
            CookedComponentReadContext context);

        [[nodiscard]] bool readBoolean(bool& value);
        [[nodiscard]] bool readInt32(int32_t& value);
        [[nodiscard]] bool readUInt32(uint32_t& value);
        [[nodiscard]] bool readFloat32(float& value);
        [[nodiscard]] bool readString(std::string& value);
        [[nodiscard]] bool readEntityReference(std::string_view propertyPath,
            bool required, std::optional<SceneEntityUuid>& value);
        [[nodiscard]] bool readAssetReference(std::string_view propertyPath,
            bool required, StableReferenceKind kind, AssetGuid& value);

        [[nodiscard]] bool finish();
        [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
        [[nodiscard]] const std::string& error() const noexcept { return error_; }
        [[nodiscard]] size_t remaining() const noexcept;

    private:
        template <typename Integer>
        bool readInteger(Integer& value);
        void fail(std::string message);

        std::span<const std::byte> bytes_;
        CookedComponentReadContext context_;
        size_t offset_ = 0;
        std::string error_;
    };

} // namespace Iridium
