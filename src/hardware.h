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
    int ramSpeedMTs = 0;     // from SMBIOS, 0 if unknown
    int ramModules = 0;
    double ramBwGBs = 0;     // estimated peak bandwidth, 0 if unknown
    std::vector<GpuInfo> gpus;

    // Index of the GPU with most dedicated VRAM, or -1.
    int bestGpu() const;
};

HwInfo detectHardware();

// Measures real memory read bandwidth in GB/s using the given number of threads (takes ~1-2 s).
double benchmarkRamBandwidth(int threads);

// Free space in GB on the drive of the given folder (current drive when empty), 0 if unknown.
double freeDiskGB(const std::string& folder);

#endif
