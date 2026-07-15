#pragma once
#include "VkContext.h"
#include <vector>

class VkSyncObjects {
public:
    VkSyncObjects(VkContext* context, uint32_t imageCount);
    ~VkSyncObjects();

    // We define this constant here so the rest of the engine can read it
    static const int MAX_FRAMES_IN_FLIGHT = 2;

    // Getters now take an index (0 or 1)
    VkSemaphore getImageAvailableSemaphore(int frame) { return imageAvailableSemaphores[frame]; }
    VkSemaphore getRenderFinishedSemaphore(int frame) { return renderFinishedSemaphores[frame]; }
    VkFence getInFlightFence(int frame) { return inFlightFences[frame]; }

private:
    VkContext* context;

    // Vectors instead of single handles
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
};