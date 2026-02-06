#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <array>

struct Vertex {
	glm::vec3 pos;
	glm::vec3 color;

	// Binding Description:
	// Describes at which rate to load data from memory throughout the vertices.
	static VkVertexInputBindingDescription getBindingDescription() {
		VkVertexInputBindingDescription bindingDescription{};
		bindingDescription.binding = 0; // Index of the binding in the array of bindings
		bindingDescription.stride = sizeof(Vertex); // Number of bytes from one entry to the next
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX; // Move to the next data entry after each vertex
		return { bindingDescription };
	}

	// Attribute Descriptions:
	// Describes how to extract a vertex attribute from a chunk of vertex data (vec2 at offset 0, vec3 at offset 8)
	static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions() {
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions(2);

		// Position attribute
		attributeDescriptions[0].binding = 0; // Which binding the per-vertex data comes.
		attributeDescriptions[0].location = 0; // Location directive of the input in the vertex shader
		attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3 
		attributeDescriptions[0].offset = offsetof(Vertex, pos); // Offset of the attribute within the struct
		
		// Color attribute
		attributeDescriptions[1].binding = 0;
		attributeDescriptions[1].location = 1; // Location 1 in the shader
		attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3
		attributeDescriptions[1].offset = offsetof(Vertex, color);
		
		return attributeDescriptions;
	}
};

struct MeshPushConstants {
	glm::vec2 offset;
	glm::vec2 scale;
};

struct UniformBufferObject {
	alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};