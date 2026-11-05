#include "advisor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>

namespace {

const double EFFICIENCY = 0.65;   // fraction of memory bandwidth really achieved
const double OS_RESERVE_GB = 3.0; // RAM kept for Windows and other programs
const double SMOOTH_TPS = 15.0;
const double USABLE_TPS = 5.0;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool containsNoCase(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

const Quant& quantOf(const Spec& s) {
    const auto& q = quants();
    int i = std::max(0, std::min((int)q.size() - 1, s.quant));
    return q[i];
}

double weightsGB(const ModelInfo& m, const Spec& s) {
    return m.kind == K_LLM ? m.paramsB * quantOf(s).bytes : m.fixedGB;
}

// KV cache grows with context; ~0.035 * active_params^0.6 GB per 1k tokens fits
// common GQA models (8B ~0.12, 70B ~0.45).
double kvGB(const ModelInfo& m, const Spec& s) {
    if (m.kind != K_LLM) return 0;
    return 0.035 * std::pow(m.activeB, 0.6) * s.ctx / 1000.0;
}

double memoryNeeded(const ModelInfo& m, const Spec& s) {
    if (m.kind != K_LLM) return m.fixedGB;
    return weightsGB(m, s) * 1.05 + kvGB(m, s) + 0.6;
}

// Memory needed from the GPU/CPU per generated token (GB).
double readPerToken(const ModelInfo& m, const Spec& s) {
    return m.activeB * quantOf(s).bytes;
}

Level levelFor(double tps) {
    if (tps >= SMOOTH_TPS) return LV_SMOOTH;
    if (tps >= USABLE_TPS) return LV_USABLE;
    return LV_SLOW;
}

const char* gpuTier(double vram) {
    if (vram <= 4)  return "GTX 1650 / RTX 3050 4GB class";
    if (vram <= 6)  return "RTX 2060 / GTX 1660 class";
    if (vram <= 8)  return "RTX 3070 / RTX 4060 / RX 7600 class";
    if (vram <= 12) return "RTX 3060 12GB / RTX 4070 / Arc B580 class";
    if (vram <= 16) return "RTX 4060 Ti 16GB / RTX 5070 Ti / RX 7800 XT class";
    if (vram <= 24) return "RTX 3090 / RTX 4090 / RX 7900 XTX class";
    if (vram <= 32) return "RTX 5090 class";
    if (vram <= 48) return "2x 24GB GPUs or RTX A6000 / RTX 6000 Ada class";
    if (vram <= 80) return "A100 / H100 80GB or 3-4x 24GB GPUs";
    return "multi-GPU workstation / server (several 80GB datacenter GPUs)";
}

std::string backendHint(const Spec& s) {
    if (s.vramGB <= 0) return "CPU build with AVX2";
    std::string n = lower(s.gpuName);
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    if (has("nvidia") || has("geforce") || has("rtx") || has("gtx") || has("quadro"))
        return "CUDA backend";
    if (has("radeon") || has("amd") || has(" rx ")) return "Vulkan or ROCm/HIP backend";
    if (has("intel") || has("arc")) return "Vulkan or SYCL backend";
    return "Vulkan backend";
}

std::string softwareHint(const ModelInfo& m, const Spec& s) {
    std::string t;
    switch (m.kind) {
        case K_LLM:
            t = "llama.cpp, Ollama or LM Studio (GGUF " + std::string(quantOf(s).name) + ")";
            break;
        case K_IMAGE: t = "ComfyUI or Stable Diffusion WebUI Forge"; break;
        case K_VIDEO: t = "ComfyUI video workflows"; break;
        case K_AUDIO: t = "whisper.cpp / faster-whisper (speech-to-text) or Piper / XTTS (voice)"; break;
        case K_EMBED: t = "Ollama or llama.cpp (embedding mode)"; break;
    }
    return t + " with " + backendHint(s);
}


void sortCatalog(std::vector<ModelInfo>& c) {
    // Categories keep first-seen order, models inside a category go by size.
    std::vector<std::string> order;
    for (const auto& m : c)
        if (std::find(order.begin(), order.end(), m.category) == order.end()) order.push_back(m.category);
    auto rank = [&](const ModelInfo& m) {
        return std::find(order.begin(), order.end(), m.category) - order.begin();
    };
    std::stable_sort(c.begin(), c.end(), [&](const ModelInfo& a, const ModelInfo& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        double sa = a.kind == K_LLM ? a.paramsB : a.fixedGB, sb = b.kind == K_LLM ? b.paramsB : b.fixedGB;
        return sa < sb;
    });
}

std::vector<ModelInfo>& catalogStorage();


struct Tier { double gb; double usd; int psuW; };
// Rough street prices (USD, new or used mix) and suggested PSU wattage per VRAM tier.
const Tier GPU_TIERS[] = {
    {4, 120, 450}, {6, 160, 450}, {8, 280, 550}, {10, 350, 650}, {12, 400, 650},
    {16, 550, 750}, {20, 800, 800}, {24, 1000, 850}, {32, 2200, 1000}, {48, 3500, 1000},
    {64, 5500, 1300}, {80, 8000, 1600}, {96, 8500, 1600}, {128, 11000, 2000},
    {192, 20000, 2400}, {256, 30000, 3000},
};
const double RAM_USD_PER_GB = 5.0;

const Tier* tierFor(double gb) {
    for (const auto& t : GPU_TIERS)
        if (gb <= t.gb) return &t;
    return &GPU_TIERS[sizeof(GPU_TIERS) / sizeof(GPU_TIERS[0]) - 1];
}

// Fraction of the model's layers that fit in VRAM when it is split between GPU and CPU.
double gpuFraction(const Spec& s, double need) {
    return std::min(1.0, std::max(0.0, (s.vramGB - 0.8) / std::max(0.1, need - 0.6)));
}

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[512];
    snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

}  // namespace

const std::vector<Quant>& quants() {
    static const std::vector<Quant> q = {
        {"Q4_K_M (4-bit)", 0.606}, {"Q5_K_M (5-bit)", 0.71}, {"Q8_0 (8-bit)", 1.06},
        {"FP16 (16-bit)", 2.0},
    };
    return q;
}

const std::vector<int>& contextSizes() {
    static const std::vector<int> c = {2048, 4096, 8192, 16384, 32768, 65536, 131072};
    return c;
}

// Approximate memory sizes and bandwidth. More specific names first.
const std::vector<GpuPreset>& gpuPresets() {
    static const std::vector<GpuPreset> g = {
        {"RTX 5090", 32, 1792}, {"RTX 5080", 16, 960}, {"RTX 5070 Ti", 16, 896},
        {"RTX 5070", 12, 672}, {"RTX 4090", 24, 1008}, {"RTX 4080 Super", 16, 736},
        {"RTX 4080", 16, 717}, {"RTX 4070 Ti Super", 16, 672}, {"RTX 4070 Ti", 12, 504},
        {"RTX 4070 Super", 12, 504}, {"RTX 4070", 12, 504}, {"RTX 4060 Ti", 16, 288},
        {"RTX 4060", 8, 272}, {"RTX 3090 Ti", 24, 1008}, {"RTX 3090", 24, 936},
        {"RTX 3080 Ti", 12, 912}, {"RTX 3080", 10, 760}, {"RTX 3070", 8, 448},
        {"RTX 3060 Ti", 8, 448}, {"RTX 3060", 12, 360}, {"RTX 3050", 8, 224},
        {"RTX 2080 Ti", 11, 616}, {"RTX 2080", 8, 448}, {"RTX 2070", 8, 448},
        {"RTX 2060", 6, 336}, {"GTX 1080 Ti", 11, 484}, {"GTX 1080", 8, 320},
        {"GTX 1070", 8, 256}, {"GTX 1660", 6, 192}, {"GTX 1650", 4, 128},
        {"GTX 1060", 6, 192},
        {"RX 7900 XTX", 24, 960}, {"RX 7900 XT", 20, 800}, {"RX 7800 XT", 16, 624},
        {"RX 7700 XT", 12, 432}, {"RX 7600", 8, 288}, {"RX 6900", 16, 512},
        {"RX 6800", 16, 512}, {"RX 6700", 12, 384}, {"RX 6600", 8, 224}, {"RX 580", 8, 256},
        {"Arc B580", 12, 456}, {"Arc A770", 16, 560}, {"Arc A750", 8, 512},
        {"RTX A6000", 48, 768}, {"RTX 6000 Ada", 48, 960}, {"L40S", 48, 864},
        {"A100", 80, 2039}, {"H100", 80, 3350},
    };
    return g;
}

namespace {
std::vector<ModelInfo>& catalogStorage() {
    // name, category, kind, params(B), active(B), fixed memory (GB), CPU fallback
    static std::vector<ModelInfo> c = {
        // Small LLMs
        {"Qwen2.5 0.5B",              "LLM small (<=4B)", K_LLM, 0.5, 0.5, 0, true},
        {"Gemma 3 1B",                "LLM small (<=4B)", K_LLM, 1.0, 1.0, 0, true},
        {"TinyLlama 1.1B",            "LLM small (<=4B)", K_LLM, 1.1, 1.1, 0, true},
        {"Llama 3.2 1B",              "LLM small (<=4B)", K_LLM, 1.2, 1.2, 0, true},
        {"Qwen2.5 1.5B",              "LLM small (<=4B)", K_LLM, 1.5, 1.5, 0, true},
        {"Gemma 2 2B",                "LLM small (<=4B)", K_LLM, 2.6, 2.6, 0, true},
        {"Llama 3.2 3B",              "LLM small (<=4B)", K_LLM, 3.2, 3.2, 0, true},
        {"Phi-3.5 mini 3.8B",         "LLM small (<=4B)", K_LLM, 3.8, 3.8, 0, true},
        {"Phi-4 mini 3.8B",           "LLM small (<=4B)", K_LLM, 3.8, 3.8, 0, true},
        {"Qwen3 4B",                  "LLM small (<=4B)", K_LLM, 4.0, 4.0, 0, true},
        {"Gemma 3 4B",                "LLM small (<=4B)", K_LLM, 4.3, 4.3, 0, true},
        // Medium LLMs
        {"Mistral 7B v0.3",           "LLM medium (7-14B)", K_LLM, 7.2, 7.2, 0, true},
        {"Llama 3.1 8B",              "LLM medium (7-14B)", K_LLM, 8.0, 8.0, 0, true},
        {"Qwen3 8B",                  "LLM medium (7-14B)", K_LLM, 8.2, 8.2, 0, true},
        {"Gemma 2 9B",                "LLM medium (7-14B)", K_LLM, 9.2, 9.2, 0, true},
        {"Mistral Nemo 12B",          "LLM medium (7-14B)", K_LLM, 12.2, 12.2, 0, true},
        {"Gemma 3 12B",               "LLM medium (7-14B)", K_LLM, 12.2, 12.2, 0, true},
        {"Phi-4 14B",                 "LLM medium (7-14B)", K_LLM, 14.7, 14.7, 0, true},
        {"Qwen2.5 14B",               "LLM medium (7-14B)", K_LLM, 14.8, 14.8, 0, true},
        {"Qwen3 14B",                 "LLM medium (7-14B)", K_LLM, 14.8, 14.8, 0, true},
        // Large LLMs
        {"Mistral Small 3 24B",       "LLM large (24-35B)", K_LLM, 23.6, 23.6, 0, true},
        {"Gemma 2 27B",               "LLM large (24-35B)", K_LLM, 27.2, 27.2, 0, true},
        {"Gemma 3 27B",               "LLM large (24-35B)", K_LLM, 27.4, 27.4, 0, true},
        {"Qwen2.5 32B",               "LLM large (24-35B)", K_LLM, 32.5, 32.5, 0, true},
        {"Qwen3 32B",                 "LLM large (24-35B)", K_LLM, 32.8, 32.8, 0, true},
        {"DeepSeek-R1-Distill 32B",   "LLM large (24-35B)", K_LLM, 32.8, 32.8, 0, true},
        {"Command R 35B",             "LLM large (24-35B)", K_LLM, 35.0, 35.0, 0, true},
        // Huge dense LLMs
        {"Llama 3.3 70B",             "LLM huge (70B+)", K_LLM, 70.6, 70.6, 0, true},
        {"Qwen2.5 72B",               "LLM huge (70B+)", K_LLM, 72.7, 72.7, 0, true},
        {"Command R+ 104B",           "LLM huge (70B+)", K_LLM, 104, 104, 0, true},
        {"Mistral Large 2 123B",      "LLM huge (70B+)", K_LLM, 123, 123, 0, true},
        {"Llama 3.1 405B",            "LLM huge (70B+)", K_LLM, 405, 405, 0, true},
        // Mixture of experts (all weights in memory, few read per token)
        {"gpt-oss 20B (MoE)",         "LLM MoE", K_LLM, 21, 3.6, 0, true},
        {"Qwen3 30B-A3B (MoE)",       "LLM MoE", K_LLM, 30.5, 3.3, 0, true},
        {"Mixtral 8x7B (MoE)",        "LLM MoE", K_LLM, 46.7, 12.9, 0, true},
        {"Llama 4 Scout 109B (MoE)",  "LLM MoE", K_LLM, 109, 17, 0, true},
        {"gpt-oss 120B (MoE)",        "LLM MoE", K_LLM, 117, 5.1, 0, true},
        {"Mixtral 8x22B (MoE)",       "LLM MoE", K_LLM, 141, 39, 0, true},
        {"Qwen3 235B-A22B (MoE)",     "LLM MoE", K_LLM, 235, 22, 0, true},
        {"Llama 4 Maverick 400B (MoE)", "LLM MoE", K_LLM, 400, 17, 0, true},
        {"DeepSeek V3 / R1 671B (MoE)", "LLM MoE", K_LLM, 671, 37, 0, true},
        // Coding LLMs
        {"Qwen2.5-Coder 7B",          "LLM coding", K_LLM, 7.6, 7.6, 0, true},
        {"Qwen2.5-Coder 14B",         "LLM coding", K_LLM, 14.8, 14.8, 0, true},
        {"StarCoder2 15B",            "LLM coding", K_LLM, 15.5, 15.5, 0, true},
        {"DeepSeek-Coder-V2-Lite 16B (MoE)", "LLM coding", K_LLM, 15.7, 2.4, 0, true},
        {"Qwen2.5-Coder 32B",         "LLM coding", K_LLM, 32.8, 32.8, 0, true},
        // Vision-language LLMs
        {"LLaVA 1.6 Mistral 7B",      "LLM vision", K_LLM, 7.6, 7.6, 0, true},
        {"Qwen2.5-VL 7B",             "LLM vision", K_LLM, 8.3, 8.3, 0, true},
        {"Llama 3.2 Vision 11B",      "LLM vision", K_LLM, 10.6, 10.6, 0, true},
        {"Qwen2.5-VL 32B",            "LLM vision", K_LLM, 33.5, 33.5, 0, true},
        {"Llama 3.2 Vision 90B",      "LLM vision", K_LLM, 88.6, 88.6, 0, true},
        // Image generation
        {"Stable Diffusion 1.5",      "Image", K_IMAGE, 0.86, 0, 4, false},
        {"Stable Diffusion XL",       "Image", K_IMAGE, 3.5, 0, 8, false},
        {"Flux.1 dev (GGUF Q4)",      "Image", K_IMAGE, 12, 0, 9, false},
        {"Stable Diffusion 3.5 Medium", "Image", K_IMAGE, 2.5, 0, 10, false},
        {"Flux.1 dev (fp8)",          "Image", K_IMAGE, 12, 0, 16, false},
        {"Stable Diffusion 3.5 Large", "Image", K_IMAGE, 8, 0, 18, false},
        {"Flux.1 dev (fp16)",         "Image", K_IMAGE, 12, 0, 24, false},
        // Video generation
        {"Wan 2.1 1.3B",              "Video", K_VIDEO, 1.3, 0, 8, false},
        {"Stable Video Diffusion",    "Video", K_VIDEO, 1.5, 0, 16, false},
        {"CogVideoX 5B",              "Video", K_VIDEO, 5, 0, 18, false},
        {"HunyuanVideo 13B",          "Video", K_VIDEO, 13, 0, 24, false},
        {"Wan 2.1 14B",               "Video", K_VIDEO, 14, 0, 28, false},
        // Speech / audio
        {"Whisper tiny",              "Audio", K_AUDIO, 0.04, 0, 1, true},
        {"Whisper base",              "Audio", K_AUDIO, 0.07, 0, 1, true},
        {"Whisper small",             "Audio", K_AUDIO, 0.24, 0, 2, true},
        {"XTTS v2 (voice cloning)",   "Audio", K_AUDIO, 0.47, 0, 4, true},
        {"Whisper medium",            "Audio", K_AUDIO, 0.77, 0, 5, true},
        {"MusicGen medium",           "Audio", K_AUDIO, 1.5, 0, 8, false},
        {"Whisper large-v3",          "Audio", K_AUDIO, 1.55, 0, 10, true},
        // Embeddings
        {"nomic-embed-text",          "Embedding", K_EMBED, 0.14, 0, 1, true},
        {"bge-m3",                    "Embedding", K_EMBED, 0.57, 0, 2, true},
        // Added: more engines (Grok, Kimi, GLM, MiniMax, DeepSeek distills, Mistral, IBM, AI2...)
        {"SmolLM3 3B",                "LLM small (<=4B)", K_LLM, 3.1, 3.1, 0, true},
        {"Gemma 3n E4B",              "LLM small (<=4B)", K_LLM, 6.9, 4.0, 0, true},
        {"DeepSeek-R1-Distill 7B",    "LLM medium (7-14B)", K_LLM, 7.6, 7.6, 0, true},
        {"OLMo 2 7B",                 "LLM medium (7-14B)", K_LLM, 7.3, 7.3, 0, true},
        {"Granite 3.3 8B",            "LLM medium (7-14B)", K_LLM, 8.2, 8.2, 0, true},
        {"Falcon 3 10B",              "LLM medium (7-14B)", K_LLM, 10.0, 10.0, 0, true},
        {"OLMo 2 13B",                "LLM medium (7-14B)", K_LLM, 13.7, 13.7, 0, true},
        {"DeepSeek-R1-Distill 14B",   "LLM medium (7-14B)", K_LLM, 14.8, 14.8, 0, true},
        {"Phi-4 reasoning 14B",       "LLM medium (7-14B)", K_LLM, 14.7, 14.7, 0, true},
        {"Mistral Small 3.1 24B",     "LLM large (24-35B)", K_LLM, 24.0, 24.0, 0, true},
        {"Magistral Small 24B",       "LLM large (24-35B)", K_LLM, 23.6, 23.6, 0, true},
        {"Yi 1.5 34B",                "LLM large (24-35B)", K_LLM, 34.4, 34.4, 0, true},
        {"DeepSeek-R1-Distill 70B",   "LLM huge (70B+)", K_LLM, 70.6, 70.6, 0, true},
        {"Llama 3.1 Nemotron 70B",    "LLM huge (70B+)", K_LLM, 70.6, 70.6, 0, true},
        {"Command A 111B",            "LLM huge (70B+)", K_LLM, 111, 111, 0, true},
        {"Grok-1 314B (MoE)",         "LLM MoE", K_LLM, 314, 86, 0, true},
        {"Grok-2 270B (MoE)",         "LLM MoE", K_LLM, 270, 115, 0, true},
        {"GLM-4.5 Air 106B (MoE)",    "LLM MoE", K_LLM, 106, 12, 0, true},
        {"DBRX 132B (MoE)",           "LLM MoE", K_LLM, 132, 36, 0, true},
        {"MiniMax-M2 230B (MoE)",     "LLM MoE", K_LLM, 230, 10, 0, true},
        {"GLM-4.5 355B (MoE)",        "LLM MoE", K_LLM, 355, 32, 0, true},
        {"Kimi K2 1T (MoE)",          "LLM MoE", K_LLM, 1000, 32, 0, true},
        {"Devstral Small 24B",        "LLM coding", K_LLM, 23.6, 23.6, 0, true},
        {"Qwen3-Coder 30B-A3B (MoE)", "LLM coding", K_LLM, 30.5, 3.3, 0, true},
        {"Qwen3-Coder 480B-A35B (MoE)", "LLM coding", K_LLM, 480, 35, 0, true},
        {"MiniCPM-V 2.6 8B",          "LLM vision", K_LLM, 8.1, 8.1, 0, true},
        {"Pixtral 12B",               "LLM vision", K_LLM, 12.4, 12.4, 0, true},
        {"Gemma 3 27B Vision",        "LLM vision", K_LLM, 27.4, 27.4, 0, true},
        {"PixArt-Sigma",              "Image", K_IMAGE, 0.6, 0, 8, false},
        {"Kolors",                    "Image", K_IMAGE, 2.6, 0, 12, false},
        {"HiDream-I1 (fp8)",          "Image", K_IMAGE, 17, 0, 20, false},
        {"Qwen-Image 20B (fp8)",      "Image", K_IMAGE, 20, 0, 24, false},
        {"LTX-Video 2B",              "Video", K_VIDEO, 2, 0, 10, false},
        {"Mochi 1",                   "Video", K_VIDEO, 10, 0, 24, false},
        {"Kokoro TTS 82M",            "Audio", K_AUDIO, 0.08, 0, 1, true},
        {"Piper TTS",                 "Audio", K_AUDIO, 0.06, 0, 1, true},
        {"Parakeet TDT 0.6B",         "Audio", K_AUDIO, 0.6, 0, 3, true},
        {"Bark (voice + effects)",    "Audio", K_AUDIO, 1.0, 0, 6, true},
        {"F5-TTS (voice cloning)",    "Audio", K_AUDIO, 0.34, 0, 4, true},
        {"Stable Audio Open",         "Audio", K_AUDIO, 1.2, 0, 8, false},
        {"all-MiniLM-L6-v2",          "Embedding", K_EMBED, 0.02, 0, 1, true},
        {"Qwen3-Embedding 8B",        "Embedding", K_EMBED, 8, 0, 9, true},
    };
    sortCatalog(c);
    return c;
}
}  // namespace

const std::vector<ModelInfo>& modelCatalog() { return catalogStorage(); }

const char* kindText(Kind k) {
    switch (k) {
        case K_LLM: return "LLM";
        case K_IMAGE: return "Image";
        case K_VIDEO: return "Video";
        case K_AUDIO: return "Audio";
        default: return "Embedding";
    }
}

std::string capabilities(const ModelInfo& m) {
    const std::string n = lower(m.name), c = m.category;
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    switch (m.kind) {
        case K_IMAGE: return "Image";
        case K_VIDEO: return has("stable video") ? "Video (from image)" : "Video";
        case K_EMBED: return "Search, RAG";
        case K_AUDIO:
            if (has("whisper")) return "Speech to text";
            if (has("xtts") || has("f5-tts")) return "Voice, Cloning";
            if (has("tts") || has("piper") || has("bark")) return "Voice (text to speech)";
            if (has("parakeet")) return "Speech to text";
            if (has("stable audio")) return "Music, Sound";
            if (has("musicgen")) return "Music";
            return "Audio";
        default: break;
    }
    if (c == "LLM coding") return "Source code";
    if (c == "LLM vision") return "Text, Image input";
    std::string t = "Text";
    if (c != "LLM small (<=4B)") t += ", Code";
    if (has("r1") || has("qwen3") || has("gpt-oss") || has("phi-4") || has("glm") ||
        has("magistral") || has("minimax") || has("kimi") || has("grok") || has("reasoning")) t += ", Reasoning";
    if (c == "LLM huge (70B+)" || c == "LLM MoE") t += ", Agents";
    return t;
}

double estimateGpuBandwidth(const Spec& s) {
    if (s.vramGB <= 0) return 0;
    if (!s.vramOverridden) {
        for (const auto& e : gpuPresets())
            if (containsNoCase(s.gpuName, e.name)) return e.bandwidth;
    }
    return std::min(1000.0, 100.0 + 25.0 * s.vramGB);  // rough guess from VRAM size
}

double estimateCpuBandwidth(const Spec& s) {
    double bw;
    if (s.ramMeasured && s.ramBw > 0)
        return s.ramBw;
    if (s.ramBw > 0)
        bw = std::min(s.ramBw, std::max(15.0, s.cores * 9.0));  // few cores cannot saturate RAM
    else
        bw = std::min(60.0, std::max(10.0, s.cores * 5.0));
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

Verdict evaluate(const ModelInfo& m, const Spec& s) {
    Verdict v;
    v.mode = "-";
    v.needGB = memoryNeeded(m, s);
    v.diskGB = weightsGB(m, s);
    const double freeRam = s.ramGB - OS_RESERVE_GB;
    const double gpuBw = estimateGpuBandwidth(s) * EFFICIENCY;

    if (m.kind != K_LLM) {
        if (s.vramGB > 0 && v.needGB <= s.vramGB) {
            v.mode = "GPU";
            double bw = estimateGpuBandwidth(s);
            v.level = bw >= 250 ? LV_SMOOTH : (bw >= 120 ? LV_USABLE : LV_SLOW);
        } else if (m.cpuOk && v.needGB <= freeRam) {
            v.mode = "CPU";
            if (m.kind == K_IMAGE || m.kind == K_VIDEO)
                v.level = LV_SLOW;
            else if (v.needGB <= 3)
                v.level = s.cores >= 6 ? LV_SMOOTH : LV_USABLE;
            else
                v.level = s.cores >= 8 ? LV_USABLE : LV_SLOW;
        }
        return v;
    }

    const double read = readPerToken(m, s);
    const double cpuBw = estimateCpuBandwidth(s) * (s.ramMeasured ? 1.0 : EFFICIENCY);

    if (s.vramGB > 0 && v.needGB <= s.vramGB) {
        v.mode = "GPU";
        v.tps = gpuBw / read;
        v.level = levelFor(v.tps);
        return v;
    }
    if (s.vramGB >= 2.0 && v.needGB <= s.vramGB + freeRam) {
        double f = gpuFraction(s, v.needGB);
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

std::string recommend(const ModelInfo& m, const Spec& s) {
    std::string out;
    Verdict v = evaluate(m, s);
    const double need = v.needGB;

    out += std::string(m.name) + "   [" + m.category + "]\r\n";
    if (m.kind == K_LLM)
        out += fmt("Memory needed: %.1f GB  (weights %.1f GB + context cache + overhead)\r\n",
                   need, v.diskGB) +
               "Settings: " + quantOf(s).name + ", context " + std::to_string(s.ctx) + " tokens\r\n";
    else
        out += fmt("Memory needed: about %.0f GB (typical default precision)\r\n", need);

    if (s.diskFreeGB > 0) {
        out += fmt("Download size: about %.1f GB; free disk space: %.0f GB", v.diskGB, s.diskFreeGB);
        out += v.diskGB * 1.1 > s.diskFreeGB ? " - NOT ENOUGH, free some space first!\r\n" : " - OK\r\n";
    } else {
        out += fmt("Download size: about %.1f GB\r\n", v.diskGB);
    }
    out += fmt("Your selection: %.0f CPU cores, %.0f GB RAM, ", s.cores, s.ramGB) +
           fmt("%.0f GB VRAM", s.vramGB) + "\r\n";
    out += std::string("Result: ") + levelText(v.level) + " (" + v.mode + ")";
    if (v.tps > 0) out += fmt(", about %.1f tokens/s", v.tps);
    out += "\r\n\r\n";

    if (v.level == LV_SMOOTH) {
        out += "Your hardware is enough to run this smoothly.\r\n";
        out += "Software: " + softwareHint(m, s) + "\r\n";
        return out;
    }

    out += "MINIMUM HARDWARE TO RUN SMOOTHLY (choose one):\r\n";
    const double vramNeed = std::ceil(need);
    double bwNeed = 0;
    if (m.kind == K_LLM) bwNeed = SMOOTH_TPS * readPerToken(m, s) / EFFICIENCY;

    out += fmt("  A) GPU with at least %.0f GB VRAM", vramNeed) + " (" + gpuTier(vramNeed) + ")";
    if (s.vramGB < need) out += fmt(" - you are missing %.0f GB", vramNeed - s.vramGB);
    out += "\r\n";
    if (m.kind == K_LLM) {
        out += fmt("     memory bandwidth about %.0f GB/s or more.\r\n", bwNeed);
        const double ramNeed = std::ceil(need + OS_RESERVE_GB);
        out += fmt("  B) CPU only: at least %.0f GB RAM", ramNeed);
        if (s.ramGB < ramNeed) out += fmt(" - you are missing %.0f GB", ramNeed - s.ramGB);
        out += fmt("\r\n     and RAM bandwidth about %.0f GB/s (dual-channel DDR5 is ~90, ", bwNeed);
        out += "8-12 channel server boards 300+).\r\n";
        if (bwNeed > 100) out += "     This is only realistic on a server/workstation platform.\r\n";
    } else if (m.cpuOk) {
        out += fmt("  B) CPU only: %.0f GB RAM and 8+ cores (works, but slower).\r\n",
                   std::ceil(need + OS_RESERVE_GB));
    } else {
        out += "     Strong GPU recommended (250+ GB/s memory bandwidth) and 16 GB+ system RAM.\r\n";
    }

    // Smallest upgrade that makes this smooth, starting from the current hardware.
    out += "\r\nCHEAPEST UPGRADE FROM YOUR CURRENT HARDWARE:\r\n";
    static const double VRAM_TIERS[] = {4, 6, 8, 10, 12, 16, 20, 24, 32, 48, 64, 80, 96, 128, 192, 256};
    static const double RAM_TIERS[] = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
    bool any = false;
    for (double t : VRAM_TIERS) {
        if (t <= s.vramGB) continue;
        Spec u = s;
        u.vramGB = t;
        u.vramOverridden = true;
        if (evaluate(m, u).level == LV_SMOOTH) {
            const Tier* tr = tierFor(t);
            out += fmt("  GPU: upgrade to %.0f GB VRAM", t) +
                   fmt(" (~%.0f GB/s class)", estimateGpuBandwidth(u)) + " - " + gpuTier(t) + "\r\n";
            out += fmt("       budget: ~$%.0f (rough), power supply of at least %.0f W", tr->usd,
                       (double)tr->psuW) + ", check case space and cooling\r\n";
            any = true;
            break;
        }
    }
    for (double t : RAM_TIERS) {
        if (t <= s.ramGB) continue;
        Spec u = s;
        u.ramGB = t;
        if (evaluate(m, u).level == LV_SMOOTH) {
            out += fmt("  RAM: upgrade to %.0f GB", t) +
                   fmt(" (add %.0f GB, budget ~$%.0f rough)\r\n", t - s.ramGB, (t - s.ramGB) * RAM_USD_PER_GB);
            any = true;
            break;
        }
    }
    if (!any) out += "  No single upgrade of RAM or VRAM is enough; see the minimum hardware above.\r\n";

    out += "\r\nSoftware: " + softwareHint(m, s) + "\r\n";
    out += "Estimates are approximate; real speed depends on engine, quantisation and context.\r\n";
    return out;
}

int loadCustomModels(const std::string& path) {
    static std::deque<std::string> store;  // keeps the strings the catalog points to
    std::ifstream f(path);
    if (!f) return 0;
    auto& cat = catalogStorage();
    int added = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> p;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) {
            size_t a = cell.find_first_not_of(" \t"), b = cell.find_last_not_of(" \t");
            p.push_back(a == std::string::npos ? "" : cell.substr(a, b - a + 1));
        }
        if (p.size() < 7 || p[0].empty() || lower(p[0]) == "name") continue;
        std::string k = lower(p[2]);
        Kind kind = k == "llm" ? K_LLM : k == "image" ? K_IMAGE : k == "video" ? K_VIDEO
                  : k == "audio" ? K_AUDIO : k == "embed" ? K_EMBED : (Kind)-1;
        if ((int)kind < 0) continue;
        double params = atof(p[3].c_str()), active = atof(p[4].c_str()), fixed = atof(p[5].c_str());
        if (kind == K_LLM && params <= 0) continue;
        if (kind != K_LLM && fixed <= 0) continue;
        if (active <= 0 || active > params) active = params;
        bool dup = false;
        for (const auto& m : cat) dup = dup || lower(m.name) == lower(p[0]);
        if (dup) continue;
        store.push_back(p[0]);
        const char* name = store.back().c_str();
        store.push_back(p[1].empty() ? "Custom" : p[1]);
        const char* category = store.back().c_str();
        cat.push_back({name, category, kind, params, active, fixed, p[6] == "1" || lower(p[6]) == "true"});
        ++added;
    }
    if (added) sortCatalog(cat);
    return added;
}

std::string runCommand(const ModelInfo& m, const Spec& s) {
    if (m.kind != K_LLM)
        return std::string("# ") + m.name + ": run it with " + softwareHint(m, s) + "\r\n";
    Verdict v = evaluate(m, s);
    if (v.mode == "-") return std::string("# ") + m.name + " does not fit this hardware.\r\n";

    const int layers = (int)std::lround(12.0 * std::pow(std::max(0.5, m.paramsB), 0.36) + 8.0);
    int ngl = 0;
    if (v.mode == "GPU") ngl = 99;
    else if (v.mode == "GPU+CPU") ngl = (int)std::floor(gpuFraction(s, v.needGB) * layers);

    std::string q = quantOf(s).name;
    q = q.substr(0, q.find(' '));
    std::string cmd = std::string("llama-server -m \"") + m.name + "-" + q + ".gguf\"";
    cmd += " -c " + std::to_string(s.ctx) + " -t " + std::to_string(std::max(1, s.cores));
    if (v.mode == "GPU+CPU" && m.activeB < m.paramsB * 0.6)
        cmd += " -ngl 99 -ot \".ffn_.*_exps.=CPU\"";  // MoE: keep attention on GPU, experts in RAM
    else
        cmd += " -ngl " + std::to_string(ngl);
    if (s.vramGB > 0 && v.mode != "CPU") cmd += " -fa";
    return cmd + "\r\n";
}
