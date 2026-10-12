#pragma once

// BBHOST_GFX_LOG=1 (or 2): a journal of the graphics pipeline, frame by frame,
// to find what makes a frame long or wrong. Off, every hook below costs one
// load of a constant.
//
// The hooks (gfxj::Scope around a call that can stall, gfxj::note for a thing
// that went differently than usual) put events into the frame they happen in;
// the game's flip closes the frame (gfxj::on_flip). Each frame becomes a line
// in logs/gfx-<time>.jsonl (build/ without a logs folder; BBHOST_GFX_LOG_DIR
// names another): how long it was, what the GPU and the draw path did in it
// (draws, failures, draws on the page-table fallback pipeline, pipelines
// made), memory (video memory headroom, system memory and commit, the free
// space of the drive the caches are written to), and its events - per kind how
// many, how long in all and the longest, and each event over
// BBHOST_GFX_EVENT_MS (0.5) singly, with the thread it ran on.
//
//   1  the frames longer than BBHOST_GFX_HITCH_MS (20), the frames with an
//      event that marks a wrong picture (a copy refused, a target made again,
//      a draw that failed), and a line a second for the rest
//   2  every frame
//
// A frame over the hitch limit is also said in the log, one line with the
// events that took its time, longest first.
#include <atomic>
#include <chrono>
#include <cstdint>

namespace gfxj {

enum Kind : std::uint8_t {
    kPipelineGfx,    // a graphics pipeline built where the draw waited for it (ns; a: vertex+pixel hash)
    kPipelineCs,     // a compute pipeline built where the dispatch waited for it
    kFenceWait,      // a wait on a submission's fence (a: submission slot)
    kSubmit,         // vkQueueSubmit
    kMemAlloc,       // vkAllocateMemory (a: bytes, b: memory type)
    kTargetMake,     // a render target or snapshot image made (a: width, b: height)
    kTargetReplaced, // a render target made again: idles the device [marks a wrong picture]
    kCopyRefused,    // a target copy done as memory: the destination's image kept [marks a wrong picture]
    kTextureUpload,  // a texture's data put on the GPU (a: bytes)
    kAcquire,        // the swap chain image acquired
    kPresent,        // vkQueuePresentKHR
    kSwapchain,      // the swap chain made again
    kDlssEval,       // DLSS's evaluation recorded (a: 1 when it reset its history)
    kDlssFail,       // DLSS did not run [marks a wrong picture]
    kVramEvict,      // textures or targets moved out of video memory (a: bytes)
    kGpuSlow,        // a command buffer that ran long on the GPU (ns; tag: its passes)
    kIdleWait,       // a vkQueueWaitIdle or vkDeviceWaitIdle
    kDrawFail,       // a draw that was not drawn (tag: why) [marks a wrong picture]
    kNote,           // anything else worth a line (tag)
    kCount
};

extern const int g_mode;  // BBHOST_GFX_LOG: 0 off
inline bool on() { return g_mode != 0; }

// Books an event of `ns` nanoseconds to the frame under way. `tag` is copied
// (23 characters), so a temporary is fine. Any thread.
void event(Kind kind, std::uint64_t ns, std::uint64_t a = 0, std::uint64_t b = 0, const char* tag = nullptr);

// An event of no duration.
inline void note(Kind kind, std::uint64_t a = 0, std::uint64_t b = 0, const char* tag = nullptr) {
    if (on()) event(kind, 0, a, b, tag);
}

// Times its scope as an event of `kind`.
class Scope {
public:
    explicit Scope(Kind kind, std::uint64_t a = 0, std::uint64_t b = 0, const char* tag = nullptr)
        : kind_(kind), a_(a), b_(b), tag_(tag) {
        if (on()) t0_ = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!on()) return;
        event(kind_, static_cast<std::uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count()),
              a_, b_, tag_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void set_a(std::uint64_t a) { a_ = a; }
    void set_b(std::uint64_t b) { b_ = b; }

private:
    Kind kind_;
    std::uint64_t a_, b_;
    const char* tag_;
    std::chrono::steady_clock::time_point t0_{};
};

// The game's main loop finished a frame of `busy_us` microseconds of its own
// work (the limiter's wait left out): shown in the frame's line as "main".
void main_frame(std::uint64_t busy_us);

// The game's flip: closes the frame. `flip` counts the game's flips so far.
void on_flip(std::uint64_t flip);

// Flushes and closes the file (the process is ending).
void shutdown();

}  // namespace gfxj
