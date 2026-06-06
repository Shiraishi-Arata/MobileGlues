// MobileGlues - config/gpu_utils.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "gpu_utils.h"
#include "../gles/loader.h"
#include "../gl/log.h"
#include "../gl/mg.h"
#if !defined(__APPLE__)
#include "vulkan/vulkan.h"
#endif

#include <EGL/egl.h>
#include <cstring>
#include <optional>
#include <sstream>
typedef const char* cstr;
static const cstr gles3_lib[] = {"libGLESv3_CM", "libGLESv3", nullptr};
static const cstr egl_libs[] = {"libEGL", nullptr};
static const cstr vk_lib[] = {"libvulkan", nullptr};

namespace egl_func {
    PFNEGLGETDISPLAYPROC eglGetDisplay = nullptr;
    PFNEGLINITIALIZEPROC eglInitialize = nullptr;
    PFNEGLCHOOSECONFIGPROC eglChooseConfig = nullptr;
    PFNEGLCREATECONTEXTPROC eglCreateContext = nullptr;
    PFNEGLMAKECURRENTPROC eglMakeCurrent = nullptr;
    PFNEGLDESTROYCONTEXTPROC eglDestroyContext = nullptr;
    PFNEGLTERMINATEPROC eglTerminate = nullptr;
} // namespace egl_func

template <typename T>
static void* open_lib(const T names[], const char* override) {
    void* handle = nullptr;
    int flags = RTLD_LOCAL | RTLD_NOW;
    if (override) {
        handle = dlopen(override, flags);
        if (handle) return handle;
    }
    for (int i = 0; names[i]; ++i) {
        handle = dlopen(names[i], flags);
        if (handle) break;
    }
    return handle;
}

static bool loadEGLFunctions(void* lib) {
    if (!lib) return false;
    egl_func::eglGetDisplay = (PFNEGLGETDISPLAYPROC)dlsym(lib, "eglGetDisplay");
    egl_func::eglInitialize = (PFNEGLINITIALIZEPROC)dlsym(lib, "eglInitialize");
    egl_func::eglChooseConfig = (PFNEGLCHOOSECONFIGPROC)dlsym(lib, "eglChooseConfig");
    egl_func::eglCreateContext = (PFNEGLCREATECONTEXTPROC)dlsym(lib, "eglCreateContext");
    egl_func::eglMakeCurrent = (PFNEGLMAKECURRENTPROC)dlsym(lib, "eglMakeCurrent");
    egl_func::eglDestroyContext = (PFNEGLDESTROYCONTEXTPROC)dlsym(lib, "eglDestroyContext");
    egl_func::eglTerminate = (PFNEGLTERMINATEPROC)dlsym(lib, "eglTerminate");

    return egl_func::eglGetDisplay && egl_func::eglInitialize && egl_func::eglChooseConfig &&
           egl_func::eglCreateContext && egl_func::eglMakeCurrent && egl_func::eglDestroyContext &&
           egl_func::eglTerminate;
}

std::string getGPUInfo() {
    void* egllib = open_lib(egl_libs, nullptr);
    if (!loadEGLFunctions(egllib)) {
        if (egllib) dlclose(egllib);
        return std::string();
    }

    EGLDisplay display = egl_func::eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        egl_func::eglTerminate(display);
        dlclose(egllib);
        return std::string();
    }
    if (egl_func::eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
        egl_func::eglTerminate(display);
        dlclose(egllib);
        return std::string();
    }

    const EGLint attribs[] = {EGL_BLUE_SIZE,
                               8,
                               EGL_GREEN_SIZE,
                               8,
                               EGL_RED_SIZE,
                               8,
                               EGL_ALPHA_SIZE,
                               8,
                               EGL_DEPTH_SIZE,
                               24,
                               EGL_SURFACE_TYPE,
                               EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_ES2_BIT,
                               EGL_NONE};
    EGLint numConfigs = 0;
    if (egl_func::eglChooseConfig(display, attribs, nullptr, 0, &numConfigs) != EGL_TRUE || numConfigs == 0) {
        egl_func::eglTerminate(display);
        dlclose(egllib);
        return std::string();
    }
    EGLConfig config;
    egl_func::eglChooseConfig(display, attribs, &config, 1, &numConfigs);

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = egl_func::eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        egl_func::eglTerminate(display);
        dlclose(egllib);
        return std::string();
    }

    if (egl_func::eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) != EGL_TRUE) {
        egl_func::eglDestroyContext(display, ctx);
        egl_func::eglTerminate(display);
        dlclose(egllib);
        return std::string();
    }

    void* glesLib = open_lib(gles3_lib, nullptr);
    std::string renderer;
    if (glesLib) {
        auto glGetString = (const GLubyte* (*)(GLenum))dlsym(glesLib, "glGetString");
        if (glGetString) {
            renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        }
        dlclose(glesLib);
    }

    egl_func::eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    egl_func::eglDestroyContext(display, ctx);
    egl_func::eglTerminate(display);
    dlclose(egllib);

    return renderer;
}

