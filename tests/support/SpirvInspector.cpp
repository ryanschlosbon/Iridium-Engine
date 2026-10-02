#include "SpirvInspector.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace Iridium::Test {
namespace {

    constexpr uint32_t SpirvMagic = 0x07230203u;

    enum Op : uint32_t {
        OpName = 5,
        OpMemberName = 6,
        OpEntryPoint = 15,
        OpExecutionMode = 16,
        OpTypeVoid = 19,
        OpTypeBool = 20,
        OpTypeInt = 21,
        OpTypeFloat = 22,
        OpTypeVector = 23,
        OpTypeMatrix = 24,
        OpTypeImage = 25,
        OpTypeSampler = 26,
        OpTypeSampledImage = 27,
        OpTypeArray = 28,
        OpTypeRuntimeArray = 29,
        OpTypeStruct = 30,
        OpTypePointer = 32,
        OpConstant = 43,
        OpSpecConstant = 50,
        OpFunction = 54,
        OpVariable = 59,
        OpDecorate = 71,
        OpMemberDecorate = 72,
    };

    enum Decoration : uint32_t {
        DecorationBlock = 2,
        DecorationBufferBlock = 3,
        DecorationArrayStride = 6,
        DecorationMatrixStride = 7,
        DecorationBuiltIn = 11,
        DecorationLocation = 30,
        DecorationBinding = 33,
        DecorationDescriptorSet = 34,
        DecorationOffset = 35,
    };

    enum StorageClass : uint32_t {
        StorageUniformConstant = 0,
        StorageInput = 1,
        StorageUniform = 2,
        StorageOutput = 3,
        StoragePushConstant = 9,
        StorageStorageBuffer = 12,
    };

    constexpr uint32_t ExecutionModeLocalSize = 17;

    std::string literalString(const uint32_t* words, size_t count) {
        std::string result;
        for (size_t index = 0; index < count; ++index) {
            for (uint32_t byte = 0; byte < 4; ++byte) {
                const char character =
                    static_cast<char>((words[index] >> (byte * 8u)) & 0xffu);
                if (character == '\0') return result;
                result.push_back(character);
            }
        }
        return result;
    }

    template<typename T>
    void ensureSize(std::vector<T>& values, uint32_t id) {
        if (values.size() <= id) values.resize(static_cast<size_t>(id) + 1u);
    }

} // namespace

SpirvModule SpirvModule::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("could not open SPIR-V " + path.string());
    const std::streamsize bytes = input.tellg();
    if (bytes <= 0 || bytes % 4 != 0)
        throw std::runtime_error("malformed SPIR-V size " + path.string());
    input.seekg(0);
    std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4u);
    input.read(reinterpret_cast<char*>(words.data()), bytes);
    return SpirvModule(std::move(words), path.filename().string());
}

SpirvModule::SpirvModule(std::vector<uint32_t> words, std::string label)
    : label_(std::move(label)), words_(std::move(words)) {
    parse();
}

