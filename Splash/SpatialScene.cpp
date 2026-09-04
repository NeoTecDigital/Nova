// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./SpatialScene.h"
#include "./Primitives.h"
#include "Splash/content/mesh_generators.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace Splash {
namespace {

// World-space half-extents handed to the shader for each reticle.
constexpr float LOOKAT_RETICLE_EXTENT = 0.12f;
constexpr float CURSOR_RETICLE_EXTENT = 0.10f;

// Orientation that keeps a flat mesh facing the camera.
glm::mat4 billboardRotation(const glm::vec3& camera_pos,
                            const glm::vec3& camera_target,
                            const glm::vec3& camera_up) {
    const glm::vec3 cam_dir = glm::normalize(camera_pos - camera_target);
    const glm::vec3 cam_right = glm::normalize(glm::cross(camera_up, cam_dir));
    const glm::vec3 cam_actual_up = glm::cross(cam_dir, cam_right);

    return glm::mat4(
        glm::vec4(cam_right, 0.0f),
        glm::vec4(cam_actual_up, 0.0f),
        glm::vec4(cam_dir, 0.0f),
        glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
    );
}

void appendReticle(Nova::SpatialMeshBuffer* mesh_buffer,
                   const Nova::MeshData& mesh,
                   const glm::mat4& model,
                   float extent,
                   std::vector<SpatialRenderCommand>& out_commands) {
    if (mesh.vertices.empty()) return;

    uint32_t first_index = 0;
    uint32_t index_count = 0;
    mesh_buffer->append(mesh, first_index, index_count);

    // Zero means the mesh buffer dropped this mesh to stay inside the frame
    // slot's capacity; emitting a command for it would draw nothing.
    if (index_count == 0) return;

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(extent, extent, 1.0f, 0.0f),
        .texture = nullptr,
        .first_index = first_index,
        .index_count = index_count
    });
}

void recordDrawCommands(Nova::SpatialPipeline* pipeline,
                        Nova::SpatialMeshBuffer* mesh_buffer,
                        VkCommandBuffer cmd,
                        const std::vector<SpatialRenderCommand>& commands,
                        const glm::mat4& view_proj,
                        const glm::vec3& camera_pos,
                        VkDescriptorSet fallback_texture) {
    for (const SpatialRenderCommand& rcmd : commands) {
        VkDescriptorSet tex = rcmd.texture ? rcmd.texture->descriptor_set : fallback_texture;
        pipeline->bindTexture(cmd, tex);

        const Nova::SpatialPushConstants push = {
            .view_proj = view_proj,
            .model = rcmd.model,
            .camera_pos = glm::vec4(camera_pos, 1.0f),
            .surface_dim = rcmd.surface_dim
        };
        pipeline->pushConstants(cmd, push);
        mesh_buffer->draw(cmd, rcmd.first_index, rcmd.index_count);
    }
}


// Normalised surface coordinate for a point already expressed in a node's
// local frame. Same convention as intersectOrientedQuad: (0,0) top-left.
glm::vec2 surfaceUv(const glm::vec2& size, const glm::vec3& local_point) {
    const float half_w = size.x * 0.5f;
    const float half_h = size.y * 0.5f;
    const float u = size.x > 0.0f ? (local_point.x + half_w) / size.x : 0.0f;
    const float v = size.y > 0.0f ? (half_h - local_point.y) / size.y : 0.0f;
    return glm::clamp(glm::vec2(u, v), glm::vec2(0.0f), glm::vec2(1.0f));
}

