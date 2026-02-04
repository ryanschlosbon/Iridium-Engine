#pragma once
#include "VkContext.h"

class VkSyncObjects {
public:
	VkSyncObjects(VkContext* context);
	~VkSyncObjects();

	VkSemaphore& getImageAvailableSemaphore() { return imageAvailableSemaphore; }
	VkSemaphore& getRenderFinishedSemaphore()  { return renderFinishedSemaphore; }
	VkFence& getInFlightFence() { return inFlightFence; }

private:
	VkContext* context;

	VkSemaphore imageAvailableSemaphore;	// Swapchain -> Graphics queue
	VkSemaphore renderFinishedSemaphore;	// Graphics queue -> Present queue
	VkFence inFlightFence;				// GPU -> CPU (Frame limiting)

	void createSyncObjects();
};