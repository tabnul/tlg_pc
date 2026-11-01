// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), tlg contributors
// SPDX-License-Identifier: MIT
//
// tlg_dlss.dll / libtlg_dlss.so: the only part of the project that uses the NVIDIA DLSS (NGX)
// SDK. Built with MSVC on Windows (the SDK's Windows libraries are MSVC only) and with GCC on
// Linux (libnvsdk_ngx.a); the port loads it at run time.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "tlg_dlss_bridge.h" // Vulkan first: the NGX headers expect its types

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>

namespace {

constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
#ifdef _WIN32
constexpr char EngineVersion[] = "tlg-windows";
#else
constexpr char EngineVersion[] = "tlg-linux";
#endif

struct State {
    TlgDlssLogFn log{};
    std::wstring dll_directory;
    std::wstring data_directory;
    const wchar_t* dll_path{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    NVSDK_NGX_Parameter* capabilities{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    TlgDlssFeature feature_desc{};
    VkDevice device{};
    bool fg_wanted{};
    bool fg_supported{};
    NVSDK_NGX_FeatureDiscoveryInfo fg_discovery{};
    std::vector<VkExtensionProperties> instance_extensions;
    std::vector<VkExtensionProperties> device_extensions;
    NVSDK_NGX_Parameter* fg_parameters{};
    NVSDK_NGX_Handle* fg_feature{};
    bool configured{};
    bool initialized{};
} state;

template <typename... Args>
void Log(int warning, const char* format, Args... args) {
    if (!state.log)
        return;
    std::array<char, 1024> text{};
    std::snprintf(text.data(), text.size(), format, args...);
    state.log(warning, text.data());
}

bool Check(const char* operation, NVSDK_NGX_Result result) {
    if (NVSDK_NGX_FAILED(result)) {
        Log(1, "%s failed: 0x%08x", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    Log(0, "NGX: %s", message ? message : "");
}

int32_t Configure(const wchar_t* dll_directory, const wchar_t* data_directory, TlgDlssLogFn log) {
    state.log = log;
    state.dll_directory = dll_directory ? dll_directory : L".";
    state.data_directory = data_directory ? data_directory : L".";
    state.dll_path = state.dll_directory.c_str();
    state.common.PathListInfo = {&state.dll_path, 1};
    state.common.LoggingInfo = {NgxLog, NVSDK_NGX_LOGGING_LEVEL_OFF, false};
    state.discovery.SDKVersion = NVSDK_NGX_Version_API;
    state.discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    state.discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    state.discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                EngineVersion};
    state.discovery.ApplicationDataPath = state.data_directory.c_str();
    state.discovery.FeatureInfo = &state.common;
    state.fg_discovery = state.discovery;
    state.fg_discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    state.configured = true;
    return 1;
}

void FgRequest(int32_t wanted) {
    state.fg_wanted = wanted != 0;
}

// Adds the extensions of `add` that `list` does not have yet.
void Merge(std::vector<VkExtensionProperties>& list, const VkExtensionProperties* add,
           uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const bool known = std::any_of(list.begin(), list.end(), [&](const auto& extension) {
            return std::strcmp(extension.extensionName, add[i].extensionName) == 0;
        });
        if (!known)
            list.push_back(add[i]);
    }
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    VkExtensionProperties* required{};
    if (!state.configured ||
        !Check("Instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&state.discovery, count,
                                                                       &required)))
        return 0;
    state.instance_extensions.clear();
    Merge(state.instance_extensions, required, *count);
    if (state.fg_wanted) {
        uint32_t fg_count{};
        VkExtensionProperties* fg_required{};
        if (Check("Frame generation instance extension query",
                  NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
                      &state.fg_discovery, &fg_count, &fg_required)))
            Merge(state.instance_extensions, fg_required, fg_count);
    }
    *count = static_cast<uint32_t>(state.instance_extensions.size());
    *extensions = state.instance_extensions.data();
    return 1;
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (!state.configured)
        return 0;
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("Feature requirement query",
               NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &state.discovery,
                                                       &support)))
        return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        Log(1, "DLSS is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(support.FeatureSupported));
        return 0;
    }
    VkExtensionProperties* required{};
    if (!Check("Device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.discovery, count, &required)))
        return 0;
    state.device_extensions.clear();
    Merge(state.device_extensions, required, *count);
    if (state.fg_wanted) {
        NVSDK_NGX_FeatureRequirement fg_support{};
        uint32_t fg_count{};
        VkExtensionProperties* fg_required{};
        if (!Check("Frame generation requirement query",
                   NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &state.fg_discovery,
                                                           &fg_support))) {
        } else if (fg_support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
            Log(1, "DLSS frame generation is not supported on this GPU or driver (code 0x%x)",
                static_cast<unsigned>(fg_support.FeatureSupported));
        } else if (Check("Frame generation device extension query",
                         NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                             instance, physical, &state.fg_discovery, &fg_count, &fg_required))) {
            Merge(state.device_extensions, fg_required, fg_count);
            state.fg_supported = true;
        }
    }
    *count = static_cast<uint32_t>(state.device_extensions.size());
    *extensions = state.device_extensions.data();
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    if (!state.configured || state.initialized)
        return 0;
    if (!Check("NGX initialization",
               NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                   state.data_directory.c_str(), instance, physical, device, get_instance_proc,
                   get_device_proc, &state.common)))
        return 0;
    state.initialized = true;
    state.device = device;
    if (!Check("Capability query", NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.capabilities)))
        return 0;
    int available{}, needs_driver{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (needs_driver) {
        Log(1, "DLSS needs a newer NVIDIA driver");
        return 0;
    }
    if (!available) {
        Log(1, "DLSS Super Resolution is not available on this system");
        return 0;
    }
    return 1;
}