// Unbounded intersection of a ray with a node's surface plane.
// intersectOrientedQuad answers "did the pointer land on this quad"; this
// answers "where on this node's surface does the pointer correspond to", which
// is what a capture redirect and a drag past the quad's edge both need. uv is
// still clamped, so a hosted surface never receives an off-surface coordinate.
bool projectRayOntoNodePlane(Registry& reg,
                             NodeId node,
                             const Nova::Math::Ray3D& ray,
                             Nova::Math::RayHit& out_hit) {
    if (!reg.alive(node)) return false;

    const Nova::Math::QuatTransform world_xf = reg.worldOf(node);
    const glm::vec3 local_orig = world_xf.inverseTransformPoint(ray.origin);
    const glm::vec3 local_dir = glm::normalize(glm::conjugate(world_xf.orientation) * ray.direction);

    if (std::abs(local_dir.z) < 1e-6f) return false;   // ray runs along the plane

    const float t = -local_orig.z / local_dir.z;
    if (t < 0.0f) return false;                        // plane is behind the pointer

    const glm::vec3 local_hit = local_orig + local_dir * t;

    out_hit.hit = true;
    out_hit.distance = t;
    out_hit.local_point = local_hit;
    out_hit.world_point = world_xf.transformPoint(local_hit);
    out_hit.normal = world_xf.orientation * glm::vec3(0.0f, 0.0f, local_dir.z < 0.0f ? 1.0f : -1.0f);
    out_hit.uv = surfaceUv(reg[node].size, local_hit);
    return true;
}

// Scoped capture. A hit on a node that does not claim pointer input belongs to
// its innermost capturing ancestor -- the window, not its titlebar panel. A
// node that claims is always its own target, which is what keeps a button
// inside a capturing window clickable.
NodeId resolveInputTarget(Registry& reg, NodeId hit_node) {
    if (!reg.alive(hit_node) || reg[hit_node].claims_pointer_input) return hit_node;

    for (NodeId owner = reg.parentOf(hit_node); owner.valid(); owner = reg.parentOf(owner)) {
        if (reg[owner].captures_subtree_input) return owner;
    }
    return hit_node;
}

} // namespace

SpatialScene::SpatialScene(Registry& registry, Nova::Core* core, Nova::TextureBridge* texture_bridge)
    : registry_(registry), core_(core), texture_bridge_(texture_bridge) {
    root = registry_.createContainer(INVALID_NODE);
    registry_[root].name = "SceneRoot";
}

SpatialScene::~SpatialScene() {
    // mesh_buffer_ and font own Vulkan/VMA resources that in-flight command
    // buffers may still reference. Implicit member destruction would free them
    // under the GPU, so drain the device first and release them explicitly
    // while the wait is still in scope.
    if (core_ != nullptr && core_->getDevice() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(core_->getDevice());
    }

    mesh_buffer_.reset();

    // Consumers before the producer. Nodes hold shared_ptr copies of the font's
    // atlas texture; ~SpatialFont hands that atlas back to the bridge, so this
    // scene's tree is destroyed here - immediately, not deferred - and no node
    // is left naming a texture whose handles have just been nulled. The
    // registry outlives the scene, so this is the only place that knows the
    // subtree is going.
    pointer_grab_ = INVALID_NODE;
    pointer_focus_ = INVALID_NODE;
    keyboard_focus_ = INVALID_NODE;
    registry_.destroy(root);
    registry_.drain();
    root = INVALID_NODE;

    font.reset();
}

void SpatialScene::initialize(const std::string& font_path) {
    mesh_buffer_ = std::make_unique<Nova::SpatialMeshBuffer>(core_, 65536, 131072);

    font = std::make_shared<Splash::SpatialFont>(core_, texture_bridge_);
    font->loadFromFile(font_path, 48);

    // Lookat Reticle: Green circle around a dark grey crosshair
    lookat_reticle_mesh_ = Splash::SpatialMeshGenerator::createReticle(
        glm::vec4(0.12f, 0.92f, 0.35f, 0.95f), // Green ring
        glm::vec4(0.22f, 0.25f, 0.28f, 0.95f), // Dark grey crosshair
        0.055f, 0.005f, 0.075f, 0.004f
    );

    // Cursor Reticle: Blue circle around a red crosshair
    cursor_reticle_mesh_ = Splash::SpatialMeshGenerator::createReticle(
        glm::vec4(0.15f, 0.60f, 1.0f, 0.95f),  // Blue ring
        glm::vec4(0.95f, 0.20f, 0.25f, 0.95f), // Red crosshair
        0.045f, 0.005f, 0.065f, 0.004f
    );
}

