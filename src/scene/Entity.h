#pragma once
#include <cstdint>

// In ECS, an Entity is JUST an ID. 
// We don't use a class because the data lives in the Registry pools.
using Entity = uint32_t;
constexpr Entity NULL_ENTITY = ~0u;