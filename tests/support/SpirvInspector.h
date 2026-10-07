#pragma once

// Read-only SPIR-V reflection for tests. It inspects the compiled binaries the
// engine loads (assets/shaders/*.spv), so assertions follow what the driver sees
// rather than GLSL source text. Only the subset of SPIR-V needed for ABI checks is
// decoded: names, member offsets, descriptor decorations, local size, push-block
// extents, stage interface locations, and the set of linked (called) functions.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium::Test {

    enum class SpirvExecutionModel : uint32_t {
        Vertex = 0,
        Fragment = 4,
        GLCompute = 5,
        Unknown = 0xffffffffu,
    };

    enum class SpirvDescriptorKind : uint8_t {
        UniformBuffer,
        StorageBuffer,
        CombinedImageSampler,
        SampledImage,
        StorageImage,
        Sampler,
        Other,
    };

    enum class SpirvScalarKind : uint8_t {
        None,
        Float,
        SignedInt,
        UnsignedInt,
        Bool,
    };

    struct SpirvMember {
        std::string name;
        uint32_t offset = 0;
        // Byte extent of the member within its block (array length * stride,
        // columns * matrix stride, or the natural size of scalars/vectors).
        // Zero for a trailing runtime array.
        uint32_t size = 0;
        uint32_t typeId = 0;
    };

    struct SpirvDescriptorBinding {
        uint32_t set = 0;
        uint32_t binding = 0;
        // Variable name, or the block type name for anonymous blocks.
        std::string name;
        std::string typeName;
        SpirvDescriptorKind kind = SpirvDescriptorKind::Other;
        // 1 for a single descriptor, N for a sized array, 0 for a runtime array.
        uint32_t count = 1;
        // Image descriptors: SPIR-V Dim (1 = 2D, 2 = 3D, 3 = Cube) and arrayed.
        std::optional<uint32_t> imageDim;
        bool imageArrayed = false;
    };

    struct SpirvInterfaceVariable {
        uint32_t location = 0;
        std::string name;
        SpirvScalarKind scalar = SpirvScalarKind::None;
        uint32_t componentCount = 0; // vector width (1 for scalars)
        uint32_t columnCount = 1;    // > 1 for matrix inputs (one location each)
        uint32_t scalarBits = 0;
    };

    class SpirvModule {
    public:
        [[nodiscard]] static SpirvModule load(const std::filesystem::path& path);
        explicit SpirvModule(std::vector<uint32_t> words, std::string label = {});

        [[nodiscard]] const std::string& label() const noexcept { return label_; }
        [[nodiscard]] SpirvExecutionModel executionModel() const noexcept {
            return executionModel_;
        }
        [[nodiscard]] std::optional<std::array<uint32_t, 3>> localSize() const noexcept {
            return localSize_;
        }

        // Linked functions (glslang drops uncalled functions), without the
        // "(signature" suffix glslang appends to OpName.
        [[nodiscard]] const std::vector<std::string>& functionNames() const noexcept {
            return functions_;
        }
        [[nodiscard]] bool callsFunction(std::string_view name) const noexcept;

        [[nodiscard]] std::vector<SpirvDescriptorBinding> descriptorBindings() const;
        [[nodiscard]] std::optional<SpirvDescriptorBinding> descriptor(
            uint32_t set, uint32_t binding) const;
        [[nodiscard]] std::optional<SpirvDescriptorBinding> descriptorNamed(
            std::string_view name) const;

        // Members and extent of the struct type with this OpName that carries
        // explicit Offset decorations (the laid-out variant used by a block).
        [[nodiscard]] std::optional<std::vector<SpirvMember>> structMembers(
            std::string_view structName) const;
        [[nodiscard]] std::optional<uint32_t> memberOffset(
            std::string_view structName, std::string_view memberName) const;
        [[nodiscard]] std::optional<uint32_t> structExtent(
            std::string_view structName) const;
        // ArrayStride of a runtime/sized array member, or nullopt.
        [[nodiscard]] std::optional<uint32_t> memberArrayStride(
            std::string_view structName, std::string_view memberName) const;

        [[nodiscard]] bool hasPushConstants() const noexcept;
        [[nodiscard]] std::optional<std::string> pushConstantBlockName() const;
        [[nodiscard]] std::vector<SpirvMember> pushConstantMembers() const;
        // Byte extent of the push block: max(offset + size) over its members.
        [[nodiscard]] uint32_t pushConstantExtent() const;

        [[nodiscard]] std::vector<SpirvInterfaceVariable> inputs() const;
        [[nodiscard]] std::vector<SpirvInterfaceVariable> outputs() const;

    private:
        struct TypeInfo {
            uint32_t opcode = 0;
            std::vector<uint32_t> operands; // operands after the result id
        };
        struct Decorations {
            std::optional<uint32_t> set, binding, location, arrayStride, builtIn;
            bool block = false;
            bool bufferBlock = false;
        };
        struct MemberDecorations {
            std::optional<uint32_t> offset, matrixStride, builtIn;
        };
        struct Variable {
            uint32_t id = 0;
            uint32_t pointerType = 0;
            uint32_t storageClass = 0;
        };

        void parse();
        [[nodiscard]] const TypeInfo* type(uint32_t id) const noexcept;
        [[nodiscard]] uint32_t pointee(uint32_t pointerType) const noexcept;
        [[nodiscard]] std::string nameOf(uint32_t id) const;
        [[nodiscard]] std::optional<uint32_t> constantValue(uint32_t id) const noexcept;
        [[nodiscard]] uint32_t naturalSize(uint32_t typeId) const;
        [[nodiscard]] uint32_t memberSize(uint32_t structId, uint32_t member) const;
        [[nodiscard]] std::vector<SpirvMember> membersOf(uint32_t structId) const;
        [[nodiscard]] std::optional<uint32_t> findLaidOutStruct(
            std::string_view name) const;
        [[nodiscard]] std::vector<SpirvInterfaceVariable> interface(
            uint32_t storageClass) const;

        std::string label_;
        std::vector<uint32_t> words_;
        SpirvExecutionModel executionModel_ = SpirvExecutionModel::Unknown;
        std::optional<std::array<uint32_t, 3>> localSize_;
        std::vector<std::string> functions_;
        std::vector<std::string> names_;            // indexed by id
        std::vector<std::vector<std::string>> memberNames_;
        std::vector<TypeInfo> types_;                // indexed by id
        std::vector<std::optional<uint32_t>> constants_;
        std::vector<Decorations> decorations_;
        std::vector<std::vector<MemberDecorations>> memberDecorations_;
        std::vector<Variable> variables_;
        std::vector<uint32_t> functionIds_;
    };

} // namespace Iridium::Test