glm::mat4 SpatialScene::getViewMatrix() const {
    return glm::lookAt(camera_pos, camera_target, camera_up);
}

glm::mat4 SpatialScene::getProjectionMatrix(float aspect_ratio) const {
    glm::mat4 proj = glm::perspective(fov_radians, aspect_ratio, near_plane, far_plane);
    proj[1][1] *= -1.0f; // Vulkan Y-flip
    return proj;
}

void SpatialScene::dropDeadReferences() {
    if (!registry_.alive(pointer_focus_)) pointer_focus_ = INVALID_NODE;
    if (!registry_.alive(keyboard_focus_)) keyboard_focus_ = INVALID_NODE;
    if (!registry_.alive(pointer_grab_)) {
        pointer_grab_ = INVALID_NODE;
        pressed_button_mask_ = 0;
    }
}

Nova::Math::Ray3D SpatialScene::buildPointerRay(const glm::vec2& screen_pixel, const glm::vec2& screen_size) {
    const float aspect = screen_size.x / std::max(screen_size.y, 1.0f);
    const glm::mat4 inv_view_proj = glm::inverse(getProjectionMatrix(aspect) * getViewMatrix());

    // Apply InputRayFilter pipeline with precision & sub-pixel dithering
    return input_filter.filterScreenRay(screen_pixel, screen_size, inv_view_proj, physics_config, frame_index_);
}

bool SpatialScene::castPointerRay(const Nova::Math::Ray3D& world_ray,
                                  Nova::Math::RayHit& out_hit,
                                  NodeId& out_target) {
    Nova::Math::RayHit hit;
    NodeId hit_node;

    // Focused laser check first if LaserFocus mode is active
    if (physics_config.accel_mode == Nova::Math::AccelerationMode::LaserFocus &&
        keyboard_focus_.valid()) {
        Splash::hitTest(registry_, keyboard_focus_, world_ray, hit, hit_node);
    }

    if (!hit_node.valid()) {
        Splash::hitTest(registry_, root, world_ray, hit, hit_node);
    }
    if (!hit_node.valid()) return false;

    // Geometry named the node; scoped capture names the target. When they
    // differ the hit is re-cast onto the target's own plane, so a redirected
    // press and every motion after it are measured in one frame -- the band
    // test and the drag delta both depend on that.
    out_target = resolveInputTarget(registry_, hit_node);
    if (out_target != hit_node) {
        projectRayOntoNodePlane(registry_, out_target, world_ray, hit);
    }
    out_hit = hit;
    return true;
}

void SpatialScene::setPointerFocus(NodeId node, const Nova::Math::RayHit& enter_hit) {
    if (pointer_focus_ == node) return;

    if (registry_.alive(pointer_focus_)) {
        registry_[pointer_focus_].onRayLeave(registry_, pointer_focus_);
    }
    pointer_focus_ = node;
    if (registry_.alive(pointer_focus_)) {
        registry_[pointer_focus_].onRayEnter(registry_, pointer_focus_, enter_hit);
    }
}

void SpatialScene::setKeyboardFocus(NodeId node) {
    if (keyboard_focus_ == node) {
        if (registry_.alive(keyboard_focus_)) registry_[keyboard_focus_].is_focused = true;
        return;
    }

    // is_focused is a property of the transfer, not of the click that started
    // it: onRayButton raises it on whatever was pressed, and only the node that
    // ends up holding focus may keep it. Clearing the previous holder here is
    // what keeps exactly one node flagged at a time.
    if (registry_.alive(keyboard_focus_)) {
        registry_[keyboard_focus_].is_focused = false;
    }
    keyboard_focus_ = node;
    if (registry_.alive(keyboard_focus_)) {
        registry_[keyboard_focus_].is_focused = true;
    }
}