void SpirvModule::parse() {
    if (words_.size() < 5 || words_[0] != SpirvMagic)
        throw std::runtime_error("not a SPIR-V module: " + label_);
    const uint32_t bound = words_[3];
    names_.resize(bound);
    memberNames_.resize(bound);
    types_.resize(bound);
    constants_.resize(bound);
    decorations_.resize(bound);
    memberDecorations_.resize(bound);

    uint32_t entryPoint = 0;
    bool haveEntry = false;
    size_t cursor = 5;
    while (cursor < words_.size()) {
        const uint32_t first = words_[cursor];
        const uint32_t count = first >> 16u;
        const uint32_t opcode = first & 0xffffu;
        if (count == 0 || cursor + count > words_.size())
            throw std::runtime_error("truncated SPIR-V instruction in " + label_);
        const uint32_t* operands = words_.data() + cursor + 1;
        const size_t operandCount = count - 1u;
        const auto id = [&](size_t index) -> uint32_t {
            if (index >= operandCount || operands[index] >= bound)
                throw std::runtime_error("SPIR-V id out of range in " + label_);
            return operands[index];
        };
        switch (opcode) {
        case OpName:
            names_[id(0)] = literalString(operands + 1, operandCount - 1);
            break;
        case OpMemberName: {
            auto& members = memberNames_[id(0)];
            const uint32_t member = operands[1];
            ensureSize(members, member);
            members[member] = literalString(operands + 2, operandCount - 2);
            break;
        }
        case OpEntryPoint:
            if (!haveEntry) {
                executionModel_ = static_cast<SpirvExecutionModel>(operands[0]);
                entryPoint = operands[1];
                haveEntry = true;
            }
            break;
        case OpExecutionMode:
            if (haveEntry && operands[0] == entryPoint &&
                operands[1] == ExecutionModeLocalSize && operandCount >= 5) {
                localSize_ = std::array<uint32_t, 3>{
                    operands[2], operands[3], operands[4] };
            }
            break;
        case OpTypeVoid: case OpTypeBool: case OpTypeInt: case OpTypeFloat:
        case OpTypeVector: case OpTypeMatrix: case OpTypeImage:
        case OpTypeSampler: case OpTypeSampledImage: case OpTypeArray:
        case OpTypeRuntimeArray: case OpTypeStruct: case OpTypePointer: {
            TypeInfo& info = types_[id(0)];
            info.opcode = opcode;
            info.operands.assign(operands + 1, operands + operandCount);
            break;
        }
        case OpConstant: case OpSpecConstant:
            if (operandCount >= 3) constants_[id(1)] = operands[2];
            break;
        case OpFunction:
            functionIds_.push_back(id(1));
            break;
        case OpVariable:
            variables_.push_back({ id(1), id(0), operands[2] });
            break;
        case OpDecorate: {
            Decorations& decoration = decorations_[id(0)];
            const uint32_t kind = operands[1];
            const std::optional<uint32_t> literal = operandCount >= 3
                ? std::optional<uint32_t>(operands[2]) : std::nullopt;
            if (kind == DecorationBlock) decoration.block = true;
            else if (kind == DecorationBufferBlock) decoration.bufferBlock = true;
            else if (kind == DecorationArrayStride) decoration.arrayStride = literal;
            else if (kind == DecorationBuiltIn) decoration.builtIn = literal;
            else if (kind == DecorationLocation) decoration.location = literal;
            else if (kind == DecorationBinding) decoration.binding = literal;
            else if (kind == DecorationDescriptorSet) decoration.set = literal;
            break;
        }
        case OpMemberDecorate: {
            auto& members = memberDecorations_[id(0)];
            const uint32_t member = operands[1];
            ensureSize(members, member);
            const uint32_t kind = operands[2];
            const std::optional<uint32_t> literal = operandCount >= 4
                ? std::optional<uint32_t>(operands[3]) : std::nullopt;
            if (kind == DecorationOffset) members[member].offset = literal;
            else if (kind == DecorationMatrixStride)
                members[member].matrixStride = literal;
            else if (kind == DecorationBuiltIn) members[member].builtIn = literal;
            break;
        }
        default:
            break;
        }
        cursor += count;
    }

    for (const uint32_t function : functionIds_) {
        std::string name = names_[function];
        const size_t signature = name.find('(');
        if (signature != std::string::npos) name.resize(signature);
        if (!name.empty()) functions_.push_back(std::move(name));
    }
}

bool SpirvModule::callsFunction(std::string_view name) const noexcept {
    return std::ranges::find(functions_, name) != functions_.end();
}

const SpirvModule::TypeInfo* SpirvModule::type(uint32_t id) const noexcept {
    if (id >= types_.size() || types_[id].opcode == 0) return nullptr;
    return &types_[id];
}

uint32_t SpirvModule::pointee(uint32_t pointerType) const noexcept {
    const TypeInfo* pointer = type(pointerType);
    if (pointer == nullptr || pointer->opcode != OpTypePointer ||
        pointer->operands.size() < 2) return 0;
    return pointer->operands[1];
}

std::string SpirvModule::nameOf(uint32_t id) const {
    return id < names_.size() ? names_[id] : std::string{};
}

std::optional<uint32_t> SpirvModule::constantValue(uint32_t id) const noexcept {
    return id < constants_.size() ? constants_[id] : std::nullopt;
}

uint32_t SpirvModule::naturalSize(uint32_t typeId) const {
    const TypeInfo* info = type(typeId);
    if (info == nullptr) return 0;
    switch (info->opcode) {
    case OpTypeBool: return 4;
    case OpTypeInt: case OpTypeFloat: return info->operands[0] / 8u;
    case OpTypeVector: return naturalSize(info->operands[0]) * info->operands[1];
    case OpTypeMatrix: return naturalSize(info->operands[0]) * info->operands[1];
    case OpTypeArray: {
        const uint32_t length = constantValue(info->operands[1]).value_or(0);
        const auto& decoration = decorations_[typeId];
        const uint32_t stride = decoration.arrayStride.value_or(
            naturalSize(info->operands[0]));
        return length * stride;
    }
    case OpTypeRuntimeArray: return 0;
    case OpTypeStruct: {
        uint32_t extent = 0;
        for (uint32_t member = 0; member < info->operands.size(); ++member) {
            const auto& decorations = memberDecorations_[typeId];
            const uint32_t offset = member < decorations.size()
                ? decorations[member].offset.value_or(0) : 0;
            extent = (std::max)(extent, offset + memberSize(typeId, member));
        }
        return extent;
    }
    default: return 0;
    }
}

