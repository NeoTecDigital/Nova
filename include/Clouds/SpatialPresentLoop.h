// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Option B (plan D.2): Vazio renders into wlroots' own output buffers instead
// of presenting through a swapchain of its own. Per output, on the output's
// `frame` event (wlr_output.h:193-194):
//
//   configure_primary_swapchain -> swapchain_acquire -> Nova renders into the
//   acquired buffer -> output_state_set_buffer -> output_commit_state
//
// Two ways for Nova's pixels to reach that buffer, chosen ONCE per output when
// it is attached and never re-decided per frame:
//
//   DmabufImport   - the acquired wlr_buffer is a DMA-BUF, Nova imports it as a
//                    VkImage and renders straight into scanout storage. Zero
//                    copy. Requires the queue-family-foreign release, which
//                    NovaImportedImage::asRenderTarget sets: a DCC-compressed
//                    radv image read back without it comes out wrong, measured.
//   PixmanSidecar  - the buffer cannot be imported, so this loop runs its own
//                    pixman renderer + allocator + mappable shm swapchain
//                    (spike-verified: an output initialised with gles2 still
//                    accepts those buffers) and Nova's frame is read back and
//                    copied in. One copy, no GPU import.
#pragma once

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#ifdef __cplusplus
}
#endif

#include "./WaylandListener.h"
#include "../../Core/nova_graphics.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Clouds {

class SpatialCompositor;

/**
 * The format both paths fall back to, and the only one the CPU path uses.
 *
 * A single-plane 32-bit BGRA-order buffer: what wl_shm mandates, and what
 * wlroots' allocators negotiate for a primary swapchain on this hardware.
 */
constexpr uint32_t kPresentFallbackDrmFormat = NOVA_DRM_FORMAT_XRGB8888;

enum class PresentPath {
    Undecided,
    DmabufImport,
    PixmanSidecar
};

const char* presentPathName(PresentPath path);

class SpatialPresentLoop {
public:
    /**
     * Draw one frame. The command buffer is already inside a render pass sized
     * to `extent` and cleared; the callback contributes draw calls only.
     */
    using SceneRenderer = std::function<void(VkCommandBuffer cmd, const VkExtent2D& extent)>;

    SpatialPresentLoop(NovaGraphics* graphics, SpatialCompositor* compositor);
    ~SpatialPresentLoop();

    SpatialPresentLoop(const SpatialPresentLoop&) = delete;
    SpatialPresentLoop& operator=(const SpatialPresentLoop&) = delete;

    /**
     * Adopt an enabled, mode-set output and start driving it from its `frame`
     * event. Selects the presentation path on the first output attached; later
     * outputs must resolve to the same one, because the render pass every
     * caller's pipeline was built against is fixed by that choice.
     *
     * renderPass() and targetFormat() are only meaningful after this returns
     * true - build the scene pipeline then, not before.
     */
    bool attach(struct wlr_output* output);

    void setSceneRenderer(SceneRenderer renderer) { scene_renderer_ = std::move(renderer); }

    // The pass renderToImage() will use, and the format it will use it on.
    VkRenderPass renderPass() const { return render_pass_; }
    VkFormat targetFormat() const { return target_format_; }

    PresentPath path() const { return path_; }
    uint64_t commits() const { return commits_; }
    uint64_t failures() const { return failures_; }

    // Detach every listener and release every GPU object. Idempotent, and safe
    // to call before the compositor tears the outputs down - which is the only
    // correct order, since the listeners live on wlr_output signals.
    void stop();

    /**
     * One driven output. Public because WaylandListener dispatches into it;
     * it is otherwise entirely this loop's business.
     */
    struct Output {
        SpatialPresentLoop* loop = nullptr;

        // Null once the wlr_output has been destroyed. The entry outlives that
        // moment because a listener callback may not delete the object whose
        // listener is being dispatched; reapDeadOutputs() erases it afterwards.
        struct wlr_output* output = nullptr;

        // Only used on the PixmanSidecar path: the mappable swapchain this
        // loop allocates for itself, separate from output->swapchain, plus the
        // extent it was created at - the comparison a mode change is detected by.
        struct wlr_swapchain* sidecar_swapchain = nullptr;
        VkExtent2D sidecar_extent = {0, 0};

        // Set from the output's own `commit` when the mode moved the extent.
        // Acted on at the top of the next presentFrame(), never inside the
        // commit dispatch that observed it.
        bool sidecar_needs_rebuild = false;

        // Exactly one LIVE output feeds frame_done, so a second screen cannot
        // double the callbacks every client is waiting on. Re-elected when the
        // holder is destroyed; see electFrameCallbackDriver().
        bool drives_frame_callbacks = false;