void SpatialScene::releaseNode(NodeId node) {
    if (!node.valid()) return;

    if (pointer_grab_ == node) {
        releasePointer();
    }
    if (pointer_focus_ == node) {
        // The leave edge is owed even here: the node is losing the pointer, and
        // whatever it does on leave is the last thing it is entitled to do.
        if (registry_.alive(node)) registry_[node].onRayLeave(registry_, node);
        pointer_focus_ = INVALID_NODE;
    }
    if (keyboard_focus_ == node) {
        if (registry_.alive(node)) registry_[node].is_focused = false;
        keyboard_focus_ = INVALID_NODE;
    }
}

void SpatialScene::grabPointer(NodeId node) {
    pointer_grab_ = node;
}

void SpatialScene::releasePointer() {
    pointer_grab_ = INVALID_NODE;
    pressed_button_mask_ = 0;
}

void SpatialScene::updateHover(const Nova::Math::Ray3D& world_ray) {
    Nova::Math::RayHit hit;
    NodeId target;

    if (!castPointerRay(world_ray, hit, target)) {
        setPointerFocus(INVALID_NODE, hit);
        placeCursorOnMissedRay(world_ray);
        return;
    }

    if (target == pointer_focus_) {
        registry_[pointer_focus_].onRayMove(registry_, pointer_focus_, hit);
    } else {
        setPointerFocus(target, hit);
    }

    last_hit_ = hit;
    cursor_3d_pos = hit.world_point + hit.normal * 0.008f;
    physics_config.last_ray_depth = hit.distance;
}

void SpatialScene::deliverGrabMotion(const Nova::Math::Ray3D& world_ray) {
    // The grabbed node's plane, not its quad: a drag that has left the quad
    // still has a well-defined position on the surface it started from. The
    // plane is invariant under the drag it drives -- the delta it produces
    // lies inside it -- so this does not feed back on itself.
    Nova::Math::RayHit hit = last_hit_;
    if (projectRayOntoNodePlane(registry_, pointer_grab_, world_ray, hit)) {
        last_hit_ = hit;
        cursor_3d_pos = hit.world_point + hit.normal * 0.008f;
        physics_config.last_ray_depth = hit.distance;
    }
    registry_[pointer_grab_].onRayMove(registry_, pointer_grab_, last_hit_);
}

void SpatialScene::placeCursorOnMissedRay(const Nova::Math::Ray3D& world_ray) {
    // Unprojected point on the view-aligned plane through camera_target.
    const glm::vec3 cam_fwd = glm::normalize(camera_target - camera_pos);
    const float denom = glm::dot(world_ray.direction, cam_fwd);

    float t = 2.5f;
    if (std::abs(denom) > 1e-4f) {
        const float plane_t = glm::dot(camera_target - world_ray.origin, cam_fwd) / denom;
        if (plane_t > 0.05f) t = plane_t;
    }
    cursor_3d_pos = world_ray.origin + world_ray.direction * t;
}

void SpatialScene::processPointerMotion(const glm::vec2& screen_pixel, const glm::vec2& screen_size) {
    dropDeadReferences();
    last_screen_pixel_ = screen_pixel;
    last_screen_size_ = screen_size;

    const Nova::Math::Ray3D world_ray = buildPointerRay(screen_pixel, screen_size);
    last_pointer_ray_ = world_ray;
    has_pointer_sample_ = true;

    // A grab owns the pointer outright: hover neither moves nor is re-evaluated
    // until the last button comes up.
    if (pointer_grab_.valid()) {
        deliverGrabMotion(world_ray);
        return;
    }
    updateHover(world_ray);
}

void SpatialScene::processPointerButton(uint32_t button, bool pressed) {
    dropDeadReferences();
    const uint32_t button_bit = 1u << (button & 31u);

    if (pressed) {
        pressed_button_mask_ |= button_bit;
        if (!pointer_grab_.valid()) grabPointer(pointer_focus_);
    } else {
        pressed_button_mask_ &= ~button_bit;
    }

    // The grab, when one is held, outranks hover: that is the whole point of
    // it, and it is what delivers a release to the node that took the press.
    const NodeId target = pointer_grab_.valid() ? pointer_grab_ : pointer_focus_;
    if (registry_.alive(target)) {
        registry_[target].onRayButton(registry_, target, last_hit_, button, pressed);
    }

    // Keyboard focus follows activation, so it survives the pointer moving on.
    if (pressed) {
        setKeyboardFocus(target);
        return;
    }

    if (pressed_button_mask_ != 0) return;

    releasePointer();
    if (has_pointer_sample_) {
        updateHover(last_pointer_ray_);   // hover was frozen for the grab's duration
    }
}

