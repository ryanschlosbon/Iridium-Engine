#include <vector>
#include <memory>
#include "renderer/rhi/Mesh.h"
#include "ecs/Entity.h"

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
    std::shared_ptr<Iridium::ModelAsset> model; // The specific car/box/plane
    glm::mat4 transform;               // Where it is
    Entity entity;
};
