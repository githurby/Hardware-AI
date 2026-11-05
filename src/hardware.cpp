#include "hardware.h"

#include <windows.h>
#include <dxgi.h>
#include <cpuid.h>
#include <cstring>

static std::string wideToUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

int HwInfo::bestGpu() const {
    int best = -1;
    for (size_t i = 0; i < gpus.size(); ++i)
        if (best < 0 || gpus[i].vramGB > gpus[best].vramGB) best = (int)i;
    return best;
}

static void detectCpu(HwInfo& hw) {
    unsigned a = 0, b = 0, c = 0, d = 0;

    char brand[49] = {0};
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004) {
        for (unsigned i = 0; i < 3; ++i) {
            __get_cpuid(0x80000002 + i, &a, &b, &c, &d);
            std::memcpy(brand + i * 16, &a, 4);
            std::memcpy(brand + i * 16 + 4, &b, 4);
            std::memcpy(brand + i * 16 + 8, &c, 4);
            std::memcpy(brand + i * 16 + 12, &d, 4);
        }
    }
    hw.cpuName = trim(brand);
    if (hw.cpuName.empty()) hw.cpuName = "Unknown CPU";

    if (__get_cpuid(1, &a, &b, &c, &d)) hw.avx = (c >> 28) & 1;
    if (__get_cpuid_max(0, nullptr) >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        hw.avx2 = (b >> 5) & 1;
        hw.avx512 = (b >> 16) & 1;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    hw.logicalCores = (int)si.dwNumberOfProcessors;

    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
            len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(info.data(), &len)) {
            for (auto& i : info)
                if (i.Relationship == RelationProcessorCore) hw.physicalCores++;
        }
    }
    if (hw.physicalCores <= 0) hw.physicalCores = hw.logicalCores;
}

static void detectRam(HwInfo& hw) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) hw.ramGB = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
}

static void detectGpus(HwInfo& hw) {
    typedef HRESULT(WINAPI * CreateFactoryFn)(REFIID, void**);
    HMODULE dll = LoadLibraryA("dxgi.dll");
    if (!dll) return;
    CreateFactoryFn create = (CreateFactoryFn)(void*)GetProcAddress(dll, "CreateDXGIFactory1");
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&factory))) {
        const double GB = 1024.0 * 1024.0 * 1024.0;
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(ad->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                GpuInfo g;
                g.name = wideToUtf8(d.Description);
                g.vramGB = (double)d.DedicatedVideoMemory / GB;
                g.sharedGB = (double)d.SharedSystemMemory / GB;
                hw.gpus.push_back(g);
            }
            ad->Release();
        }
        factory->Release();
    }
    FreeLibrary(dll);
}

HwInfo detectHardware() {
    HwInfo hw;
    detectCpu(hw);
    detectRam(hw);
    detectGpus(hw);
    return hw;
}