void SpatialScene::processKey(uint32_t key, bool pressed) {
    dropDeadReferences();
    if (registry_.alive(keyboard_focus_)) {
        registry_[keyboard_focus_].onKey(registry_, keyboard_focus_, key, pressed);
    }
}

void SpatialScene::update(float dt) {
    dropDeadReferences();
    frame_index_++;

    // Calculate real-time FPS metric
    fps_accumulator_ += dt;
    fps_frames_++;
    if (fps_accumulator_ >= 0.5f) {
        physics_config.current_fps = static_cast<float>(fps_frames_) / fps_accumulator_;
        fps_accumulator_ = 0.0f;
        fps_frames_ = 0;
    }

    // Evolve non-linear phase physics. Per-frame animation is driven by the
    // owners that hold their own lists, not by a walk of the whole tree.
    if (registry_.alive(root) && physics_config.phase_coupling_strength > 0.0f) {
        registry_[root].evolvePhase(dt, physics_config.phase_coupling_strength);
    }
}

void SpatialScene::render(Nova::SpatialPipeline* pipeline, VkCommandBuffer cmd, const glm::vec2& screen_size) {
    if (!registry_.alive(root) || !pipeline || !mesh_buffer_) return;

    const float aspect = screen_size.x / std::max(screen_size.y, 1.0f);
    const glm::mat4 view_proj = getProjectionMatrix(aspect) * getViewMatrix();

    // 1. Open this frame's buffer slot. Selects the frame-in-flight index the
    //    graphics core is recording, applies any growth deferred from an earlier
    //    frame (safe here: renderFrame() already waited on this slot's fence),
    //    and clears the CPU staging arrays.
    mesh_buffer_->beginFrame();

    // 2. Collect all render commands & geometry across the scene graph
    std::vector<SpatialRenderCommand> commands;
    Splash::collectSubtree(registry_, root, mesh_buffer_.get(), commands);

    // 3. Append the camera-facing reticles: lookat (green ring, grey crosshair)
    //    and 3D cursor (blue ring, red crosshair).
    const glm::mat4 billboard_rot = billboardRotation(camera_pos, camera_target, camera_up);

    if (show_lookat_reticle) {
        appendReticle(mesh_buffer_.get(), lookat_reticle_mesh_,
                      glm::translate(glm::mat4(1.0f), camera_target) * billboard_rot,
                      LOOKAT_RETICLE_EXTENT, commands);
    }

    if (show_cursor_reticle) {
        appendReticle(mesh_buffer_.get(), cursor_reticle_mesh_,
                      glm::translate(glm::mat4(1.0f), cursor_3d_pos) * billboard_rot,
                      CURSOR_RETICLE_EXTENT, commands);
    }

    // 4. Upload unified vertex and index data to GPU in ONE single operation.
    //    Called unconditionally: upload() also settles the deferred-growth
    //    bookkeeping, and a frame whose very first mesh overflowed produces no
    //    drawable geometry yet must still schedule the resize that unblocks it.
    mesh_buffer_->upload();

    if (commands.empty() || mesh_buffer_->getIndexCount() == 0) return;

    // 5. Bind graphics pipeline and this slot's mesh buffers, then dispatch.
    pipeline->bind(cmd);
    mesh_buffer_->bind(cmd);

    recordDrawCommands(pipeline, mesh_buffer_.get(), cmd, commands, view_proj, camera_pos,
                       texture_bridge_->getFallbackTexture()->descriptor_set);
}

} // namespace Splash
