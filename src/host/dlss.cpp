// NVIDIA DLSS, as anti-aliasing at the render size (DLAA), in place of the
// game's own edge filter.
//
// Where it runs. The scene colour DLSS needs - lit, before the HUD and the
// tone map - is the one YEBIS takes in, and YEBIS's motion blur already needs
// what DLSS needs alongside it: the depth, and every pixel's motion from the
// last frame. Its velocity pass (7ea47480+d3c8bb21) rebuilds each pixel's
// clip position from the depth snapshot, reprojects it with a clip-to-
// previous-clip matrix the game hands it this frame, and blends in the
// characters' velocity map (drawn at half size by the scene renderer). Just
// after that pass, the colour has had depth of field composited into it and
// is about to be blurred; DLSS goes there:
//
//   1. motion vectors at the render size from that pass's own constants
//      (shaders/dlss_mv.comp - the pass without the blur's scale and clamp);
//   2. DLSS on the scene colour (the DoF composite's target, ...+111fce32),
//      the depth snapshot and those vectors, into an image of our own;
//   3. its colour copied back over the scene colour, keeping the alpha the
//      motion blur reads (shaders/dlss_merge.comp).
//
// The rest of the frame - motion blur, bloom, the tone map, the HUD - runs on
// the result as it would on the game's own.
//
// The sub-pixel detail DLSS accumulates comes from a jitter the game never
// had: every draw of the scene into its depth buffer has its viewport moved
// by a Halton (2, 3) offset that changes each frame. Full-screen passes are
// left where they are - they read the jittered buffers pixel for pixel - and
// so is the HUD, which draws without a depth test.
//
// NGX itself. Its core is the driver's own (_nvngx.dll, from the registry
// key the driver installs), which exports the API by name; the static library
// NVIDIA's SDK links does no more than find it and forward. So bbhost loads
// it at run time, with declarations of its own for the handful of calls and
// structures it uses, and links no NVIDIA code. The DLSS model itself,
// nvngx_dlss.dll, comes from NVIDIA's SDK or the driver's own updates; it
// goes next to bbhost.exe. NGX's parameter maps are C++ objects built by
// MSVC, so their methods are called through the vtable in MSVC's order
// (overloads reversed), checked once with a value written and read back.
//
// Off unless asked for: BBHOST_DLSS=1 (or the setting). BBHOST_DLSS_DEBUG=mv
// shows the motion vectors in place of the picture; BBHOST_DLSS_JITTER=0
// keeps the viewport still (DLSS then only smooths).
#include "host/gpu_internal.h"
#include "host/options.h"
#include "host/settings.h"
#include "host/shaders/dlss_merge.spv.h"
#include "host/shaders/dlss_mv.spv.h"
#include "core/config.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gpu {
namespace {

// ---- NGX, as much of it as is used --------------------------------------------
namespace ngx {

using Result = std::uint32_t;
inline bool ok(Result r) { return (r & 0xfff00000u) != 0xbad00000u; }

struct Parameter;
struct Handle;

constexpr int kVersionApi = 0x15;     // NVSDK_NGX_Version_API, SDK 310
constexpr int kFeatureSuperSampling = 1;
constexpr int kPerfQualityDlaa = 5;   // NVSDK_NGX_PerfQuality_Value_DLAA
constexpr int kFlagIsHdr = 1 << 0, kFlagMvLowRes = 1 << 1, kFlagDepthInverted = 1 << 3, kFlagAutoExposure = 1 << 6;
constexpr int kEngineCustom = 0;
constexpr int kIdentifierProjectId = 1;
constexpr int kLoggingOn = 1;
constexpr int kResourceImageView = 0;

struct PathListInfo {
    const wchar_t* const* path;
    unsigned length;
};
using LogCallback = void (*)(const char* message, int level, int feature);
struct LoggingInfo {
    LogCallback callback;
    int minimum_level;
    bool disable_other_sinks;
};
struct FeatureCommonInfo {
    PathListInfo paths;
    void* internal;
    LoggingInfo logging;
};
struct ProjectIdDescription {
    const char* project_id;
    int engine_type;
    const char* engine_version;
};
struct ApplicationIdentifier {
    int type;
    union {
        ProjectIdDescription project;
        unsigned long long application_id;
    } v;
};
struct FeatureDiscoveryInfo {
    int sdk_version;
    int feature;
    ApplicationIdentifier identifier;
    const wchar_t* data_path;
    const FeatureCommonInfo* info;
};
struct FeatureRequirement {
    int supported;  // 0 when it is; else why not (bits)
    unsigned min_hw_architecture;
    char min_os_version[255];
};
struct ImageViewInfoVk {
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
};
struct BufferInfoVk {
    VkBuffer buffer;
    unsigned bytes;
};
struct ResourceVk {
    union {
        ImageViewInfoVk image;
        BufferInfoVk buffer;
    } resource;
    int type;
    bool read_write;
};

// The core's exports, by the signatures it is called with.
using PfnInstanceExtensions = Result (*)(const FeatureDiscoveryInfo*, std::uint32_t*, VkExtensionProperties**);
using PfnDeviceExtensions = Result (*)(VkInstance, VkPhysicalDevice, const FeatureDiscoveryInfo*, std::uint32_t*,
                                       VkExtensionProperties**);
using PfnFeatureRequirements = Result (*)(VkInstance, VkPhysicalDevice, const FeatureDiscoveryInfo*, FeatureRequirement*);
// The core's own Init_ProjectID takes no loader entry points (its _Ext does),
// and the SDK version before the common info - read off its prologue, as the
// SDK's headers declare the static library's wrappers, not these.
using PfnInitProjectId = Result (*)(const char*, int, const char*, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, int,
                                    const FeatureCommonInfo*);
using PfnShutdown1 = Result (*)(VkDevice);
using PfnParameters = Result (*)(Parameter**);
using PfnDestroyParameters = Result (*)(Parameter*);
using PfnCreateFeature1 = Result (*)(VkDevice, VkCommandBuffer, int, Parameter*, Handle**);
using PfnEvaluateFeature = Result (*)(VkCommandBuffer, const Handle*, const Parameter*, void*);
using PfnReleaseFeature = Result (*)(Handle*);

// NVSDK_NGX_Parameter's methods by vtable slot. MSVC groups a virtual
// function's overloads and lays them out last-declared first; Itanium (the
// Linux core) keeps declaration order.
enum Slot { kSetPtr, kSetD3d12, kSetD3d11, kSetI, kSetUi, kSetD, kSetF, kSetUll, kGetPtr, kGetD3d12, kGetD3d11, kGetI, kGetUi, kGetD,
            kGetF, kGetUll, kSlots };
#if defined(_WIN32)
constexpr int kSlotOf[kSlots] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
#else
constexpr int kSlotOf[kSlots] = {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8};
#endif
template <typename F>
F method(Parameter* p, Slot s) {
    return reinterpret_cast<F>((*reinterpret_cast<void***>(p))[kSlotOf[s]]);
}
void set_ptr(Parameter* p, const char* name, void* v) { method<void (*)(Parameter*, const char*, void*)>(p, kSetPtr)(p, name, v); }
void set_i(Parameter* p, const char* name, int v) { method<void (*)(Parameter*, const char*, int)>(p, kSetI)(p, name, v); }
void set_ui(Parameter* p, const char* name, unsigned v) { method<void (*)(Parameter*, const char*, unsigned)>(p, kSetUi)(p, name, v); }
void set_f(Parameter* p, const char* name, float v) { method<void (*)(Parameter*, const char*, float)>(p, kSetF)(p, name, v); }
Result get_i(Parameter* p, const char* name, int* v) { return method<Result (*)(Parameter*, const char*, int*)>(p, kGetI)(p, name, v); }
Result get_ui(Parameter* p, const char* name, unsigned* v) {
    return method<Result (*)(Parameter*, const char*, unsigned*)>(p, kGetUi)(p, name, v);
}
Result get_f(Parameter* p, const char* name, float* v) { return method<Result (*)(Parameter*, const char*, float*)>(p, kGetF)(p, name, v); }

}  // namespace ngx

// bbhost's project id for NGX (a GUID of its own; NGX asks one of every
// application that is not on NVIDIA's list).
constexpr const char* kProjectId = "5b0d3a1e-8c2f-4e57-9d61-b8b0b10d0b09";

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct Pass {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct MvPush {
    std::uint32_t width, height;
    std::uint32_t bias;        // the dword the pass's constants start at in their binding
    std::uint32_t use_camera;  // reproject from the two frames' cameras rather than the pass's matrix
    std::uint32_t cur, prev;   // their blocks in the camera ring, in floats
};
// The scene constants' dwords copied each frame: 180..215, the camera's
// position at 183, 187, 191 and the camera-relative view-projection at 200.
constexpr std::uint32_t kCameraFirstDw = 180, kCameraDwords = 36;
struct MergePush {
    std::uint32_t width, height, mode;
};

struct Dlss {
    // The core, from the first extension query on.
    bool loaded = false;
#if defined(_WIN32)
    HMODULE lib = nullptr;
#else
    void* lib = nullptr;
#endif
    ngx::PfnInstanceExtensions instance_extensions = nullptr;
    ngx::PfnDeviceExtensions device_extensions = nullptr;
    ngx::PfnFeatureRequirements feature_requirements = nullptr;
    ngx::PfnInitProjectId init = nullptr;
    ngx::PfnShutdown1 shutdown = nullptr;
    ngx::PfnParameters capability_parameters = nullptr, allocate_parameters = nullptr;
    ngx::PfnDestroyParameters destroy_parameters = nullptr;
    ngx::PfnCreateFeature1 create_feature = nullptr;
    ngx::PfnEvaluateFeature evaluate = nullptr;
    ngx::PfnReleaseFeature release = nullptr;
    bool extensions_ok = false;  // what the device needs was enabled

    // NGX up and DLSS available, from the first frame that wanted it.
    bool tried = false, ok = false;
    ngx::Parameter* params = nullptr;
    ngx::Handle* feature = nullptr;
    std::uint32_t width = 0, height = 0;
    int flags = 0;
    // A feature replaced (a size change) is released once the frames that
    // used it have run.
    std::vector<std::pair<ngx::Handle*, std::uint64_t>> retired;
    Image motion, result;
    // The characters' velocity map as the scene renderer drew it, copied
    // before the motion blur's post-pass widens it in place (raw_valid: this
    // frame's copy is there).
    Image raw_velocity;
    std::uint32_t raw_width = 0, raw_height = 0;
    VkFormat raw_format = VK_FORMAT_UNDEFINED;
    bool raw_valid = false;
    // The scene's camera, copied on the GPU from the first G-buffer draw's
    // constants each frame into one of two blocks: `camera_now` says this
    // frame's is there (in block camera_block), `camera_last` that the frame
    // before's is in the other.
    DevBuffer camera;
    std::uint32_t camera_block = 0;
    bool camera_now = false, camera_last = false;
    Pass mv, merge;
    VkSampler linear = VK_NULL_HANDLE, point = VK_NULL_HANDLE;

    // The frame.
    std::uint64_t scene_colour = 0;  // the DoF composite's target this frame
    std::map<std::uint64_t, std::uint64_t> depth_of_snapshot;  // snapshot base -> the depth target it copies
    std::uint64_t scene_depth = 0;   // the depth target the jitter applies to
    std::uint32_t phase = 0;         // into the Halton sequence
    float jitter[2] = {};            // this frame's, in pixels
    std::uint64_t evaluations = 0;
    std::uint64_t last_flip = 0;     // hle_video_flip_count() at the last evaluation
    bool reset = true;
    std::uint64_t jittered = 0, logged_at = 0;
} g_dlss;

std::wstring widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));  // the paths it is given are ASCII
    return w;
}

