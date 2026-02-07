#include <vector>
#include <memory>
#include "renderer/VkMesh.h"
#include "Entity.h"

class Scene {
public:
    void addEntity(Entity e) {
        entities.push_back(e);
    }

    std::vector<Entity>& getEntities() {
        return entities;
    }

private:
    std::vector<Entity> entities;
    std::vector<RenderObject> renderQueue;
};

struct RenderObject {
    std::shared_ptr<ModelAsset> model; // The specific car/box/plane
    glm::mat4 transform;               // Where it is
    uint32_t entityID;
};