        WaylandListener<Output> frame_listener;
        WaylandListener<Output> present_listener;
        WaylandListener<Output> commit_listener;
        WaylandListener<Output> destroy_listener;

        bool live() const { return output != nullptr; }

        ~Output();
        void onFrame(void* data);
        void onPresent(void* data);
        void onCommit(void* data);
        void onDestroy(void* data);
    };

private:
    /**
     * A wlr_buffer imported into Nova, cached for the buffer's lifetime.
     *
     * The swapchain recycles a small fixed set of slots (WLR_SWAPCHAIN_CAP),
     * so importing per frame would import the same four buffers forever. The
     * destroy listener is what makes the cache safe: a swapchain reallocated
     * on a mode change drops its buffers, and the imported VkImages must go
     * with them rather than outlive the fds they were built from.
     */
    struct ImportedBuffer {
        SpatialPresentLoop* loop = nullptr;
        struct wlr_buffer* buffer = nullptr;
        NovaImportedImage image;
        WaylandListener<ImportedBuffer> destroy_listener;

        void onDestroy(void* data);
    };

    // Nova-owned render target plus the host-visible buffer its pixels are read
    // back into. PixmanSidecar only: the imported path renders in place.
    struct SidecarTarget {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        Buffer_T readback;

        // NovaCore::createEphemeralBuffer does not set VMA's MAPPED bit
        // (core_base.cpp:399), so the mapping is this loop's to make and to
        // drop - once, not per frame.
        void* readback_mapped = nullptr;
        VkExtent2D extent = {0, 0};

        bool valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
    };

    bool selectPath(struct wlr_output* output);
    bool probeDmabufImport(struct wlr_output* output);

    // False, with a reason logged, when the CPU path already drives an output.
    // Always true on the import path, which has no shared per-frame buffer.
    bool sidecarSlotFree(struct wlr_output* candidate) const;

    // Erase the bindings whose wlr_output has been destroyed. Called from
    // attach(), which is the one place outside listener dispatch that every
    // hotplug passes through; never from a callback, where erasing would delete
    // the object owning the listener being dispatched.
    void reapDeadOutputs();

    // Give frame_done to the first live output if the holder is gone. Safe to
    // call from a destroy dispatch: it only writes other entries' flags.
    void electFrameCallbackDriver();
    bool hasFrameCallbackDriver() const;

    // Record a geometry change the output committed for itself, for the
    // sidecar path to act on at the next frame.
    void noteOutputCommit(Output& bound, const struct wlr_output_state& state);

    // Nova's VkImage for `buffer`, imported on first sight and cached after.
    const NovaImportedImage* importedImageFor(struct wlr_buffer* buffer);
    void releaseImport(ImportedBuffer* entry);

    bool presentImported(Output& bound, struct wlr_buffer* buffer);
    bool presentSidecar(Output& bound, struct wlr_buffer* buffer);
    bool renderIntoSidecarTarget();
    bool ensureSidecarTarget(VkExtent2D extent);
    bool createSidecarImage(VkExtent2D extent);
    bool ensureSidecar(Output& bound, struct wlr_output* output);

    // (Re)create this output's mappable swapchain and the shared render target
    // at the output's current extent. Also the mode-change recovery path.
    bool createSidecarSwapchain(Output& bound);
    bool rebuildSidecar(Output& bound);
    void destroySidecarTarget();

    // Acquire, render, set_buffer, commit, unlock. One frame, one output.
    void presentFrame(Output& bound);

    NovaGraphics* graphics_ = nullptr;
    SpatialCompositor* compositor_ = nullptr;
    SceneRenderer scene_renderer_;

    PresentPath path_ = PresentPath::Undecided;
    VkFormat target_format_ = VK_FORMAT_UNDEFINED;
    VkImageLayout target_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;

    std::vector<std::unique_ptr<Output>> outputs_;
    std::vector<std::unique_ptr<ImportedBuffer>> imports_;

    // PixmanSidecar only. The renderer and allocator are this loop's, not the
    // compositor's: the output keeps the renderer it was initialised with.
    struct wlr_renderer* sidecar_renderer_ = nullptr;
    struct wlr_allocator* sidecar_allocator_ = nullptr;

    // ONE target for the whole loop, which is why the CPU path drives at most
    // one output (see attach()). Sizing it per output would silently make the
    // per-frame row copy read `sidecar_target_.extent` rows into a buffer
    // belonging to a differently sized screen.
    SidecarTarget sidecar_target_;

    uint64_t commits_ = 0;
    uint64_t failures_ = 0;
};

} // namespace Clouds