// Where nvngx_dlss.dll is looked for (with the driver's own updates).
const std::wstring& feature_dir() {
    static const std::wstring dir = widen(config_exe_dir());
    return dir;
}
const wchar_t* const* feature_paths() {
    static const wchar_t* paths[1] = {feature_dir().c_str()};
    return paths;
}

void log_callback(const char* message, int, int) {
    if (!message) return;
    std::string m = message;
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    host_log("dlss: ngx: %s", m.c_str());
}

ngx::FeatureCommonInfo common_info() {
    ngx::FeatureCommonInfo info{};
    info.paths.path = feature_paths();
    info.paths.length = 1;
    info.logging.callback = &log_callback;
    info.logging.minimum_level = ngx::kLoggingOn;
    info.logging.disable_other_sinks = true;
    return info;
}

ngx::FeatureDiscoveryInfo discovery(const ngx::FeatureCommonInfo* info) {
    ngx::FeatureDiscoveryInfo d{};
    d.sdk_version = ngx::kVersionApi;
    d.feature = ngx::kFeatureSuperSampling;
    d.identifier.type = ngx::kIdentifierProjectId;
    d.identifier.v.project = {kProjectId, ngx::kEngineCustom, "1.0"};
    d.data_path = feature_dir().c_str();
    d.info = info;
    return d;
}