uint32_t SpirvModule::memberSize(uint32_t structId, uint32_t member) const {
    const TypeInfo* info = type(structId);
    if (info == nullptr || member >= info->operands.size()) return 0;
    const uint32_t memberType = info->operands[member];
    const TypeInfo* memberInfo = type(memberType);
    if (memberInfo != nullptr && memberInfo->opcode == OpTypeMatrix) {
        const auto& decorations = memberDecorations_[structId];
        if (member < decorations.size() && decorations[member].matrixStride)
            return *decorations[member].matrixStride * memberInfo->operands[1];
    }
    return naturalSize(memberType);
}

std::vector<SpirvMember> SpirvModule::membersOf(uint32_t structId) const {
    std::vector<SpirvMember> result;
    const TypeInfo* info = type(structId);
    if (info == nullptr || info->opcode != OpTypeStruct) return result;
    const auto& names = memberNames_[structId];
    const auto& decorations = memberDecorations_[structId];
    for (uint32_t member = 0; member < info->operands.size(); ++member) {
        SpirvMember value;
        value.name = member < names.size() ? names[member] : std::string{};
        value.offset = member < decorations.size()
            ? decorations[member].offset.value_or(0) : 0;
        value.size = memberSize(structId, member);
        value.typeId = info->operands[member];
        result.push_back(std::move(value));
    }
    return result;
}

std::optional<uint32_t> SpirvModule::findLaidOutStruct(std::string_view name) const {
    std::optional<uint32_t> fallback;
    for (uint32_t id = 0; id < types_.size(); ++id) {
        if (types_[id].opcode != OpTypeStruct || names_[id] != name) continue;
        const auto& decorations = memberDecorations_[id];
        const bool laidOut = std::ranges::any_of(decorations,
            [](const MemberDecorations& member) { return member.offset.has_value(); });
        if (laidOut) return id;
        if (!fallback) fallback = id;
    }
    return fallback;
}

std::optional<std::vector<SpirvMember>> SpirvModule::structMembers(
    std::string_view structName) const {
    const std::optional<uint32_t> id = findLaidOutStruct(structName);
    if (!id) return std::nullopt;
    return membersOf(*id);
}

std::optional<uint32_t> SpirvModule::memberOffset(std::string_view structName,
    std::string_view memberName) const {
    const auto members = structMembers(structName);
    if (!members) return std::nullopt;
    for (const SpirvMember& member : *members)
        if (member.name == memberName) return member.offset;
    return std::nullopt;
}

std::optional<uint32_t> SpirvModule::structExtent(std::string_view structName) const {
    const std::optional<uint32_t> id = findLaidOutStruct(structName);
    if (!id) return std::nullopt;
    return naturalSize(*id);
}

std::optional<uint32_t> SpirvModule::memberArrayStride(std::string_view structName,
    std::string_view memberName) const {
    const auto members = structMembers(structName);
    if (!members) return std::nullopt;
    for (const SpirvMember& member : *members) {
        if (member.name != memberName) continue;
        const TypeInfo* info = type(member.typeId);
        if (info == nullptr || (info->opcode != OpTypeArray &&
            info->opcode != OpTypeRuntimeArray)) return std::nullopt;
        return decorations_[member.typeId].arrayStride;
    }
    return std::nullopt;
}

std::vector<SpirvDescriptorBinding> SpirvModule::descriptorBindings() const {
    std::vector<SpirvDescriptorBinding> result;
    for (const Variable& variable : variables_) {
        const Decorations& decoration = decorations_[variable.id];
        if (!decoration.set || !decoration.binding) continue;
        SpirvDescriptorBinding binding;
        binding.set = *decoration.set;
        binding.binding = *decoration.binding;
        uint32_t typeId = pointee(variable.pointerType);
        for (const TypeInfo* info = type(typeId); info != nullptr &&
            (info->opcode == OpTypeArray || info->opcode == OpTypeRuntimeArray);
            info = type(typeId)) {
            binding.count = info->opcode == OpTypeRuntimeArray ? 0u :
                binding.count * constantValue(info->operands[1]).value_or(1);
            typeId = info->operands[0];
        }
        binding.typeName = nameOf(typeId);
        binding.name = nameOf(variable.id);
        if (binding.name.empty()) binding.name = binding.typeName;
        const TypeInfo* info = type(typeId);
        const Decorations& typeDecoration = decorations_[typeId];
        const TypeInfo* image = nullptr;
        if (info == nullptr) binding.kind = SpirvDescriptorKind::Other;
        else if (info->opcode == OpTypeStruct) {
            if (variable.storageClass == StorageStorageBuffer ||
                typeDecoration.bufferBlock)
                binding.kind = SpirvDescriptorKind::StorageBuffer;
            else if (variable.storageClass == StorageUniform)
                binding.kind = SpirvDescriptorKind::UniformBuffer;
        }
        else if (info->opcode == OpTypeSampledImage) {
            binding.kind = SpirvDescriptorKind::CombinedImageSampler;
            image = type(info->operands[0]);
        }
        else if (info->opcode == OpTypeSampler)
            binding.kind = SpirvDescriptorKind::Sampler;
        else if (info->opcode == OpTypeImage) {
            binding.kind = info->operands.size() >= 6 && info->operands[5] == 2u
                ? SpirvDescriptorKind::StorageImage : SpirvDescriptorKind::SampledImage;
            image = info;
        }
        if (image != nullptr && image->opcode == OpTypeImage && image->operands.size() >= 4) {
            // OpTypeImage operands: sampled type, Dim, Depth, Arrayed, MS, Sampled.
            binding.imageDim = image->operands[1];
            binding.imageArrayed = image->operands[3] != 0u;
        }
        result.push_back(std::move(binding));
    }
    std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
        return lhs.set != rhs.set ? lhs.set < rhs.set : lhs.binding < rhs.binding;
    });
    return result;
}

