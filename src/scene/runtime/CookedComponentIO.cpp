#include "scene/runtime/CookedComponentIO.h"

#include "scene/runtime/SceneReferenceState.h"

#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Iridium {
    namespace {

        template <typename Integer>
        void appendInteger(std::vector<std::byte>& bytes, Integer value) {
            using Unsigned = std::make_unsigned_t<Integer>;
            const Unsigned bits = static_cast<Unsigned>(value);
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                bytes.push_back(static_cast<std::byte>(bits >> (index * 8)));
            }
        }

    } // namespace

    CookedComponentWriter::CookedComponentWriter(
        std::vector<std::byte>& bytes,
        CookedComponentWriteContext context)
        : bytes_(&bytes), context_(std::move(context)) {}

    bool CookedComponentWriter::writeBoolean(bool value) {
        bytes_->push_back(value ? std::byte{ 1 } : std::byte{ 0 });
        return true;
    }

    bool CookedComponentWriter::writeInt32(int32_t value) {
        appendInteger(*bytes_, value);
        return true;
    }

    bool CookedComponentWriter::writeUInt32(uint32_t value) {
        appendInteger(*bytes_, value);
        return true;
    }

    bool CookedComponentWriter::writeFloat32(float value) {
        if (!std::isfinite(value)) {
            fail("Cooked component float must be finite");
            return false;
        }
        appendInteger(*bytes_, std::bit_cast<uint32_t>(value));
        return true;
    }

    bool CookedComponentWriter::writeString(std::string_view value) {
        if (!context_.internString) {
            fail("Cooked string table callback is unavailable");
            return false;
        }
        return writeUInt32(context_.internString(value));
    }

    bool CookedComponentWriter::writeEntityReference(Entity value) {
        if (value == NULL_ENTITY) return writeUInt32(kNullCookedSceneIndex);
        if (!context_.entityIndex) {
            fail("Cooked entity index callback is unavailable");
            return false;
        }
        const std::optional<uint32_t> index = context_.entityIndex(value);
        if (!index) {
            fail("Cooked entity reference is outside the scene");
            return false;
        }
        return writeUInt32(*index);
    }

    bool CookedComponentWriter::writeAssetReference(AssetGuid value) {
        if (value.isNil()) return writeUInt32(kNullCookedSceneIndex);
        if (!context_.dependencyIndex) {
            fail("Cooked dependency index callback is unavailable");
            return false;
        }
        const std::optional<uint32_t> index = context_.dependencyIndex(value);
        if (!index) {
            fail("Cooked asset reference is absent from the dependency table");
            return false;
        }
        return writeUInt32(*index);
    }

    void CookedComponentWriter::fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }

    CookedComponentReader::CookedComponentReader(
        std::span<const std::byte> bytes,
        CookedComponentReadContext context)
        : bytes_(bytes), context_(std::move(context)) {}

    template <typename Integer>
    bool CookedComponentReader::readInteger(Integer& value) {
        static_assert(std::is_integral_v<Integer>);
        if (offset_ + sizeof(Integer) > bytes_.size()) {
            fail("Cooked component record is truncated");
            return false;
        }
        using Unsigned = std::make_unsigned_t<Integer>;
        Unsigned bits = 0;
        for (size_t index = 0; index < sizeof(Integer); ++index) {
            bits |= static_cast<Unsigned>(
                std::to_integer<uint8_t>(bytes_[offset_ + index])) << (index * 8);
        }
        offset_ += sizeof(Integer);
        value = static_cast<Integer>(bits);
        return true;
    }

    bool CookedComponentReader::readBoolean(bool& value) {
        uint8_t encoded = 0;
        if (!readInteger(encoded)) return false;
        if (encoded > 1) {
            fail("Cooked boolean is outside the 0/1 domain");
            return false;
        }
        value = encoded != 0;
        return true;
    }

    bool CookedComponentReader::readInt32(int32_t& value) {
        return readInteger(value);
    }

    bool CookedComponentReader::readUInt32(uint32_t& value) {
        return readInteger(value);
    }

    bool CookedComponentReader::readFloat32(float& value) {
        uint32_t bits = 0;
        if (!readInteger(bits)) return false;
        value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) {
            fail("Cooked component float is not finite");
            return false;
        }
        return true;
    }

    bool CookedComponentReader::readString(std::string& value) {
        uint32_t index = 0;
        if (!readUInt32(index)) return false;
        if (index >= context_.strings.size()) {
            fail("Cooked component string index is out of range");
            return false;
        }
        value = context_.strings[index];
        return true;
    }

    bool CookedComponentReader::readEntityReference(
        std::string_view propertyPath,
        bool required,
        std::optional<SceneEntityUuid>& value) {
        uint32_t index = 0;
        if (!readUInt32(index)) return false;
        if (index == kNullCookedSceneIndex) {
            if (required) {
                fail("Required cooked entity reference is null");
                return false;
            }
            value.reset();
            return true;
        }
        if (index >= context_.entities.size()) {
            fail("Cooked entity reference index is out of range");
            return false;
        }
        value = context_.entities[index];
        if (!context_.references || !context_.references->add({
                .key = { context_.owner, context_.component,
                    std::string(propertyPath) },
                .kind = StableReferenceKind::Entity,
                .target = value->bytes(),
                .required = required,
            })) {
            fail("Cooked entity reference is duplicated or invalid");
            return false;
        }
        return true;
    }

    bool CookedComponentReader::readAssetReference(
        std::string_view propertyPath,
        bool required,
        StableReferenceKind kind,
        AssetGuid& value) {
        uint32_t index = 0;
        if (!readUInt32(index)) return false;
        if (index == kNullCookedSceneIndex) {
            if (required) {
                fail("Required cooked asset reference is null");
                return false;
            }
            value = {};
            return true;
        }
        if (index >= context_.dependencies.size()) {
            fail("Cooked asset dependency index is out of range");
            return false;
        }
        const AssetDependency& dependency = context_.dependencies[index];
        if ((dependency.type != AssetDependencyType::Asset &&
                dependency.type != AssetDependencyType::OptionalAsset) ||
            !dependency.assetGuid || dependency.assetGuid->isNil()) {
            fail("Cooked asset dependency has an invalid type or GUID");
            return false;
        }
        value = *dependency.assetGuid;
        if (!context_.references || !context_.references->add({
                .key = { context_.owner, context_.component,
                    std::string(propertyPath) },
                .kind = kind,
                .target = value.bytes(),
                .required = required,
            })) {
            fail("Cooked asset reference is duplicated or invalid");
            return false;
        }
        return true;
    }

    bool CookedComponentReader::finish() {
        if (!valid()) return false;
        if (offset_ != bytes_.size()) {
            fail("Cooked component record has trailing bytes");
            return false;
        }
        return true;
    }

    size_t CookedComponentReader::remaining() const noexcept {
        return offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0;
    }

    void CookedComponentReader::fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }

} // namespace Iridium