bool env_wanted() {
    static const int v = [] {
        const char* e = std::getenv("BBHOST_DLSS");
        return e && e[0] ? (e[0] == '0' ? 0 : 1) : -1;
    }();
    return v == 1;
}
bool env_refused() {
    const char* e = std::getenv("BBHOST_DLSS");
    return e && e[0] == '0';
}

// The setting, or BBHOST_DLSS=1.
bool wanted() {
    static std::uint64_t seen = ~0ull;
    static bool on = false;
    const std::uint64_t serial = host_opt_serial();
    if (serial != seen) {
        seen = serial;
        on = env_wanted() || (!env_refused() && host_settings().dlss);
    }
    return on;
}

template <typename F>
F symbol(const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(g_dlss.lib, name)));
#else
    return reinterpret_cast<F>(dlsym(g_dlss.lib, name));
#endif
}

// The driver's NGX core: from the folder its registry key names (as the
// SDK's loader finds it), or next to bbhost.exe.
bool load_core() {
    if (g_dlss.loaded) return g_dlss.lib != nullptr;
    g_dlss.loaded = true;
    if (env_refused()) return false;
#if defined(_WIN32)
    std::vector<std::wstring> tries;
    for (const wchar_t* key : {L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
                               L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore"}) {
        wchar_t path[MAX_PATH] = {};
        DWORD bytes = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"NGXPath", RRF_RT_REG_SZ, nullptr, path, &bytes) == ERROR_SUCCESS && path[0]) {
            tries.push_back(std::wstring(path) + L"\\_nvngx.dll");
        }
    }
    {
        wchar_t path[MAX_PATH] = {};
        DWORD bytes = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, path,
                         &bytes) == ERROR_SUCCESS && path[0]) {
            tries.push_back(std::wstring(path) + L"\\_nvngx.dll");
            tries.push_back(std::wstring(path) + L"\\nvngx.dll");
        }
    }
    tries.push_back(feature_dir() + L"\\_nvngx.dll");
    for (const std::wstring& p : tries) {
        g_dlss.lib = LoadLibraryW(p.c_str());
        if (g_dlss.lib) break;
    }
#else
    g_dlss.lib = dlopen("libnvidia-ngx.so.1", RTLD_NOW);
#endif
    if (!g_dlss.lib) {
        host_log("dlss: no NGX core (not an NVIDIA driver, or one without it): DLSS unavailable");
        return false;
    }
    g_dlss.instance_extensions = symbol<ngx::PfnInstanceExtensions>("NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");
    g_dlss.device_extensions = symbol<ngx::PfnDeviceExtensions>("NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
    g_dlss.feature_requirements = symbol<ngx::PfnFeatureRequirements>("NVSDK_NGX_VULKAN_GetFeatureRequirements");
    g_dlss.init = symbol<ngx::PfnInitProjectId>("NVSDK_NGX_VULKAN_Init_ProjectID");
    g_dlss.shutdown = symbol<ngx::PfnShutdown1>("NVSDK_NGX_VULKAN_Shutdown1");
    g_dlss.capability_parameters = symbol<ngx::PfnParameters>("NVSDK_NGX_VULKAN_GetCapabilityParameters");
    g_dlss.allocate_parameters = symbol<ngx::PfnParameters>("NVSDK_NGX_VULKAN_AllocateParameters");
    g_dlss.destroy_parameters = symbol<ngx::PfnDestroyParameters>("NVSDK_NGX_VULKAN_DestroyParameters");
    g_dlss.create_feature = symbol<ngx::PfnCreateFeature1>("NVSDK_NGX_VULKAN_CreateFeature1");
    g_dlss.evaluate = symbol<ngx::PfnEvaluateFeature>("NVSDK_NGX_VULKAN_EvaluateFeature");
    g_dlss.release = symbol<ngx::PfnReleaseFeature>("NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!g_dlss.instance_extensions || !g_dlss.device_extensions || !g_dlss.init || !g_dlss.capability_parameters ||
        !g_dlss.allocate_parameters || !g_dlss.create_feature || !g_dlss.evaluate || !g_dlss.release) {
        host_log("dlss: the NGX core lacks the Vulkan entry points: DLSS unavailable");
#if defined(_WIN32)
        FreeLibrary(g_dlss.lib);
#else
        dlclose(g_dlss.lib);
#endif
        g_dlss.lib = nullptr;
        return false;
    }
    return true;
}

