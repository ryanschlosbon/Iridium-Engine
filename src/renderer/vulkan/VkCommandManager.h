#pragma once

#include "VkContext.h"
#include "VkFramebuffer.h"
#include "VkGraphicsPipeline.h"
#include "VkLightingPipeline.h"
#include "renderer/rhi/Mesh.h"
#include "editor/EditorSystem.h"
#include "ecs/Registry.h"
#include "scene/Components.h"
#include "VkUIRenderPass.h"
#include "VkForwardRenderPass.h" 
#include "VkForwardPipeline.h"
#include "GlassDepthPipeline.h"

class VkRenderPassWrapper;
class VkFramebufferWrapper;
class VkGraphicsPipeline;
class VkLightingPipeline;
class VkForwardRenderPass;
class VkForwardPipeline;

class EditorSystem;

struct RenderPacket {
	Iridium::SubMesh* subMesh;
	Iridium::Material* material;
	Iridium::ModelAsset* model;
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
		VkRenderPass lightingRenderPass,
		VkFramebuffer lightingFramebuffer,
		VkLightingPipeline* lightingPipeline,
		VkDescriptorSet lightingDescriptorSet,
		glm::vec3 cameraPos,
		glm::mat4 view,
		glm::mat4 proj,
		VkUIRenderPass* uiPass,
		const std::vector<VkFramebuffer>& uiFramebuffers,
		VkGraphicsPipeline* pipeline,
		VkExtent2D extent,
		Registry& registry,
		VkDescriptorSet globalDescriptorSet,
		EditorSystem* editor,
		VkForwardRenderPass* forwardPass,
		VkForwardPipeline* forwardPipeline,
		const std::vector<VkFramebuffer>& forwardFramebuffers,
		VkImage litSceneImage,
		VkImage opaqueSceneCopy,
		VkRenderPass glassDepthRenderPass,
		VkFramebuffer glassDepthFramebuffer, 
		Iridium::GlassDepthPipeline* glassDepthPipeline);

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