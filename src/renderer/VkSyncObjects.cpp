#include "VkSyncObjects.h"
#include <stdexcept>

VkSyncObjects::VkSyncObjects(VkContext* context) : context(context) {
	createSyncObjects();
}

VkSyncObjects::~VkSyncObjects() {
	vkDestroySemaphore(context->getDevice(), renderFinishedSemaphore, nullptr);
	vkDestroySemaphore(context->getDevice(), imageAvailableSemaphore, nullptr);
	vkDestroyFence(context->getDevice(), inFlightFence, nullptr);
}

void VkSyncObjects::createSyncObjects() {
	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled so we don't wait forever the first frame

	if (vkCreateSemaphore(context->getDevice(), &semaphoreInfo, nullptr, &imageAvailableSemaphore) != VK_SUCCESS ||
		vkCreateSemaphore(context->getDevice(), &semaphoreInfo, nullptr, &renderFinishedSemaphore) != VK_SUCCESS ||
		vkCreateFence(context->getDevice(), &fenceInfo, nullptr, &inFlightFence) != VK_SUCCESS) {
		throw std::runtime_error("failed to create synchronization objects!");
	}
}