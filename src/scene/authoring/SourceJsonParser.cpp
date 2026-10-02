#include "scene/authoring/SourceJsonParser.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Iridium {
    namespace {

        class DuplicateJsonKey final : public std::runtime_error {
        public:
            explicit DuplicateJsonKey(std::string key)
                : std::runtime_error("Duplicate JSON object key: " + key),
                  key_(std::move(key)) {}

            [[nodiscard]] const std::string& key() const noexcept { return key_; }

        private:
            std::string key_;
        };

        class ObjectKeySet {
        public:
            [[nodiscard]] bool insert(std::string key) {
                if (wide_) return wide_->insert(std::move(key)).second;
                for (size_t index = 0; index < count_; ++index) {
                    if (*small_[index] == key) return false;
                }
                if (count_ < small_.size()) {
                    small_[count_++].emplace(std::move(key));
                    return true;
                }
                wide_ = std::make_unique<std::unordered_set<std::string>>();
                wide_->reserve(small_.size() * 2);
                for (std::optional<std::string>& value : small_) {
                    wide_->insert(std::move(*value));
                    value.reset();
                }
                return wide_->insert(std::move(key)).second;
            }

        private:
            // Source scene objects are intentionally narrow. This avoids one
            // bucket allocation plus one node allocation per key for the common
            // envelope/entity/component/reference shapes while keeping wide
            // extension objects bounded by a hash-set fallback.
            std::array<std::optional<std::string>, 12> small_{};
            size_t count_ = 0;
            std::unique_ptr<std::unordered_set<std::string>> wide_;
        };

        [[nodiscard]] std::string escapeJsonPointerToken(std::string_view token) {
            std::string result;
            result.reserve(token.size());
            for (char value : token) {
                if (value == '~') result += "~0";
                else if (value == '/') result += "~1";
                else result.push_back(value);
            }
            return result;
        }

        [[nodiscard]] SceneDiagnostic parseError(
            std::string code,
            std::string message,
            std::string propertyPath = {}) {
            return {
                .severity = SceneDiagnosticSeverity::Error,
                .code = std::move(code),
                .phase = ScenePhase::Parse,
                .propertyPath = std::move(propertyPath),
                .message = std::move(message),
            };
        }

    } // namespace

    SourceJsonParseResult parseSourceJsonStrict(
        std::string_view bytes,
        SourceJsonParseOptions options) {
        SourceJsonParseResult result;
        if (bytes.size() > options.maximumBytes) {
            result.diagnostics.push_back(parseError(
                "scene.json.size_limit",
                "Source scene JSON exceeds the configured byte limit"));
            return result;
        }
        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xefu &&
            static_cast<unsigned char>(bytes[1]) == 0xbbu &&
            static_cast<unsigned char>(bytes[2]) == 0xbfu) {
            result.diagnostics.push_back(parseError(
                "scene.json.bom_not_allowed",
                "Source scene JSON must be UTF-8 without a byte-order mark"));
            return result;
        }

        std::vector<ObjectKeySet> objectKeys;
        objectKeys.reserve(32);
        const SourceJson::parser_callback_t callback =
            [&objectKeys](int, SourceJson::parse_event_t event, SourceJson& value) {
                if (event == SourceJson::parse_event_t::object_start) {
                    objectKeys.emplace_back();
                }
                else if (event == SourceJson::parse_event_t::key) {
                    if (objectKeys.empty()) {
                        throw std::logic_error(
                            "JSON parser reported a key outside an object");
                    }
                    const std::string key = value.get<std::string>();
                    if (!objectKeys.back().insert(key)) {
                        throw DuplicateJsonKey(key);
                    }
                }
                else if (event == SourceJson::parse_event_t::object_end) {
                    if (objectKeys.empty()) {
                        throw std::logic_error(
                            "JSON parser reported an unmatched object end");
                    }
                    objectKeys.pop_back();
                }
                return true;
            };

        try {
            result.value = SourceJson::parse(
                bytes.begin(), bytes.end(), callback, true, false);
        }
        catch (const DuplicateJsonKey& duplicate) {
            result.diagnostics.push_back(parseError(
                "scene.json.duplicate_key",
                duplicate.what(),
                "/" + escapeJsonPointerToken(duplicate.key())));
        }
        catch (const nlohmann::json::exception& exception) {
            result.diagnostics.push_back(parseError(
                "scene.json.invalid_syntax", exception.what()));
        }
        catch (const std::exception& exception) {
            result.diagnostics.push_back(parseError(
                "scene.json.parser_failure", exception.what()));
        }
        return result;
    }

} // namespace Iridium
