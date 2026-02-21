#pragma once

#include "VkContext.h"
#include "VkFramebuffer.h"
#include "VkGraphicsPipeline.h"
#include "VkMesh.h"
#include "editor/EditorSystem.h"
#include "ecs/Registry.h"
#include "scene/Components.h"
#include "VkUIRenderPass.h"

class EditorSystem;

struct RenderPacket {
	SubMesh* subMesh;
	Material* material;
	ModelAsset* model;
	glm::mat4 transform;
	uint32_t materialId; // Used for sorting
};

class VkCommandManager {
public: 
	VkCommandManager(VkContext* context, VkFramebufferWrapper* framebuffer,
		VkGraphicsPipeline* pipeline, uint32_t count);
	~VkCommandManager();

	// The function that writes the commands into the command buffer
	void recordCommands(
		uint32_t currentFrame,
		uint32_t imageIndex,
		VkRenderPassWrapper* offscreenPass,
		VkFramebufferWrapper* offscreenFramebuffer,
		VkUIRenderPass* uiPass,                           // NEW
		const std::vector<VkFramebuffer>& uiFramebuffers, // NEW
		VkGraphicsPipeline* pipeline,
		VkExtent2D extent,
		Registry& registry,
		VkDescriptorSet globalDescriptorSet, 
		EditorSystem* editor);

	void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

	// Getter
	VkCommandBuffer& getCommandBuffer(int index)  { return commandBuffers[index]; }
	VkCommandPool getCommandPool() const { return commandPool; }
	VkCommandBuffer beginSingleTimeCommands();
	void endSingleTimeCommands(VkCommandBuffer commandBuffer);

private:
	VkContext* context;
	std::vector<VkCommandBuffer> commandBuffers;
	VkCommandPool commandPool;
	std::vector<RenderPacket> drawList;

	void createCommandBuffers(int count);

};