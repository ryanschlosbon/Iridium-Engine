#include "scene/authoring/SourceJsonParser.h"

#include <array>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    bool validJsonProducesOrderedSemanticDom() {
        const auto result = Iridium::parseSourceJsonStrict(
            R"({"format":"iridium.scene","schemaVersion":1,"entities":[]})");
        CHECK(result);
        CHECK(result.diagnostics.empty());
        CHECK(result.value->is_object());
        CHECK(result.value->at("format") == "iridium.scene");
        CHECK(result.value->at("entities").empty());
        return true;
    }

    bool duplicateKeysAreHardErrorsAtEveryDepth() {
        for (const std::string input : {
                std::string(R"({"name":"A","name":"B"})"),
                std::string(R"({"outer":{"value":1,"value":2}})"),
                std::string(R"([{"id":1,"id":2}])"),
            }) {
            const auto result = Iridium::parseSourceJsonStrict(input);
            CHECK(!result);
            CHECK(!result.value);
            CHECK(result.diagnostics.size() == 1);
            CHECK(result.diagnostics.front().code == "scene.json.duplicate_key");
            CHECK(result.diagnostics.front().phase == Iridium::ScenePhase::Parse);
        }
        return true;
    }

    bool bomAndSizeLimitAreRejectedBeforeParsing() {
        const std::string bom =
            std::string("\xef\xbb\xbf") + R"({"value":1})";
        const auto bomResult = Iridium::parseSourceJsonStrict(bom);
        CHECK(!bomResult);
        CHECK(bomResult.diagnostics.front().code ==
            "scene.json.bom_not_allowed");

        const auto sizeResult = Iridium::parseSourceJsonStrict(
            R"({"value":1})", { .maximumBytes = 4 });
        CHECK(!sizeResult);
        CHECK(sizeResult.diagnostics.front().code == "scene.json.size_limit");
        return true;
    }

    bool invalidSyntaxCommentsAndTrailingDataAreRejected() {
        for (const std::string input : {
                std::string(R"({"value":})"),
                std::string(R"({"value":1} trailing)"),
                std::string("{/* comment */\"value\":1}"),
                std::string(R"({"value":1e9999})"),
            }) {
            const auto result = Iridium::parseSourceJsonStrict(input);
            CHECK(!result);
            CHECK(!result.value);
            CHECK(result.diagnostics.size() == 1);
            CHECK(result.diagnostics.front().code == "scene.json.invalid_syntax");
        }
        return true;
    }

    bool duplicatePointerTokensAreEscaped() {
        const auto result = Iridium::parseSourceJsonStrict(
            R"({"a/b~c":1,"a/b~c":2})");
        CHECK(!result);
        CHECK(result.diagnostics.front().propertyPath == "/a~1b~0c");
        return true;
    }

    bool wideObjectsRetainDuplicateDetection() {
        const auto unique = Iridium::parseSourceJsonStrict(
            R"({"k00":0,"k01":1,"k02":2,"k03":3,"k04":4,"k05":5,"k06":6,"k07":7,"k08":8,"k09":9,"k10":10,"k11":11,"k12":12})");
        CHECK(unique);
        CHECK(unique.value->size() == 13);

        const auto duplicate = Iridium::parseSourceJsonStrict(
            R"({"k00":0,"k01":1,"k02":2,"k03":3,"k04":4,"k05":5,"k06":6,"k07":7,"k08":8,"k09":9,"k10":10,"k11":11,"k12":12,"k00":13})");
        CHECK(!duplicate);
        CHECK(!duplicate.value);
        CHECK(duplicate.diagnostics.size() == 1);
        CHECK(duplicate.diagnostics.front().code == "scene.json.duplicate_key");
        CHECK(duplicate.diagnostics.front().propertyPath == "/k00");
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    const std::array tests{
        TestCase{ "valid ordered DOM", validJsonProducesOrderedSemanticDom },
        TestCase{ "duplicate key rejection", duplicateKeysAreHardErrorsAtEveryDepth },
        TestCase{ "BOM and size rejection", bomAndSizeLimitAreRejectedBeforeParsing },
        TestCase{ "invalid syntax rejection", invalidSyntaxCommentsAndTrailingDataAreRejected },
        TestCase{ "JSON pointer escaping", duplicatePointerTokensAreEscaped },
        TestCase{ "wide object duplicate rejection", wideObjectsRetainDuplicateDetection },
    };

    size_t passed = 0;
    for (const TestCase& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        ++passed;
        std::cout << "[PASS] " << test.name << '\n';
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return 0;
}
