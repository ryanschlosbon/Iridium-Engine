#include "core/BuildFeatures.h"

static_assert(Iridium::kQualificationBuild,
    "iridium_qualification is only built when IRIDIUM_QUALIFICATION=ON");