// Adds what NGX asks for that `available` has and `exts` lacks; false when
// something it asks for is not there.
bool add_extensions(std::vector<const char*>& exts, const VkExtensionProperties* want, std::uint32_t n,
                    const std::vector<VkExtensionProperties>& available, const char* what) {
    static std::vector<std::string> names;  // what `exts` points into
    names.reserve(64);
    bool all = true;
    std::string added;
    for (std::uint32_t i = 0; i < n; ++i) {
        const char* name = want[i].extensionName;
        bool have = false;
        for (const char* e : exts) have = have || std::strcmp(e, name) == 0;
        if (have) continue;
        bool there = false;
        for (const VkExtensionProperties& a : available) there = there || std::strcmp(a.extensionName, name) == 0;
        if (!there) {
            host_log("dlss: NGX asks for %s extension %s, which is not available", what, name);
            all = false;
            continue;
        }
        if (names.size() == names.capacity()) continue;  // never in practice: the pointers must not move
        names.emplace_back(name);
        exts.push_back(names.back().c_str());
        added += added.empty() ? name : std::string(", ") + name;
    }
    if (!added.empty()) host_log("dlss: %s extensions for NGX: %s", what, added.c_str());
    return all;
}

void destroy_image(Image& im) {
    if (im.view) vkDestroyImageView(g.device, im.view, nullptr);
    if (im.image) vkDestroyImage(g.device, im.image, nullptr);
    if (im.memory) vkFreeMemory(g.device, im.memory, nullptr);
    im = Image{};
}

bool make_image(Image& im, VkFormat format, std::uint32_t w, std::uint32_t h) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g.device, &ici, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g.device, im.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.device, &mai, nullptr, &im.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(g.device, im.image, im.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(g.device, &vci, nullptr, &im.view) == VK_SUCCESS;
}