int isAdreno(const char* gpu) {
    if (!gpu) return 0;
    return strstr(gpu, "Adreno") != nullptr;
}

int isAdreno740(const char* gpu) {
    if (!gpu) return 0;
    return isAdreno(gpu) && (strstr(gpu, "740") != nullptr);
}

int isAdreno730(const char* gpu) {
    if (!gpu) return 0;
    return isAdreno(gpu) && (strstr(gpu, "730") != nullptr);
}

bool checkIfANGLESupported(const char* gpu) {
    return !isAdreno730(gpu) && !isAdreno740(gpu) && hasVulkan12();
}

int isAdreno830(const char* gpu) {
    if (!gpu) return 0;
    return isAdreno(gpu) && (strstr(gpu, "830") != nullptr);
}

std::string VulkanDeviceInfo::toString() const {
    std::ostringstream ss;
    if (!available) {
        ss << "Vulkan: Not available";
        return ss.str();
    }
    ss << "Vulkan Device: " << deviceName << "\n";
    uint32_t apiMajor = VK_API_VERSION_MAJOR(apiVersion);
    uint32_t apiMinor = VK_API_VERSION_MINOR(apiVersion);
    uint32_t apiPatch = VK_API_VERSION_PATCH(apiVersion);
    ss << "  API Version: " << apiMajor << "." << apiMinor << "." << apiPatch << "\n";
    uint32_t driverMajor = VK_VERSION_MAJOR(driverVersion);
    uint32_t driverMinor = VK_VERSION_MINOR(driverVersion);
    uint32_t driverPatch = VK_VERSION_PATCH(driverVersion);
    ss << "  Driver Version: " << driverMajor << "." << driverMinor << "." << driverPatch << "\n";
    ss << "  Vendor ID: 0x" << std::hex << vendorID << std::dec;
    ss << "  Device ID: 0x" << std::hex << deviceID << std::dec << "\n";
    ss << "  Device Type: ";
    switch (deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: ss << "Integrated GPU"; break;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: ss << "Discrete GPU"; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: ss << "Virtual GPU"; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: ss << "CPU"; break;
    default: ss << "Other"; break;
    }
    ss << "\n";
    ss << "  Dedicated VRAM: " << (dedicatedVRAM / (1024 * 1024)) << " MB\n";
    ss << "  Total VRAM: " << (totalVRAM / (1024 * 1024)) << " MB\n";
    ss << "  Graphics Queues: " << graphicsQueueCount;
    ss << "  Compute Queues: " << computeQueueCount;
    ss << "  Transfer Queues: " << transferQueueCount << "\n";
    ss << "  Features:\n";
    ss << "    Vulkan 1.1: " << (supportsVulkan11 ? "Yes" : "No") << "\n";
    ss << "    Vulkan 1.2: " << (supportsVulkan12 ? "Yes" : "No") << "\n";
    ss << "    Vulkan 1.3: " << (supportsVulkan13 ? "Yes" : "No") << "\n";
    ss << "    MultiDrawIndirect: " << (hasMultiDrawIndirect ? "Yes" : "No") << "\n";
    ss << "    DrawIndirectFirstInstance: " << (hasDrawIndirectFirstInstance ? "Yes" : "No") << "\n";
    ss << "    GeometryShader: " << (hasGeometryShader ? "Yes" : "No") << "\n";
    ss << "    TessellationShader: " << (hasTessellationShader ? "Yes" : "No") << "\n";
    ss << "    DualSrcBlend: " << (hasDualSrcBlend ? "Yes" : "No") << "\n";
    ss << "    SamplerMirrorClampEdge: " << (hasSamplerMirrorClampEdge ? "Yes" : "No") << "\n";
    ss << "    DescriptorIndexing: " << (hasDescriptorIndexing ? "Yes" : "No") << "\n";
    ss << "    DynamicRendering: " << (hasDynamicRendering ? "Yes" : "No") << "\n";
    ss << "    Synchronization2: " << (hasSynchronization2 ? "Yes" : "No") << "\n";
    ss << "    ShaderOutputLayer: " << (hasShaderOutputLayer ? "Yes" : "No") << "\n";
    ss << "    ShaderOutputViewportIndex: " << (hasShaderOutputViewportIndex ? "Yes" : "No") << "\n";
    return ss.str();
}

