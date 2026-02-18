#pragma once
#include <vector>
#include <map>
#include <memory> // Needed for std::unique_ptr
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

// FORWARD DECLARATION: Tells the compiler "A struct named Node exists, trust me."
struct Node;

// 1. Define SubMesh FIRST (because ModelAsset uses it)
struct SubMesh {
	uint32_t indexStart;
	uint32_t indexCount;
	int materialIndex;
};

// 2. Define BakedMesh SECOND (because ModelAsset uses it)
struct BakedMesh {
	int meshIndex;
	glm::mat4 transform; // The pre-calculated offset
};

struct Vertex {
	glm::vec3 pos;
	glm::vec3 color;
	glm::vec3 normal;
	glm::vec2 uv;

	static VkVertexInputBindingDescription getBindingDescription() {
		VkVertexInputBindingDescription bindingDescription{};
		bindingDescription.binding = 0;
		bindingDescription.stride = sizeof(Vertex);
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		return { bindingDescription };
	}

	static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions() {
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions(4);

		// Position
		attributeDescriptions[0].binding = 0;
		attributeDescriptions[0].location = 0;
		attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[0].offset = offsetof(Vertex, pos);

		// Color
		attributeDescriptions[1].binding = 0;
		attributeDescriptions[1].location = 1;
		attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[1].offset = offsetof(Vertex, color);

		// Normal
		attributeDescriptions[2].binding = 0;
		attributeDescriptions[2].location = 2;
		attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[2].offset = offsetof(Vertex, normal);

		// UV
		attributeDescriptions[3].binding = 0;
		attributeDescriptions[3].location = 3;
		attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions[3].offset = offsetof(Vertex, uv);

		return attributeDescriptions;
	}
};

struct MeshPushConstants {
	glm::mat4 renderMatrix;
};

struct UniformBufferObject {
	alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};

struct Texture {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkSampler sampler;
};

struct Material {
	glm::vec4 baseColor;
	int textureIndex;
	std::vector<VkDescriptorSet> descriptorSets;
};

// 3. NOW define ModelAsset (Since it knows what SubMesh and BakedMesh are)
struct ModelAsset {
	std::string filePath;

	// GPU Buffers
	VkBuffer vertexBuffer;
	VkDeviceMemory vertexBufferMemory;
	VkBuffer indexBuffer;
	VkDeviceMemory indexBufferMemory;

	// Data for rendering
	uint32_t totalIndices;
	std::vector<SubMesh> subMeshes;             // Works now!
	std::map<int, std::vector<int>> meshToSubMeshes;
	std::vector<Material> materials;
	std::vector<Texture> textures;

	// Hierarchy
	std::vector<std::unique_ptr<Node>> rootNodes;

	struct BakedPart {
		int meshIndex;
		glm::mat4 transform;
	};

	std::map<int, std::vector<BakedPart>> materialBuckets;
};

struct RenderObject {
	std::shared_ptr<ModelAsset> model;
	glm::mat4 transform;
	VkDescriptorSet descriptorSet;
};