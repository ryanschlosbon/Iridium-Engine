#include "assets/cooker/CookTypes.h"

#include <algorithm>

namespace Iridium {

    bool hasCookErrors(const std::vector<CookDiagnostic>& diagnostics) noexcept {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const CookDiagnostic& diagnostic) {
                return diagnostic.severity == CookDiagnosticSeverity::Error;
            });
    }

} // namespace Iridium