static std::optional<int> hasVk12;
int hasVulkan12() {
    if (hasVk12.has_value()) return hasVk12.value();
    const VulkanDeviceInfo& info = getVulkanDeviceInfo();
    hasVk12 = info.supportsVulkan12;
    return hasVk12.value();
}

static VulkanDeviceInfo cachedVkInfo;
const VulkanDeviceInfo& getVulkanDeviceInfo() {
    if (cachedVkInfo.available) return cachedVkInfo;

    void* vulkan_lib = open_lib(vk_lib, nullptr);
    if (!vulkan_lib) return cachedVkInfo;

#ifndef __APPLE__

    typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
    typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    typedef void (*PFN_vkDestroyInstance)(VkInstance, const VkAllocationCallbacks*);
    typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
    typedef void (*PFN_vkGetPhysicalDeviceProperties2)(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
    typedef void (*PFN_vkGetPhysicalDeviceFeatures)(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
    typedef void (*PFN_vkGetPhysicalDeviceFeatures2)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
    typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
    typedef void (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);

    auto vkEnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)dlsym(vulkan_lib, "vkEnumerateInstanceExtensionProperties");
    auto vkCreateInstance = (PFN_vkCreateInstance)dlsym(vulkan_lib, "vkCreateInstance");
    auto vkDestroyInstance = (PFN_vkDestroyInstance)dlsym(vulkan_lib, "vkDestroyInstance");
    auto vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)dlsym(vulkan_lib, "vkEnumeratePhysicalDevices");
    auto vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)dlsym(vulkan_lib, "vkGetPhysicalDeviceProperties");
    auto vkGetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)dlsym(vulkan_lib, "vkGetPhysicalDeviceProperties2");
    auto vkGetPhysicalDeviceFeatures =
        (PFN_vkGetPhysicalDeviceFeatures)dlsym(vulkan_lib, "vkGetPhysicalDeviceFeatures");
    auto vkGetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)dlsym(vulkan_lib, "vkGetPhysicalDeviceFeatures2");
    auto vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)dlsym(vulkan_lib, "vkGetPhysicalDeviceMemoryProperties");
    auto vkGetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)dlsym(vulkan_lib, "vkGetPhysicalDeviceQueueFamilyProperties");

    if (!vkEnumerateInstanceExtensionProperties || !vkCreateInstance || !vkDestroyInstance ||
        !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties || !vkGetPhysicalDeviceMemoryProperties ||
        !vkGetPhysicalDeviceQueueFamilyProperties) {
        dlclose(vulkan_lib);
        return cachedVkInfo;
    }

    VkResult result = VK_SUCCESS;
    uint32_t instanceExtensionCount = 0;
    result = vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);
    if (result != VK_SUCCESS) {
        dlclose(vulkan_lib);
        return cachedVkInfo;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pNext = nullptr;
    appInfo.pApplicationName = "MobileGlues Vulkan Device Info";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "MobileGlues";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = nullptr;
    createInfo.enabledExtensionCount = 0;
    createInfo.ppEnabledExtensionNames = nullptr;

    VkInstance instance = {};
    result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        dlclose(vulkan_lib);
        return cachedVkInfo;
    }

    uint32_t gpuCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (result != VK_SUCCESS || gpuCount == 0) {
        vkDestroyInstance(instance, nullptr);
        dlclose(vulkan_lib);
        return cachedVkInfo;
    }

    auto* physicalDevices = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, physicalDevices);

    for (uint32_t i = 0; i < gpuCount; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevices[i], &props);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevices[i], &memProps);

        VkPhysicalDeviceFeatures features;
        memset(&features, 0, sizeof(features));
        if (vkGetPhysicalDeviceFeatures) {
            vkGetPhysicalDeviceFeatures(physicalDevices[i], &features);
        }

        VulkanDeviceInfo info;
        info.available = true;
        info.deviceName = props.deviceName;
        info.apiVersion = props.apiVersion;
        info.driverVersion = props.driverVersion;
        info.vendorID = props.vendorID;
        info.deviceID = props.deviceID;
        info.deviceType = props.deviceType;

        info.supportsVulkan11 = props.apiVersion >= VK_API_VERSION_1_1;
        info.supportsVulkan12 = props.apiVersion >= VK_API_VERSION_1_2;
        info.supportsVulkan13 = props.apiVersion >= VK_API_VERSION_1_3;

        for (uint32_t h = 0; h < memProps.memoryHeapCount; h++) {
            if (memProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                info.totalVRAM += memProps.memoryHeaps[h].size;
                if (memProps.memoryHeaps[h].size > info.dedicatedVRAM) {
                    info.dedicatedVRAM = memProps.memoryHeaps[h].size;
                }
            }
        }

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueFamilyCount, nullptr);
        if (queueFamilyCount > 0) {
            auto* queueFamilies = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueFamilyCount, queueFamilies);
            for (uint32_t q = 0; q < queueFamilyCount; q++) {
                if (queueFamilies[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                    info.graphicsQueueCount += queueFamilies[q].queueCount;
                if (queueFamilies[q].queueFlags & VK_QUEUE_COMPUTE_BIT)
                    info.computeQueueCount += queueFamilies[q].queueCount;
                if (queueFamilies[q].queueFlags & VK_QUEUE_TRANSFER_BIT)
                    info.transferQueueCount += queueFamilies[q].queueCount;
            }
            free(queueFamilies);
        }

        info.hasMultiDrawIndirect = features.multiDrawIndirect;
        info.hasDrawIndirectFirstInstance = features.drawIndirectFirstInstance;
        info.hasGeometryShader = features.geometryShader;
        info.hasTessellationShader = features.tessellationShader;
        info.hasDualSrcBlend = features.dualSrcBlend;

        VkPhysicalDeviceVulkan12Features vk12Features = {};
        vk12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

        VkPhysicalDeviceVulkan13Features vk13Features = {};
        vk13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

        VkPhysicalDeviceFeatures2 features2 = {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &vk12Features;
        vk12Features.pNext = &vk13Features;

        if (vkGetPhysicalDeviceFeatures2) {
            vkGetPhysicalDeviceFeatures2(physicalDevices[i], &features2);
            info.hasSamplerMirrorClampEdge = vk12Features.samplerMirrorClampToEdge;
            info.hasDescriptorIndexing = vk12Features.descriptorIndexing;
            info.hasShaderOutputLayer = vk12Features.shaderOutputLayer;
            info.hasShaderOutputViewportIndex = vk12Features.shaderOutputViewportIndex;
            info.hasDynamicRendering = vk13Features.dynamicRendering;
            info.hasSynchronization2 = vk13Features.synchronization2;
        }

        cachedVkInfo = info;

        LOG_I("Vulkan Device [%d/%d]: %s", i + 1, gpuCount, props.deviceName);
        LOG_I("  API Version: %d.%d.%d", VK_API_VERSION_MAJOR(props.apiVersion),
              VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
        LOG_I("  Driver Version: %d.%d.%d", VK_VERSION_MAJOR(props.driverVersion),
              VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
        LOG_I("  Vendor ID: 0x%x, Device ID: 0x%x", props.vendorID, props.deviceID);
        LOG_I("  Device Type: %s",
              props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "Discrete GPU" :
              props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "Integrated GPU" :
              props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? "Virtual GPU" :
              props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? "CPU" : "Other");
        LOG_I("  Total Device VRAM: %llu MB", (unsigned long long)(info.totalVRAM / (1024 * 1024)));
        LOG_I("  Vulkan 1.1: %s, 1.2: %s, 1.3: %s",
              info.supportsVulkan11 ? "Yes" : "No",
              info.supportsVulkan12 ? "Yes" : "No",
              info.supportsVulkan13 ? "Yes" : "No");
        LOG_I("  Features: MultiDrawIndirect=%d, DescriptorIndexing=%d, DynamicRendering=%d, Synchronization2=%d",
              info.hasMultiDrawIndirect, info.hasDescriptorIndexing,
              info.hasDynamicRendering, info.hasSynchronization2);
    }

    free(physicalDevices);
    vkDestroyInstance(instance, nullptr);
    dlclose(vulkan_lib);

#endif
    return cachedVkInfo;
}