// Three sampled or sampler bindings and a storage image, a push block.
bool make_pass(Pass& p, const std::uint32_t* code, std::size_t bytes, const VkDescriptorType* types, std::uint32_t n,
               std::uint32_t push_bytes) {
    VkDescriptorSetLayoutBinding binds[6] = {};
    for (std::uint32_t i = 0; i < n; ++i) binds[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = n;
    sli.pBindings = binds;
    if (vkCreateDescriptorSetLayout(g.device, &sli, nullptr, &p.set_layout) != VK_SUCCESS) return false;
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(g.device, &pli, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = bytes;
    smi.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g.device, &smi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = p.layout;
    const bool made = vkCreateComputePipelines(g.device, g.cache, 1, &ci, nullptr, &p.pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(g.device, module, nullptr);
    return made;
}

VkSampler make_sampler(VkFilter filter) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = filter;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler s = VK_NULL_HANDLE;
    return vkCreateSampler(g.device, &sci, nullptr, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

// The parameter map's methods, through the vtable: a value written must read back.
bool parameters_answer(ngx::Parameter* p) {
    ngx::set_ui(p, "bbhost.check.ui", 0x5a5a1234u);
    ngx::set_f(p, "bbhost.check.f", 2.5f);
    unsigned u = 0;
    float f = 0.0f;
    return ngx::ok(ngx::get_ui(p, "bbhost.check.ui", &u)) && u == 0x5a5a1234u && ngx::ok(ngx::get_f(p, "bbhost.check.f", &f)) &&
           f == 2.5f;
}

bool ngx_init_locked() {
    if (!load_core() || !g_dlss.extensions_ok) {
        host_log("dlss: unavailable (%s)", g_dlss.lib ? "the device lacks an extension NGX needs" : "no NGX core");
        return false;
    }
    const ngx::FeatureCommonInfo info = common_info();
    if (g_dlss.feature_requirements) {
        const ngx::FeatureDiscoveryInfo d = discovery(&info);
        ngx::FeatureRequirement req{};
        const ngx::Result r = g_dlss.feature_requirements(g.instance, g.phys, &d, &req);
        if (ngx::ok(r) && req.supported != 0) {
            host_log("dlss: unavailable on this GPU or driver (NGX says 0x%x; needs architecture 0x%x, OS %s)", req.supported,
                     req.min_hw_architecture, req.min_os_version);
            return false;
        }
    }
    const ngx::Result r = g_dlss.init(kProjectId, ngx::kEngineCustom, "1.0", feature_dir().c_str(), g.instance, g.phys, g.device,
                                      ngx::kVersionApi, &info);
    if (!ngx::ok(r)) {
        host_log("dlss: NGX init failed (0x%x)", r);
        return false;
    }
    ngx::Parameter* caps = nullptr;
    if (!ngx::ok(g_dlss.capability_parameters(&caps)) || !caps) {
        host_log("dlss: NGX gave no capability parameters");
        return false;
    }
    if (!parameters_answer(caps)) {
        host_log("dlss: NGX's parameter map does not answer through the vtable as laid out here: DLSS off");
        return false;
    }
    int available = 0, needs_driver = 0, init_result = 0;
    unsigned major = 0, minor = 0;
    ngx::get_i(caps, "SuperSampling.Available", &available);
    ngx::get_i(caps, "SuperSampling.NeedsUpdatedDriver", &needs_driver);
    ngx::get_ui(caps, "SuperSampling.MinDriverVersionMajor", &major);
    ngx::get_ui(caps, "SuperSampling.MinDriverVersionMinor", &minor);
    ngx::get_i(caps, "SuperSampling.FeatureInitResult", &init_result);
    if (g_dlss.destroy_parameters) g_dlss.destroy_parameters(caps);
    if (!available) {
        host_log("dlss: DLSS not available (%s; feature init 0x%x) - nvngx_dlss.dll belongs next to bbhost.exe (get-dlss.bat downloads it from NVIDIA)",
                 needs_driver ? "the driver is too old" : "no model found", static_cast<unsigned>(init_result));
        if (needs_driver) host_log("dlss: it needs driver %u.%u or newer", major, minor);
        return false;
    }
    if (!ngx::ok(g_dlss.allocate_parameters(&g_dlss.params)) || !g_dlss.params) {
        host_log("dlss: NGX gave no parameter map");
        return false;
    }
    static const VkDescriptorType mv_types[6] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    if (!create_dev_buffer(g_dlss.camera, 2 * kCameraDwords * 4, false)) {
        host_log("dlss: no memory for the camera blocks");
        return false;
    }
    if (!make_pass(g_dlss.mv, k_dlss_mv_spv, sizeof(k_dlss_mv_spv), mv_types, 6, sizeof(MvPush)) ||
        !make_pass(g_dlss.merge, k_dlss_merge_spv, sizeof(k_dlss_merge_spv), mv_types, 4, sizeof(MergePush))) {
        host_log("dlss: its own passes could not be made");
        return false;
    }
    g_dlss.linear = make_sampler(VK_FILTER_LINEAR);
    g_dlss.point = make_sampler(VK_FILTER_NEAREST);
    return g_dlss.linear && g_dlss.point;
}

// The feature and our two images for a frame of w x h; false when they cannot be made.
bool size_feature_locked(VkCommandBuffer cmd, std::uint32_t w, std::uint32_t h, int flags) {
    if (g_dlss.feature && w == g_dlss.width && h == g_dlss.height && flags == g_dlss.flags) return true;
    if (g_dlss.feature) g_dlss.retired.emplace_back(g_dlss.feature, g_dlss.evaluations + kSlots + 2);
    g_dlss.feature = nullptr;
    // Ours can go once nothing in flight uses them (they are a frame's worth).
    if (g_dlss.motion.image) defer_destroy_image(g_dlss.motion.image, g_dlss.motion.memory), defer_destroy_view(g_dlss.motion.view);
    if (g_dlss.result.image) defer_destroy_image(g_dlss.result.image, g_dlss.result.memory), defer_destroy_view(g_dlss.result.view);
    g_dlss.motion = Image{};
    g_dlss.result = Image{};
    g_dlss.width = g_dlss.height = 0;
    if (!make_image(g_dlss.motion, VK_FORMAT_R16G16_SFLOAT, w, h) || !make_image(g_dlss.result, VK_FORMAT_R16G16B16A16_SFLOAT, w, h)) {
        destroy_image(g_dlss.motion);
        destroy_image(g_dlss.result);
        host_log("dlss: no memory for its %ux%u images", w, h);
        return false;
    }
    ngx::Parameter* p = g_dlss.params;
    ngx::set_ui(p, "CreationNodeMask", 1);
    ngx::set_ui(p, "VisibilityNodeMask", 1);
    ngx::set_ui(p, "Width", w);
    ngx::set_ui(p, "Height", h);
    ngx::set_ui(p, "OutWidth", w);
    ngx::set_ui(p, "OutHeight", h);
    ngx::set_i(p, "PerfQualityValue", ngx::kPerfQualityDlaa);
    ngx::set_i(p, "DLSS.Feature.Create.Flags", flags);
    ngx::set_i(p, "DLSS.Enable.Output.Subrects", 0);
    const ngx::Result r = g_dlss.create_feature(g.device, cmd, ngx::kFeatureSuperSampling, p, &g_dlss.feature);
    if (!ngx::ok(r) || !g_dlss.feature) {
        host_log("dlss: the DLSS feature could not be made at %ux%u (0x%x)", w, h, r);
        g_dlss.feature = nullptr;
        return false;
    }
    g_dlss.width = w;
    g_dlss.height = h;
    g_dlss.flags = flags;
    g_dlss.reset = true;
    // The images start undefined; they live in GENERAL.
    VkImageMemoryBarrier b[2] = {};
    const VkImage images[2] = {g_dlss.motion.image, g_dlss.result.image};
    for (int i = 0; i < 2; ++i) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = images[i];
        b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    host_log("dlss: DLAA at %ux%u (flags 0x%x)", w, h, flags);
    return true;
}

void release_retired_locked() {
    for (std::size_t i = 0; i < g_dlss.retired.size();) {
        if (g_dlss.evaluations >= g_dlss.retired[i].second) {
            g_dlss.release(g_dlss.retired[i].first);
            g_dlss.retired.erase(g_dlss.retired.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

float halton(std::uint32_t i, std::uint32_t base) {
    float f = 1.0f, r = 0.0f;
    for (; i; i /= base) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
    }
    return r;
}

void advance_jitter() {
    static const bool still = [] {
        const char* e = std::getenv("BBHOST_DLSS_JITTER");
        return e && e[0] == '0';
    }();
    // DLAA: 8 phases (NVIDIA's guide: 8 times the scale factor squared).
    g_dlss.phase = (g_dlss.phase % 8) + 1;
    g_dlss.jitter[0] = still ? 0.0f : halton(g_dlss.phase, 2) - 0.5f;
    g_dlss.jitter[1] = still ? 0.0f : halton(g_dlss.phase, 3) - 0.5f;
}

VkImageView view_of(VkImage image, VkFormat format) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g.device, &vci, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    defer_destroy_private_view(v);
    return v;
}

bool write_set(VkDescriptorSet set, VkImageView a, VkImageView b, VkSampler s, VkImageView storage) {
    if (!set || !a || !b || !storage) return false;
    const VkDescriptorImageInfo ia{VK_NULL_HANDLE, a, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo ib{VK_NULL_HANDLE, b, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo is{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo io{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo* infos[4] = {&ia, &ib, &is, &io};
    const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkWriteDescriptorSet w[4] = {};
    for (std::uint32_t k = 0; k < 4; ++k) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = set;
        w[k].dstBinding = k;
        w[k].descriptorCount = 1;
        w[k].descriptorType = types[k];
        w[k].pImageInfo = infos[k];
    }
    vkUpdateDescriptorSets(g.device, 4, w, 0, nullptr);
    return true;
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags src, VkPipelineStageFlags to, VkAccessFlags dst) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src;
    mb.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

ngx::ResourceVk resource(VkImageView view, VkImage image, VkFormat format, std::uint32_t w, std::uint32_t h, bool rw) {
    ngx::ResourceVk r{};
    r.resource.image = {view, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, format, w, h};
    r.type = ngx::kResourceImageView;
    r.read_write = rw;
    return r;
}

float dword_float(const std::uint32_t* c, std::uint32_t i) {
    float f = 0.0f;
    std::memcpy(&f, c + i, 4);
    return f;
}

}  // namespace

void dlss_instance_extensions(std::vector<const char*>& exts) {
    if (!load_core()) return;
    const ngx::FeatureCommonInfo info = common_info();
    const ngx::FeatureDiscoveryInfo d = discovery(&info);
    std::uint32_t n = 0;
    VkExtensionProperties* want = nullptr;
    if (!ngx::ok(g_dlss.instance_extensions(&d, &n, &want))) return;  // the driver's core does not implement it: none
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    add_extensions(exts, want, n, available, "instance");
}

void dlss_device_extensions(std::vector<const char*>& exts) {
    if (!g_dlss.lib) return;
    const ngx::FeatureCommonInfo info = common_info();
    const ngx::FeatureDiscoveryInfo d = discovery(&info);
    std::uint32_t n = 0;
    VkExtensionProperties* want = nullptr;
    if (!ngx::ok(g_dlss.device_extensions(g.instance, g.phys, &d, &n, &want))) {
        host_log("dlss: NGX did not say which device extensions it needs");
        return;
    }
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &count, available.data());
    g_dlss.extensions_ok = add_extensions(exts, want, n, available, "device");
}

void dlss_note_depth_snapshot_locked(std::uint64_t depth_base, std::uint64_t snapshot_base) {
    g_dlss.depth_of_snapshot[snapshot_base] = depth_base;
}

void dlss_note_scene_colour_locked(std::uint64_t base) { g_dlss.scene_colour = base; }

void dlss_note_velocity_post_locked(RtImage* map) {
    if (!g_dlss.ok || g_dlss.raw_valid || !map || !map->initialised || map->depth) return;
    if (map->width != g_dlss.raw_width || map->height != g_dlss.raw_height || map->format != g_dlss.raw_format) {
        if (g_dlss.raw_velocity.image) {
            defer_destroy_image(g_dlss.raw_velocity.image, g_dlss.raw_velocity.memory);
            defer_destroy_view(g_dlss.raw_velocity.view);
        }
        g_dlss.raw_velocity = Image{};
        g_dlss.raw_width = g_dlss.raw_height = 0;
        if (!make_image(g_dlss.raw_velocity, map->format, map->width, map->height)) {
            destroy_image(g_dlss.raw_velocity);
            return;
        }
        g_dlss.raw_width = map->width;
        g_dlss.raw_height = map->height;
        g_dlss.raw_format = map->format;
        begin_recording_locked();
        render_end_pass_locked();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g_dlss.raw_velocity.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(g_cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    begin_recording_locked();
    render_end_pass_locked();
    VkCommandBuffer cmd = g_cmd();
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {map->width, map->height, 1};
    vkCmdCopyImage(cmd, map->image, VK_IMAGE_LAYOUT_GENERAL, g_dlss.raw_velocity.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    g_dlss.raw_valid = true;
}

bool dlss_wants_camera_locked(std::uint64_t depth_base) {
    return g_dlss.ok && !g_dlss.camera_now && depth_base && depth_base == g_dlss.scene_depth && g_dlss.camera.buffer;
}

void dlss_note_camera_locked(const VkDescriptorBufferInfo& binding, std::uint32_t bias_dw) {
    const VkDeviceSize from = binding.offset + (static_cast<VkDeviceSize>(bias_dw) + kCameraFirstDw) * 4;
    if (!binding.buffer || (binding.range != VK_WHOLE_SIZE && binding.range < (static_cast<VkDeviceSize>(bias_dw) + kCameraFirstDw + kCameraDwords) * 4)) {
        return;
    }
    // A copy cannot be recorded inside the pass the draw is in; the next draw opens another.
    begin_recording_locked();
    render_end_pass_locked();
    VkCommandBuffer cmd = g_cmd();
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferCopy region{from, static_cast<VkDeviceSize>(g_dlss.camera_block) * kCameraDwords * 4, kCameraDwords * 4};
    vkCmdCopyBuffer(cmd, binding.buffer, g_dlss.camera.buffer, 1, &region);
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    g_dlss.camera_now = true;
}

bool dlss_jitter_locked(std::uint64_t depth_base, bool depth_test, std::uint32_t prim, std::uint32_t count, float* dx, float* dy) {
    if (!g_dlss.ok || !depth_base || depth_base != g_dlss.scene_depth || !depth_test) return false;
    if (prim == 6 && count <= 4) return false;  // a full-screen strip: it reads the scene pixel for pixel
    // Only while DLSS has seen a recent frame: a jittered picture nothing
    // resolves would shake.
    if (hle_video_flip_count() > g_dlss.last_flip + 4) return false;
    *dx = g_dlss.jitter[0];
    *dy = g_dlss.jitter[1];
    ++g_dlss.jittered;
    return true;
}

void dlss_after_velocity_locked(const DlssVelocityPass& in) {
    if (!wanted()) {
        g_dlss.scene_colour = 0;
        return;
    }
    if (!g_dlss.tried) {
        g_dlss.tried = true;
        g_dlss.ok = ngx_init_locked();
        host_log("dlss: %s", g_dlss.ok ? "ready" : "off for this run");
    }
    const std::uint64_t colour_base = g_dlss.scene_colour;
    g_dlss.scene_colour = 0;
    // The undilated map, when the motion blur's post-pass ran this frame (and
    // so widened the one the velocity pass reads).
    const bool raw_velocity = g_dlss.raw_valid && g_dlss.raw_width == (in.object_velocity ? in.object_velocity->width : 0) &&
                              g_dlss.raw_height == (in.object_velocity ? in.object_velocity->height : 0);
    g_dlss.raw_valid = false;
    // Why a velocity pass went by without DLSS: each reason the first time.
    auto skip = [&](const char* why) {
        static std::vector<std::string> said;
        if (std::find(said.begin(), said.end(), why) != said.end()) return;
        said.emplace_back(why);
        host_log("dlss: a velocity pass without DLSS: %s (flip %llu)", why, static_cast<unsigned long long>(hle_video_flip_count()));
    };
    if (!g_dlss.ok) return;
    if (!colour_base) return skip("no depth-of-field composite before it");
    if (!in.depth_snapshot || !in.object_velocity || !in.constants || in.constant_dwords < 912) return skip("its inputs did not resolve");
    RtImage* colour = find_render_target(colour_base);
    if (!colour || !colour->initialised || colour->depth || colour->format != VK_FORMAT_R16G16B16A16_SFLOAT) return skip("the colour is not an RGBA16F target");
    const std::uint32_t w = colour->width, h = colour->height;
    if (in.depth_snapshot->width != w || in.depth_snapshot->height != h || !in.depth_snapshot->initialised ||
        !in.object_velocity->initialised) {
        char why[96];
        std::snprintf(why, sizeof(why), "the depth snapshot is %ux%u, the colour %ux%u", in.depth_snapshot->width,
                      in.depth_snapshot->height, w, h);
        return skip(why);
    }
    const std::uint32_t* c = in.constants;
    // The depth runs which way: clip w at z = 0 against z = 1.
    auto clip_w = [&](float z) {
        const float d = std::clamp(dword_float(c, 620) * (dword_float(c, 0) * z + dword_float(c, 1)) + dword_float(c, 621), 0.0f, 1.0f);
        return -dword_float(c, 62) / (d * dword_float(c, 63) - dword_float(c, 61));
    };
    const bool inverted = clip_w(0.0f) > clip_w(1.0f);
    const int flags = ngx::kFlagIsHdr | ngx::kFlagMvLowRes | ngx::kFlagAutoExposure | (inverted ? ngx::kFlagDepthInverted : 0);

    begin_recording_locked();
    render_end_pass_locked();
    VkCommandBuffer cmd = g_cmd();
    release_retired_locked();
    if (!size_feature_locked(cmd, w, h, flags)) {
        g_dlss.ok = false;
        host_log("dlss: off for this run");
        return;
    }
    if (const auto it = g_dlss.depth_of_snapshot.find(in.depth_snapshot->base); it != g_dlss.depth_of_snapshot.end()) {
        g_dlss.scene_depth = it->second;
    }
    // Every write so far is visible to what follows.
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    // 1. Motion vectors.
    // From the constants as the pass's shader binds them: the game hands them
    // over in a dynamic buffer whose bytes reach their place on the GPU just
    // before the pass runs, and the CPU's view of that memory when the command
    // processor gets here can be an earlier frame's - in real time, now and
    // then, and the picture combed wherever the motion was a frame out.
    const VkDescriptorBufferInfo& cb = in.constants_binding;
    if (!cb.buffer || (cb.range != VK_WHOLE_SIZE && cb.range < (static_cast<VkDeviceSize>(in.constants_bias_dw) + 912) * 4)) {
        return skip("its constants are not bound as a buffer");
    }
    // From the two frames' cameras when both were caught, else the pass's own matrix.
    const bool by_camera = g_dlss.camera_now && g_dlss.camera_last;
    const MvPush mp{w, h, in.constants_bias_dw, by_camera ? 1u : 0u, g_dlss.camera_block * kCameraDwords,
                    (g_dlss.camera_block ^ 1u) * kCameraDwords};
    // The next frame's goes in the other block; this one is then the last.
    g_dlss.camera_last = g_dlss.camera_now;
    if (g_dlss.camera_now) g_dlss.camera_block ^= 1u;
    g_dlss.camera_now = false;
    const VkDescriptorSet mv_set = alloc_set_locked(g_dlss.mv.set_layout);
    if (!write_set(mv_set, in.depth_snapshot->view, raw_velocity ? g_dlss.raw_velocity.view : in.object_velocity->view, g_dlss.linear,
                   g_dlss.motion.view)) {
        return;
    }
    {
        VkWriteDescriptorSet wb{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wb.dstSet = mv_set;
        wb.dstBinding = 4;
        wb.descriptorCount = 1;
        wb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wb.pBufferInfo = &cb;
        vkUpdateDescriptorSets(g.device, 1, &wb, 0, nullptr);
        const VkDescriptorBufferInfo ci{g_dlss.camera.buffer, 0, 2 * kCameraDwords * 4};
        wb.dstBinding = 5;
        wb.pBufferInfo = &ci;
        vkUpdateDescriptorSets(g.device, 1, &wb, 0, nullptr);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_dlss.mv.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_dlss.mv.layout, 0, 1, &mv_set, 0, nullptr);
    vkCmdPushConstants(cmd, g_dlss.mv.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT);




    // 2. DLSS.
    static const int debug_mode = [] {
        const char* e = std::getenv("BBHOST_DLSS_DEBUG");
        return e && std::strcmp(e, "mv") == 0 ? 1 : 0;
    }();
    // BBHOST_DLSS_JITTER_SIGN=-: the offset handed over the other way (to
    // check the convention; a still view is visibly softer with the wrong one).
    static const float jitter_sign = [] {
        const char* e = std::getenv("BBHOST_DLSS_JITTER_SIGN");
        return e && e[0] == '-' ? -1.0f : 1.0f;
    }();
    const VkImageView colour_view = view_of(colour->image, colour->format);
    const VkImageView depth_view = view_of(in.depth_snapshot->image, in.depth_snapshot->format);
    if (!colour_view || !depth_view) return;
    ngx::ResourceVk r_colour = resource(colour_view, colour->image, colour->format, w, h, false);
    ngx::ResourceVk r_depth = resource(depth_view, in.depth_snapshot->image, in.depth_snapshot->format, w, h, false);
    ngx::ResourceVk r_motion = resource(g_dlss.motion.view, g_dlss.motion.image, VK_FORMAT_R16G16_SFLOAT, w, h, false);
    ngx::ResourceVk r_out = resource(g_dlss.result.view, g_dlss.result.image, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, true);
    // An evaluation after a gap (a menu, a load) starts the history again.
    const std::uint64_t flip = hle_video_flip_count();
    if (flip > g_dlss.last_flip + 4) g_dlss.reset = true;
    ngx::Parameter* p = g_dlss.params;
    ngx::set_ptr(p, "Color", &r_colour);
    ngx::set_ptr(p, "Output", &r_out);
    ngx::set_ptr(p, "Depth", &r_depth);
    ngx::set_ptr(p, "MotionVectors", &r_motion);
    // The offset our viewport moved the scene by, as it is: measured, a still
    // view's gradient energy is 4.50-4.53 this way and 4.19-4.25 with both
    // signs flipped (4.21-4.29 with y alone).
    ngx::set_f(p, "Jitter.Offset.X", jitter_sign * g_dlss.jitter[0]);
    ngx::set_f(p, "Jitter.Offset.Y", jitter_sign * g_dlss.jitter[1]);
    ngx::set_f(p, "Sharpness", 0.0f);
    ngx::set_i(p, "Reset", g_dlss.reset ? 1 : 0);
    ngx::set_f(p, "MV.Scale.X", 1.0f);
    ngx::set_f(p, "MV.Scale.Y", 1.0f);
    ngx::set_ui(p, "DLSS.Input.Color.Subrect.Base.X", 0);
    ngx::set_ui(p, "DLSS.Input.Color.Subrect.Base.Y", 0);
    ngx::set_ui(p, "DLSS.Input.Depth.Subrect.Base.X", 0);
    ngx::set_ui(p, "DLSS.Input.Depth.Subrect.Base.Y", 0);
    ngx::set_ui(p, "DLSS.Input.MV.SubrectBase.X", 0);
    ngx::set_ui(p, "DLSS.Input.MV.SubrectBase.Y", 0);
    ngx::set_ui(p, "DLSS.Output.Subrect.Base.X", 0);
    ngx::set_ui(p, "DLSS.Output.Subrect.Base.Y", 0);
    ngx::set_ui(p, "DLSS.Render.Subrect.Dimensions.Width", w);
    ngx::set_ui(p, "DLSS.Render.Subrect.Dimensions.Height", h);
    ngx::set_f(p, "DLSS.Pre.Exposure", 1.0f);
    ngx::set_f(p, "DLSS.Exposure.Scale", 1.0f);
    const ngx::Result r = g_dlss.evaluate(cmd, g_dlss.feature, p, nullptr);
    if (!ngx::ok(r)) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 8) host_log("dlss: evaluation failed (0x%x)", r);
        return;
    }
    g_dlss.reset = false;
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // 3. Back into the scene colour, its alpha kept.
    const VkDescriptorSet merge_set = alloc_set_locked(g_dlss.merge.set_layout);
    if (!write_set(merge_set, colour_view, g_dlss.motion.view, g_dlss.point, g_dlss.result.view)) return;
    const MergePush mg{w, h, static_cast<std::uint32_t>(debug_mode)};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_dlss.merge.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_dlss.merge.layout, 0, 1, &merge_set, 0, nullptr);
    vkCmdPushConstants(cmd, g_dlss.merge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mg), &mg);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {w, h, 1};
    vkCmdCopyImage(cmd, g_dlss.result.image, VK_IMAGE_LAYOUT_GENERAL, colour->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    colour->fill_last = false;

    ++g_dlss.evaluations;
    g_dlss.last_flip = flip;
    if (g_dlss.evaluations == 1 || flip >= g_dlss.logged_at + 600) {
        g_dlss.logged_at = flip;
        host_log("dlss: frame %llu at %ux%u: colour 0x%llx, depth 0x%llx (jittering 0x%llx, %llu draws since), motion from 0x%llx%s",
                 static_cast<unsigned long long>(g_dlss.evaluations), w, h, static_cast<unsigned long long>(colour_base),
                 static_cast<unsigned long long>(in.depth_snapshot->base), static_cast<unsigned long long>(g_dlss.scene_depth),
                 static_cast<unsigned long long>(g_dlss.jittered), static_cast<unsigned long long>(in.object_velocity->base),
                 inverted ? ", depth inverted" : "");
        g_dlss.jittered = 0;
    }
    advance_jitter();
}

}  // namespace gpu

bool host_gpu_dlss_active() { return gpu::g_dlss.ok && gpu::g_dlss.last_flip + 4 >= hle_video_flip_count(); }
