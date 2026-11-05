#include "advisor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

const double EFFICIENCY = 0.65;   // fraction of memory bandwidth really achieved
const double OS_RESERVE_GB = 3.0; // RAM kept for Windows and other programs
const double SMOOTH_TPS = 15.0;
const double USABLE_TPS = 5.0;

double needFor(const ModelInfo& m) { return m.sizeGB * 1.1 + 0.8; }  // weights + context

struct GpuBw { const char* key; double gbs; };

// Approximate memory bandwidth (GB/s). More specific names first.
const GpuBw GPU_TABLE[] = {
    {"RTX 5090", 1792}, {"RTX 5080", 960}, {"RTX 5070 Ti", 896}, {"RTX 5070", 672},
    {"RTX 4090", 1008}, {"RTX 4080", 717}, {"RTX 4070 Ti", 504}, {"RTX 4070", 504},
    {"RTX 4060 Ti", 288}, {"RTX 4060", 272},
    {"RTX 3090", 936}, {"RTX 3080 Ti", 912}, {"RTX 3080", 760}, {"RTX 3070", 448},
    {"RTX 3060 Ti", 448}, {"RTX 3060", 360}, {"RTX 3050", 224},
    {"RTX 2080", 448}, {"RTX 2070", 448}, {"RTX 2060", 336},
    {"GTX 1080", 320}, {"GTX 1070", 256}, {"GTX 1660", 192}, {"GTX 1650", 128},
    {"GTX 1060", 192},
    {"RX 7900 XTX", 960}, {"RX 7900 XT", 800}, {"RX 7800", 624}, {"RX 7700", 432},
    {"RX 7600", 288}, {"RX 6900", 512}, {"RX 6800", 512}, {"RX 6700", 384},
    {"RX 6600", 224}, {"RX 580", 256},
    {"Arc A770", 560}, {"Arc A750", 512}, {"Arc A580", 512},
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool containsNoCase(const std::string& hay, const char* needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

}  // namespace

const std::vector<ModelInfo>& modelCatalog() {
    static const std::vector<ModelInfo> c = {
        {"TinyLlama 1.1B (Q4)",          false, 0.7,  1.0},
        {"Llama 3.2 3B (Q4)",            false, 2.0,  1.0},
        {"Phi-3 mini 3.8B (Q4)",         false, 2.3,  1.0},
        {"Mistral 7B (Q4)",              false, 4.1,  1.0},
        {"Llama 3.1 8B (Q4)",            false, 4.9,  1.0},
        {"Gemma 2 9B (Q4)",              false, 5.8,  1.0},
        {"Qwen 2.5 14B (Q4)",            false, 9.0,  1.0},
        {"Gemma 2 27B (Q4)",             false, 16.5, 1.0},
        {"Mixtral 8x7B MoE (Q4)",        false, 26.0, 0.28},
        {"Qwen 2.5 32B (Q4)",            false, 19.5, 1.0},
        {"Llama 3.3 70B (Q4)",           false, 42.0, 1.0},
        {"Llama 3.1 405B (Q4)",          false, 240.0, 1.0},
        {"Stable Diffusion 1.5",         true,  2.5,  1.0},
        {"Stable Diffusion XL",          true,  6.5,  1.0},
        {"Flux.1 dev (fp8)",             true,  16.0, 1.0},
    };
    return c;
}

double estimateGpuBandwidth(const Spec& s) {
    if (s.vramGB <= 0) return 0;
    if (!s.vramOverridden) {
        for (const auto& e : GPU_TABLE)
            if (containsNoCase(s.gpuName, e.key)) return e.gbs;
    }
    return std::min(1000.0, 100.0 + 25.0 * s.vramGB);  // rough guess from VRAM size
}

double estimateCpuBandwidth(const Spec& s) {
    double bw = std::min(60.0, std::max(10.0, s.cores * 5.0));
    if (!s.avx2) bw *= 0.6;
    return bw;
}

const char* levelText(Level l) {
    switch (l) {
        case LV_SMOOTH: return "Smooth";
        case LV_USABLE: return "Usable";
        case LV_SLOW:   return "Slow";
        default:        return "Cannot run";
    }
}

static Level levelFor(double tps) {
    if (tps >= SMOOTH_TPS) return LV_SMOOTH;
    if (tps >= USABLE_TPS) return LV_USABLE;
    return LV_SLOW;
}

Verdict evaluate(const ModelInfo& m, const Spec& s) {
    Verdict v;
    v.mode = "-";
    v.needGB = needFor(m);
    const double read = m.sizeGB * m.activeFrac;
    const double gpuBw = estimateGpuBandwidth(s) * EFFICIENCY;
    const double cpuBw = estimateCpuBandwidth(s) * EFFICIENCY;
    const double freeRam = s.ramGB - OS_RESERVE_GB;

    if (s.vramGB > 0 && v.needGB <= s.vramGB) {
        v.mode = "GPU";
        if (m.image) {
            v.level = LV_SMOOTH;
        } else {
            v.tps = gpuBw / read;
            v.level = levelFor(v.tps);
        }
        return v;
    }
    if (m.image) return v;  // image models are only offered on GPU

    if (s.vramGB >= 2.0 && v.needGB <= s.vramGB + freeRam) {
        double f = std::min(1.0, std::max(0.0, (s.vramGB - 0.8) / m.sizeGB));
        double t = f * read / gpuBw + (1.0 - f) * read / cpuBw;
        v.mode = "GPU+CPU";
        v.tps = 1.0 / t;
        v.level = levelFor(v.tps);
        return v;
    }
    if (v.needGB <= freeRam) {
        v.mode = "CPU";
        v.tps = cpuBw / read;
        v.level = levelFor(v.tps);
    }
    return v;
}

static const char* gpuTier(double vram) {
    if (vram <= 4)  return "GTX 1650 / RTX 3050 4GB class";
    if (vram <= 6)  return "RTX 2060 / RTX 3050 6GB class";
    if (vram <= 8)  return "RTX 3060 Ti / RTX 4060 / RX 7600 class";
    if (vram <= 12) return "RTX 3060 12GB / RTX 4070 / RX 7700 XT class";
    if (vram <= 16) return "RTX 4060 Ti 16GB / RTX 4080 / RX 7800 XT class";
    if (vram <= 24) return "RTX 3090 / RTX 4090 / RX 7900 XTX class";
    if (vram <= 32) return "RTX 5090 class";
    if (vram <= 48) return "2x 24GB GPUs or RTX A6000 / RTX 6000 Ada class";
    return "multi-GPU workstation or datacenter GPUs (A100/H100)";
}

std::string recommend(const ModelInfo& m, const Spec& s) {
    char buf[512];
    std::string out;
    Verdict v = evaluate(m, s);
    const double need = needFor(m);

    snprintf(buf, sizeof buf, "Target: %s\r\nMemory needed to load: %.1f GB (weights + context)\r\n\r\n",
             m.name, need);
    out += buf;

    snprintf(buf, sizeof buf, "Selected hardware: %d cores, %.0f GB RAM, %.0f GB VRAM\r\n",
             s.cores, s.ramGB, s.vramGB);
    out += buf;
    snprintf(buf, sizeof buf, "Result: %s (%s)", levelText(v.level), v.mode.c_str());
    out += buf;
    if (v.tps > 0) {
        snprintf(buf, sizeof buf, ", about %.1f tokens/s", v.tps);
        out += buf;
    }
    out += "\r\n\r\n";

    if (v.level == LV_SMOOTH) {
        out += "Your selected hardware is enough to run this smoothly.\r\n";
        return out;
    }

    out += "To run it smoothly you need ONE of:\r\n";
    const double vramNeed = std::ceil(need);
    snprintf(buf, sizeof buf, "  A) GPU: at least %.0f GB VRAM (%s)", vramNeed, gpuTier(vramNeed));
    out += buf;
    if (s.vramGB < need) {
        snprintf(buf, sizeof buf, " - missing %.0f GB", vramNeed - s.vramGB);
        out += buf;
    }
    out += "\r\n";
    if (!m.image) {
        const double bwNeed = SMOOTH_TPS * m.sizeGB * m.activeFrac / EFFICIENCY;
        snprintf(buf, sizeof buf, "     with memory bandwidth of about %.0f GB/s or more.\r\n", bwNeed);
        out += buf;
        const double ramNeed = std::ceil(need + OS_RESERVE_GB);
        snprintf(buf, sizeof buf,
                 "  B) CPU only: at least %.0f GB RAM (more for headroom)", ramNeed);
        out += buf;
        if (s.ramGB < ramNeed) {
            snprintf(buf, sizeof buf, " - missing %.0f GB", ramNeed - s.ramGB);
            out += buf;
        }
        snprintf(buf, sizeof buf,
                 "\r\n     CPU/RAM bandwidth of about %.0f GB/s is needed for smooth speed; typical\r\n"
                 "     dual-channel desktops reach 40-60 GB/s, so large models are slow on CPU.\r\n",
                 bwNeed);
        out += buf;
    } else {
        out += "     (image models need a GPU; also keep 16 GB+ system RAM).\r\n";
    }

    out += "\r\nEstimates are approximate. Actual speed depends on the engine, quantisation and context size.\r\n";
    return out;
}