std::optional<SpirvDescriptorBinding> SpirvModule::descriptor(uint32_t set,
    uint32_t binding) const {
    for (SpirvDescriptorBinding& value : descriptorBindings())
        if (value.set == set && value.binding == binding) return std::move(value);
    return std::nullopt;
}

std::optional<SpirvDescriptorBinding> SpirvModule::descriptorNamed(
    std::string_view name) const {
    for (SpirvDescriptorBinding& value : descriptorBindings())
        if (value.name == name || value.typeName == name) return std::move(value);
    return std::nullopt;
}

bool SpirvModule::hasPushConstants() const noexcept {
    return std::ranges::any_of(variables_, [](const Variable& variable) {
        return variable.storageClass == StoragePushConstant;
    });
}

std::optional<std::string> SpirvModule::pushConstantBlockName() const {
    for (const Variable& variable : variables_)
        if (variable.storageClass == StoragePushConstant)
            return nameOf(pointee(variable.pointerType));
    return std::nullopt;
}

std::vector<SpirvMember> SpirvModule::pushConstantMembers() const {
    for (const Variable& variable : variables_)
        if (variable.storageClass == StoragePushConstant)
            return membersOf(pointee(variable.pointerType));
    return {};
}

uint32_t SpirvModule::pushConstantExtent() const {
    for (const Variable& variable : variables_)
        if (variable.storageClass == StoragePushConstant)
            return naturalSize(pointee(variable.pointerType));
    return 0;
}

std::vector<SpirvInterfaceVariable> SpirvModule::interface(uint32_t storageClass) const {
    std::vector<SpirvInterfaceVariable> result;
    for (const Variable& variable : variables_) {
        if (variable.storageClass != storageClass) continue;
        const Decorations& decoration = decorations_[variable.id];
        if (decoration.builtIn || !decoration.location) continue;
        const uint32_t typeId = pointee(variable.pointerType);
        if (const TypeInfo* info = type(typeId); info != nullptr &&
            info->opcode == OpTypeStruct) continue; // builtin blocks
        SpirvInterfaceVariable value;
        value.location = *decoration.location;
        value.name = nameOf(variable.id);
        uint32_t component = typeId;
        const TypeInfo* info = type(component);
        if (info != nullptr && info->opcode == OpTypeMatrix) {
            value.columnCount = info->operands[1];
            component = info->operands[0];
            info = type(component);
        }
        value.componentCount = 1;
        if (info != nullptr && info->opcode == OpTypeVector) {
            value.componentCount = info->operands[1];
            component = info->operands[0];
            info = type(component);
        }
        if (info != nullptr) {
            if (info->opcode == OpTypeFloat) value.scalar = SpirvScalarKind::Float;
            else if (info->opcode == OpTypeBool) value.scalar = SpirvScalarKind::Bool;
            else if (info->opcode == OpTypeInt)
                value.scalar = info->operands[1] != 0
                    ? SpirvScalarKind::SignedInt : SpirvScalarKind::UnsignedInt;
            if (info->opcode == OpTypeFloat || info->opcode == OpTypeInt)
                value.scalarBits = info->operands[0];
        }
        result.push_back(std::move(value));
    }
    std::ranges::sort(result, {}, &SpirvInterfaceVariable::location);
    return result;
}

std::vector<SpirvInterfaceVariable> SpirvModule::inputs() const {
    return interface(StorageInput);
}

std::vector<SpirvInterfaceVariable> SpirvModule::outputs() const {
    return interface(StorageOutput);
}

} // namespace Iridium::Test