void ReleaseFeature() {
    if (state.feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.feature);
        state.feature = nullptr;
    }
    if (state.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.parameters);
        state.parameters = nullptr;
    }
}

int32_t CreateFeature(VkCommandBuffer command, const TlgDlssFeature* desc) {
    if (!state.initialized || !state.capabilities || !desc)
        return 0;
    ReleaseFeature();
    if (!Check("Parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&state.parameters)))
        return 0;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(state.parameters, key, desc->preset);
    static constexpr std::array qualities{
        NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_DLSS_Create_Params settings{};
    settings.Feature.InWidth = desc->input_width;
    settings.Feature.InHeight = desc->input_height;
    settings.Feature.InTargetWidth = desc->output_width;
    settings.Feature.InTargetHeight = desc->output_height;
    settings.Feature.InPerfQualityValue = qualities[std::clamp(desc->quality, 0, 4)];
    settings.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        (desc->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
        (desc->hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) |
        (desc->hdr == 1 ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
    const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(state.device, command, 1, 1, &state.feature,
                                                   state.parameters, &settings);
    if (!Check("DLSS feature creation", result) || !state.feature) {
        state.feature = nullptr;
        return 0;
    }
    state.feature_desc = *desc;
    Log(0, "DLSS %ux%u -> %ux%u, quality %d, preset %u%s", desc->input_width, desc->input_height,
        desc->output_width, desc->output_height, desc->quality, desc->preset,
        desc->hdr == 1 ? ", HDR input" : desc->hdr ? ", HDR input, fixed exposure" : "");
    return 1;
}

NVSDK_NGX_Resource_VK Wrap(const TlgDlssImage& image, bool writable) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                  image.format, image.width, image.height,
                                                  writable);
}

int32_t Evaluate(VkCommandBuffer command, const TlgDlssEvaluate* evaluate) {
    if (!state.feature || !state.parameters || !evaluate)
        return 0;
    auto color = Wrap(evaluate->color, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = &color;
    parameters.Feature.pInOutput = &output;
    parameters.Feature.InSharpness = evaluate->sharpness;
    parameters.pInDepth = &depth;
    parameters.pInMotionVectors = &motion;
    parameters.InRenderSubrectDimensions = {state.feature_desc.input_width,
                                            state.feature_desc.input_height};
    parameters.InJitterOffsetX = evaluate->jitter_x;
    parameters.InJitterOffsetY = evaluate->jitter_y;
    parameters.InReset = evaluate->reset;
    parameters.InMVScaleX = parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = parameters.InExposureScale = 1.0f;
    parameters.InFrameTimeDeltaInMsec = evaluate->frame_ms;
    return Check("DLSS evaluation", NGX_VULKAN_EVALUATE_DLSS_EXT(command, state.feature,
                                                                state.parameters, &parameters))
               ? 1
               : 0;
}

int32_t FgAvailable() {
    if (!state.fg_supported || !state.capabilities)
        return 0;
    int available{}, needs_driver{}, result{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_FrameGeneration_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &result);
    if (!available || needs_driver)
        Log(1, "DLSS frame generation unavailable: available %d, needs driver %d, result 0x%08x",
            available, needs_driver, static_cast<unsigned>(result));
    return available && !needs_driver;
}

void FgRelease() {
    if (state.fg_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(state.fg_feature);
        state.fg_feature = nullptr;
    }
    if (state.fg_parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.fg_parameters);
        state.fg_parameters = nullptr;
    }
}

int32_t FgCreate(VkCommandBuffer command, uint32_t width, uint32_t height, VkFormat format) {
    if (!state.initialized)
        return 0;
    FgRelease();
    if (!Check("Frame generation parameter allocation",
               NVSDK_NGX_VULKAN_AllocateParameters(&state.fg_parameters)))
        return 0;
    NVSDK_NGX_DLSSG_Create_Params settings{};
    settings.Width = width;
    settings.Height = height;
    settings.NativeBackbufferFormat = static_cast<unsigned>(format);
    if (!Check("Frame generation feature creation",
               NGX_VK_CREATE_DLSSG(command, 1, 1, &state.fg_feature, state.fg_parameters,
                                   &settings)) ||
        !state.fg_feature) {
        state.fg_feature = nullptr;
        return 0;
    }
    Log(0, "DLSS frame generation %ux%u, format %d", width, height, static_cast<int>(format));
    return 1;
}

int32_t FgEvaluate(VkCommandBuffer command, const TlgDlssFgEvaluate* evaluate) {
    if (!state.fg_feature || !evaluate)
        return 0;
    auto backbuffer = Wrap(evaluate->backbuffer, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
    auto output = Wrap(evaluate->output, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params resources{};
    resources.pBackbuffer = &backbuffer;
    resources.pDepth = &depth;
    resources.pMVecs = &motion;
    resources.pOutputInterpFrame = &output;

    // The port has no real camera data yet: a consistent stand-in camera. All camera movement is
    // in the motion vectors, so the clip-to-previous-clip matrices stay identity.
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    const auto identity = [](float (&m)[4][4]) {
        std::memset(m, 0, sizeof(m));
        m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
    };
    identity(options.clipToLensClip);
    identity(options.clipToPrevClip);
    identity(options.prevClipToClip);
    const float aspect = float(evaluate->backbuffer.width) / float(evaluate->backbuffer.height);
    const float fov = 1.0f, near_plane = 0.1f, far_plane = 10000.0f;
    const float cot = 1.0f / std::tan(fov * 0.5f);
    const float z = far_plane / (far_plane - near_plane);
    float(&proj)[4][4] = options.cameraViewToClip;
    std::memset(proj, 0, sizeof(proj));
    proj[0][0] = cot / aspect;
    proj[1][1] = cot;
    proj[2][2] = z;
    proj[2][3] = 1.0f;
    proj[3][2] = -near_plane * z;
    float(&inv)[4][4] = options.clipToCameraView;
    std::memset(inv, 0, sizeof(inv));
    inv[0][0] = aspect / cot;
    inv[1][1] = 1.0f / cot;
    inv[2][3] = 1.0f;
    inv[3][2] = -1.0f / (near_plane * z);
    inv[3][3] = 1.0f / near_plane;
    options.mvecScale[0] = 1.0f / float(evaluate->motion.width);
    options.mvecScale[1] = 1.0f / float(evaluate->motion.height);
    options.cameraUp[1] = 1.0f;
    options.cameraRight[0] = 1.0f;
    options.cameraFwd[2] = 1.0f;
    options.cameraNear = near_plane;
    options.cameraFar = far_plane;
    options.cameraFOV = fov;
    options.cameraAspectRatio = aspect;
    options.colorBuffersHDR = evaluate->hdr != 0;
    options.cameraMotionIncluded = true;
    options.reset = evaluate->reset != 0;
    options.mvecsSubrectSize = {evaluate->motion.width, evaluate->motion.height};
    options.depthSubrectSize = {evaluate->depth.width, evaluate->depth.height};
    options.backbufferSubrectSize = {evaluate->backbuffer.width, evaluate->backbuffer.height};
    options.outputInterpSubrectSize = {evaluate->output.width, evaluate->output.height};
    return Check("Frame generation evaluation",
                 NGX_VK_EVALUATE_DLSSG(command, state.fg_feature, state.fg_parameters, &resources,
                                       &options))
               ? 1
               : 0;
}

void Shutdown() {
    FgRelease();
    ReleaseFeature();
    if (state.capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
        state.capabilities = nullptr;
    }
    if (state.initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
        state.initialized = false;
        state.device = VK_NULL_HANDLE;
    }
}

const TlgDlssApi api{TLG_DLSS_BRIDGE_ABI, Configure,      InstanceExtensions, DeviceExtensions,
                    Initialize,             CreateFeature,  Evaluate,           ReleaseFeature,
                    Shutdown,               FgRequest,      FgAvailable,        FgCreate,
                    FgEvaluate,             FgRelease};

} // namespace

#ifdef _WIN32
#define TLG_DLSS_EXPORT __declspec(dllexport)
#else
#define TLG_DLSS_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TLG_DLSS_EXPORT const TlgDlssApi* TlgDlssGetApi() {
    return &api;
}
