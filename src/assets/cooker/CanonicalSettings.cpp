#include "assets/cooker/CanonicalSettings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

namespace Iridium {

    namespace {

        enum class ValueTag : uint8_t {
            Null = 0,
            False = 1,
            True = 2,
            SignedInteger = 3,
            UnsignedInteger = 4,
            Float64 = 5,
            String = 6,
            Array = 7,
            Object = 8,
        };

        template <typename Integer>
        void appendLittleEndian(std::vector<std::byte>& output, Integer value) {
            using Unsigned = std::make_unsigned_t<Integer>;
            const Unsigned bits = static_cast<Unsigned>(value);
            for (size_t byte = 0; byte < sizeof(Integer); ++byte) {
                output.push_back(static_cast<std::byte>(bits >> (byte * 8)));
            }
        }

        void appendTag(std::vector<std::byte>& output, ValueTag tag) {
            output.push_back(static_cast<std::byte>(tag));
        }

        void appendString(std::vector<std::byte>& output, std::string_view value) {
            appendLittleEndian<uint64_t>(output, value.size());
            for (const char character : value) {
                output.push_back(static_cast<std::byte>(
                    static_cast<unsigned char>(character)));
            }
        }

        bool appendValue(const nlohmann::json& value, std::vector<std::byte>& output,
            std::vector<CookDiagnostic>& diagnostics, const std::string& field) {
            if (value.is_null()) {
                appendTag(output, ValueTag::Null);
                return true;
            }
            if (value.is_boolean()) {
                appendTag(output, value.get<bool>() ? ValueTag::True : ValueTag::False);
                return true;
            }
            if (value.is_number_integer()) {
                appendTag(output, ValueTag::SignedInteger);
                appendLittleEndian<int64_t>(output, value.get<int64_t>());
                return true;
            }
            if (value.is_number_unsigned()) {
                appendTag(output, ValueTag::UnsignedInteger);
                appendLittleEndian<uint64_t>(output, value.get<uint64_t>());
                return true;
            }
            if (value.is_number_float()) {
                const double number = value.get<double>();
                if (!std::isfinite(number)) {
                    diagnostics.push_back({
                        .code = "SETTINGS_NONFINITE",
                        .field = field,
                        .message = "Canonical settings reject NaN and infinity.",
                    });
                    return false;
                }
                appendTag(output, ValueTag::Float64);
                appendLittleEndian<uint64_t>(output, std::bit_cast<uint64_t>(number));
                return true;
            }
            if (value.is_string()) {
                appendTag(output, ValueTag::String);
                appendString(output, value.get_ref<const std::string&>());
                return true;
            }
            if (value.is_array()) {
                appendTag(output, ValueTag::Array);
                appendLittleEndian<uint64_t>(output, value.size());
                bool valid = true;
                for (size_t index = 0; index < value.size(); ++index) {
                    valid = appendValue(value[index], output, diagnostics,
                        field + "[" + std::to_string(index) + "]") && valid;
                }
                return valid;
            }
            if (value.is_object()) {
                appendTag(output, ValueTag::Object);
                std::vector<std::string> keys;
                keys.reserve(value.size());
                for (const auto& [key, ignored] : value.items()) {
                    (void)ignored;
                    keys.push_back(key);
                }
                std::sort(keys.begin(), keys.end());
                appendLittleEndian<uint64_t>(output, keys.size());
                bool valid = true;
                for (const std::string& key : keys) {
                    appendString(output, key);
                    valid = appendValue(value.at(key), output, diagnostics,
                        field.empty() ? key : field + "." + key) && valid;
                }
                return valid;
            }
            diagnostics.push_back({
                .code = "SETTINGS_TYPE_UNSUPPORTED",
                .field = field,
                .message = "Settings contain an unsupported JSON value type.",
            });
            return false;
        }

    } // namespace

    CanonicalSettingsResult canonicalizeSettings(const nlohmann::json& settings) {
        CanonicalSettingsResult result;
        appendValue(settings, result.bytes, result.diagnostics, "");
        if (!result.valid()) result.bytes.clear();
        return result;
    }

} // namespace Iridium
