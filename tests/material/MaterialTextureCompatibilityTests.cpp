#include "material/MaterialTextureCompatibility.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n'; return false; } } while (false)

    uint8_t value(const MaterialMipChain& chain, size_t offset) {
        return std::to_integer<uint8_t>(chain.bytes.at(offset));
    }

    bool testSamplerMappingAndMipRequirement() {
        SourceSampler source{};
        source.magFilter.value = 9728;
        source.minFilter.value = 9987;
        source.wrapS.value = 33648;
        source.wrapT.value = 33071;
        const MaterialTextureCompatibilityPlan plan =
            planMaterialTextureCompatibility(source, 8, 4);
        CHECK(plan.mipLevels == 4);
        CHECK(plan.sampler.magFilter == FilterMode::Nearest);
        CHECK(plan.sampler.minFilter == FilterMode::Linear);
        CHECK(plan.sampler.mipmapFilter == MipmapFilterMode::Linear);
        CHECK(plan.sampler.addressU == SamplerAddressMode::MirroredRepeat);
        CHECK(plan.sampler.addressV == SamplerAddressMode::ClampToEdge);
        CHECK(plan.sampler.maxLod == 3);

        SourceSampler defaults{};
        const auto defaultPlan = planMaterialTextureCompatibility(defaults, 8, 4);
        CHECK(defaultPlan.mipLevels == 1);
        CHECK(defaultPlan.sampler.minFilter == FilterMode::Linear);
        CHECK(defaultPlan.sampler.magFilter == FilterMode::Linear);
        CHECK(defaultPlan.sampler.addressU == SamplerAddressMode::Repeat);
        return true;
    }

    bool testMipLayoutAndLinearData() {
        std::array<std::byte, 4 * 4 * 4> pixels{};
        for (size_t texel = 0; texel < 16; ++texel) {
            pixels[texel * 4 + 0] = std::byte{ static_cast<uint8_t>(texel) };
            pixels[texel * 4 + 3] = std::byte{ 255 };
        }
        const MaterialMipChain chain = buildRgba8MipChain(pixels, 4, 4, 3,
            MaterialMipSemantic::LinearData);
        CHECK(chain.levels.size() == 3);
        CHECK(chain.levels[0].byteOffset == 0 && chain.levels[0].byteSize == 64);
        CHECK(chain.levels[1].byteOffset == 64 && chain.levels[1].byteSize == 16);
        CHECK(chain.levels[2].byteOffset == 80 && chain.levels[2].byteSize == 4);
        CHECK(chain.bytes.size() == 84);
        CHECK(value(chain, 64) == 3);
        CHECK(value(chain, 83) == 255);
        return true;
    }

    bool testSrgbAndNormalFiltering() {
        const std::array<std::byte, 16> checker{
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255},
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255},
        };
        const MaterialMipChain srgb = buildRgba8MipChain(checker, 2, 2, 2,
            MaterialMipSemantic::SrgbColor);
        CHECK(value(srgb, 16) >= 187 && value(srgb, 16) <= 188);

        const std::array<std::byte, 16> normals{
            std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255},
            std::byte{255}, std::byte{128}, std::byte{128}, std::byte{255},
            std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255},
            std::byte{0}, std::byte{128}, std::byte{128}, std::byte{255},
        };
        const MaterialMipChain normal = buildRgba8MipChain(normals, 2, 2, 2,
            MaterialMipSemantic::TangentNormal);
        CHECK(std::abs(static_cast<int>(value(normal, 16)) - 128) <= 1);
        CHECK(value(normal, 18) > 250);
        return true;
    }

    bool testInvalidInput() {
        bool threw = false;
        try {
            SourceSampler invalid{};
            invalid.minFilter.value = 1234;
            (void)planMaterialTextureCompatibility(invalid, 4, 4);
        }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        threw = false;
        try {
            const std::array<std::byte, 3> invalid{};
            (void)buildRgba8MipChain(invalid, 1, 1, 1,
                MaterialMipSemantic::LinearData);
        }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        return true;
    }

} // namespace

int main() {
    CHECK(testSamplerMappingAndMipRequirement());
    CHECK(testMipLayoutAndLinearData());
    CHECK(testSrgbAndNormalFiltering());
    CHECK(testInvalidInput());
    std::cout << "Material texture compatibility tests passed\n";
    return 0;
}
