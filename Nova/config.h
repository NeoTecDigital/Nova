// VkExtent2D, and nothing more. This used to reach for the deprecated core.h,
// which pulled the whole NovaCoreLegacy declaration in for one Vulkan typedef.
#include <vulkan/vulkan.h>
#include <string>
namespace Nova {
struct Config {
    std::string name;
    VkExtent2D screen;
    std::string debug_level;     // none, release, staging, development, debug
    std::string dimensions;      // 2D, 3D (Not Implemented)             // TODO: Defaults to 3D presently. make 2d.
    std::string camera_type;     // fps, orbit, fixed (Not Implemented)  // TODO: Map this to MVP/Camera. set to orbit.
    bool compute;                // True, False (Not Implemented)        // TODO: Defaults to True presently. make togglable, and later add Sparse support.

    // Shader paths for graphics pipeline (optional - only needed if graphics is used)
    std::string vert_shader_path = "";
    std::string frag_shader_path = "";
};

} // namespace Nova
