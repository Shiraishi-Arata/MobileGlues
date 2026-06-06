// MobileGlues - config/gpu_utils.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PLUGIN_GPU_UTILS_H
#define MOBILEGLUES_PLUGIN_GPU_UTILS_H

#include <string.h>
#include <string>
#include <cstdint>

struct VulkanDeviceInfo {
    bool available = false;
    std::string deviceName;
    uint32_t apiVersion = 0;
    uint32_t driverVersion = 0;
    uint32_t vendorID = 0;
    uint32_t deviceID = 0;
    int deviceType = -1;

    bool supportsVulkan11 = false;
    bool supportsVulkan12 = false;
    bool supportsVulkan13 = false;

    uint64_t dedicatedVRAM = 0;
    uint64_t totalVRAM = 0;

    uint32_t graphicsQueueCount = 0;
    uint32_t computeQueueCount = 0;
    uint32_t transferQueueCount = 0;

    bool hasMultiDrawIndirect = false;
    bool hasDrawIndirectFirstInstance = false;
    bool hasGeometryShader = false;
    bool hasTessellationShader = false;
    bool hasDualSrcBlend = false;
    bool hasSamplerMirrorClampEdge = false;
    bool hasDescriptorIndexing = false;
    bool hasDynamicRendering = false;
    bool hasSynchronization2 = false;
    bool hasShaderOutputLayer = false;
    bool hasShaderOutputViewportIndex = false;

    std::string toString() const;
};

std::string getGPUInfo();
const VulkanDeviceInfo& getVulkanDeviceInfo();

#ifdef __cplusplus
extern "C"
{
#endif

    int isAdreno(const char* gpu);

    int isAdreno730(const char* gpu);

    int isAdreno740(const char* gpu);

    int isAdreno830(const char* gpu);

    int hasVulkan12();

    bool checkIfANGLESupported(const char* gpu);

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_PLUGIN_GPU_UTILS_H
