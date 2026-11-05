#ifndef ADVISOR_H
#define ADVISOR_H

#include <string>
#include <vector>

// Hardware profile used for the calculations (detected or "what-if").
struct Spec {
    int cores = 4;           // physical CPU cores
    bool avx2 = true;
    double ramGB = 16;
    double ramBw = 0;        // system RAM bandwidth in GB/s, 0 = estimate from cores
    double vramGB = 0;
    std::string gpuName;
    bool vramOverridden = false;  // true: VRAM differs from the named GPU, estimate bandwidth
    int quant = 0;           // index into quants()
    int ctx = 4096;          // context length in tokens
    bool ramMeasured = false;  // ramBw comes from a real benchmark (no efficiency factor)
    double diskFreeGB = 0;   // free disk space for downloads, 0 = unknown
};

enum Kind { K_LLM, K_IMAGE, K_VIDEO, K_AUDIO, K_EMBED };

struct ModelInfo {
    const char* name;
    const char* category;
    Kind kind;
    double paramsB;    // total parameters in billions (display / LLM size)
    double activeB;    // parameters read per token (MoE < paramsB), LLM only
    double fixedGB;    // memory needed for non-LLM models
    bool cpuOk;        // can fall back to CPU
};

struct Quant {
    const char* name;
    double bytes;      // bytes per parameter
};

struct GpuPreset {
    const char* name;
    double vramGB;
    double bandwidth;  // GB/s
};

enum Level { LV_NO = 0, LV_SLOW = 1, LV_USABLE = 2, LV_SMOOTH = 3 };

struct Verdict {
    Level level = LV_NO;
    std::string mode;    // "GPU", "GPU+CPU", "CPU", "-"
    double tps = 0;      // estimated tokens/second (LLMs only)
    double needGB = 0;   // memory needed to load and run the model
    double diskGB = 0;   // approximate download size
};

const std::vector<ModelInfo>& modelCatalog();
const std::vector<Quant>& quants();
const std::vector<GpuPreset>& gpuPresets();
const std::vector<int>& contextSizes();

double estimateGpuBandwidth(const Spec& s);  // GB/s
double estimateCpuBandwidth(const Spec& s);  // GB/s
Verdict evaluate(const ModelInfo& m, const Spec& s);
const char* levelText(Level l);
const char* kindText(Kind k);

// Short capability tags, e.g. "Source code", "Voice, Cloning", "Video".
std::string capabilities(const ModelInfo& m);

// Explains what hardware model m needs to run smoothly and the cheapest upgrade
// from spec s, plus suggested software.
std::string recommend(const ModelInfo& m, const Spec& s);

// Loads extra models from a CSV file (name,category,kind,paramsB,activeB,fixedGB,cpuOk;
// kind = llm|image|video|audio|embed). Returns how many models were added.
int loadCustomModels(const std::string& path);

// A ready-to-paste llama.cpp command with suggested GPU layers, threads and context.
std::string runCommand(const ModelInfo& m, const Spec& s);

#endif
