#ifndef HARDWARE_H
#define HARDWARE_H

#include <string>
#include <vector>

struct GpuInfo {
    std::string name;
    double vramGB = 0;     // dedicated video memory
    double sharedGB = 0;   // shared system memory (integrated GPUs)
};

struct HwInfo {
    std::string cpuName;
    int physicalCores = 0;
    int logicalCores = 0;
    bool avx = false, avx2 = false, avx512 = false;
    double ramGB = 0;
    std::vector<GpuInfo> gpus;

    // Index of the GPU with most dedicated VRAM, or -1.
    int bestGpu() const;
};

HwInfo detectHardware();

#endif
