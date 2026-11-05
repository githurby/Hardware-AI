#ifndef ADVISOR_H
#define ADVISOR_H

#include <string>
#include <vector>

// Hardware profile used for the calculations (detected or "what-if").
struct Spec {
    int cores = 4;          // physical CPU cores
    bool avx2 = true;
    double ramGB = 16;
    double vramGB = 0;
    std::string gpuName;
    bool vramOverridden = false;  // true: ignore GPU name table, estimate from VRAM
};

struct ModelInfo {
    const char* name;
    bool image;          // image-generation model (GPU only)
    double sizeGB;       // size in memory (4-bit quantised for LLMs)
    double activeFrac;   // fraction of weights read per token (MoE < 1)
};

enum Level { LV_NO = 0, LV_SLOW = 1, LV_USABLE = 2, LV_SMOOTH = 3 };

struct Verdict {
    Level level = LV_NO;
    std::string mode;    // "GPU", "GPU+CPU", "CPU", "-"
    double tps = 0;      // estimated tokens/second (LLMs only)
    double needGB = 0;   // memory needed to load the model
};

const std::vector<ModelInfo>& modelCatalog();
double estimateGpuBandwidth(const Spec& s);  // GB/s
double estimateCpuBandwidth(const Spec& s);  // GB/s
Verdict evaluate(const ModelInfo& m, const Spec& s);
const char* levelText(Level l);

// Text describing what hardware is needed to run model m smoothly,
// compared with spec s.
std::string recommend(const ModelInfo& m, const Spec& s);

#endif
