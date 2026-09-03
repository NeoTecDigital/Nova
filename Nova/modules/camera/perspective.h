#pragma once
#include <glm/glm.hpp>
namespace Nova {
// Used to switch between different camera perspectives
enum CameraPerspective {
    FIRST_PERSON,
    THIRD_PERSON_BEHIND,
    THIRD_PERSON_ABOVE
};

class Perspective {
    public:
        Perspective();
        ~Perspective();

        void togglePerspective();
        glm::vec3 getOffset(glm::mat4);

    private:
        CameraPerspective perspective;
};

} // namespace Nova
