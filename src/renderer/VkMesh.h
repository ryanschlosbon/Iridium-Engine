#pragma once
#include <vector>
#include <map>
#include <memory> // Needed for std::unique_ptr
#include <string>
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
	int subMeshIndex;
	glm::mat4 transform; // The pre-calculated offset
};

struct Vertex {
	glm::vec3 pos;
	glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
	glm::vec3 normal;
	glm::vec2 uv;
	glm::vec4 tangent;

	static VkVertexInputBindingDescription getBindingDescription() {
		VkVertexInputBindingDescription bindingDescription{};
		bindingDescription.binding = 0;
		bindingDescription.stride = sizeof(Vertex);
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		return bindingDescription;
	}

	static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions() {
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions(5);

		// Location 0: Position
		attributeDescriptions[0].binding = 0;
		attributeDescriptions[0].location = 0;
		attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[0].offset = offsetof(Vertex, pos);

		// Location 1: Color
		attributeDescriptions[1].binding = 0;
		attributeDescriptions[1].location = 1;
		attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[1].offset = offsetof(Vertex, color);

		// Location 2: Normal
		attributeDescriptions[2].binding = 0;
		attributeDescriptions[2].location = 2;
		attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions[2].offset = offsetof(Vertex, normal);

		// THE CULPRIT: Location 3 MUST specifically target the UV offset with a VEC2 format!
		attributeDescriptions[3].binding = 0;
		attributeDescriptions[3].location = 3;
		attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions[3].offset = offsetof(Vertex, uv);

		// Location 4: Tangent
		attributeDescriptions[4].binding = 0;
		attributeDescriptions[4].location = 4;
		attributeDescriptions[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attributeDescriptions[4].offset = offsetof(Vertex, tangent);

		return attributeDescriptions;
	}
};

struct MeshPushConstants {
	glm::mat4 renderMatrix;
	glm::vec4 baseColor;
	float metallicFactor;
	float roughnessFactor;
	float emissiveFactor;
	float padding;
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

enum class AlphaMode {
	Opaque,
	Mask,
	Blend
};

struct Material {
	glm::vec4 baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f); // MUST default to white!
	float metallicFactor = 1.0f;                             // glTF spec default
	float roughnessFactor = 1.0f;                            // glTF spec default
	float emissiveFactor = 0.0f;
	int albedoTextureIndex = -1;  // Renamed from textureIndex
	int normalTextureIndex = -1;  
	int metallicRoughnessTextureIndex = -1; 
	std::vector<VkDescriptorSet> descriptorSets;

	AlphaMode alphaMode = AlphaMode::Opaque; // Defaults to solid

	VkDescriptorImageInfo albedoInfo{};
	VkDescriptorImageInfo normalInfo{};
	VkDescriptorImageInfo pbrInfo{};
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
		int subMeshIndex;
		glm::mat4 transform;
	};

	std::map<int, std::vector<BakedPart>> materialBuckets;
};

struct RenderObject {
	std::shared_ptr<ModelAsset> model;
	glm::mat4 transform;
	VkDescriptorSet descriptorSet;
};