#include "i18n.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace {

bool g_pt = false;

struct Pair {
    const char* en;
    const char* pt;
};

// Whole-string matches: labels, buttons, headers, messages, format strings, tags.
const Pair EXACT[] = {
    {" Detected hardware ", " Hardware detetado "},
    {" What-if hardware (edit to test upgrades, blank RAM GB/s = estimate) ",
     " Hardware hipotético (edite para testar upgrades; RAM GB/s vazio = estimativa) "},
    {" Options ", " Opções "},
    {"Re-detect", "Detetar de novo"},
    {"Export report...", "Exportar relatório..."},
    {"Benchmark RAM", "Medir RAM"},
    {"Save profile...", "Guardar perfil..."},
    {"Load profile...", "Carregar perfil..."},
    {"Copy run command", "Copiar comando"},
    {"CPU cores:", "Núcleos CPU:"},
    {"GPU preset:", "Predef. GPU:"},
    {"Apply", "Aplicar"},
    {"Reset to detected", "Repor detetado"},
    {"Sum all GPUs", "Somar todas as GPUs"},
    {"Quant:", "Quant.:"},
    {"Context:", "Contexto:"},
    {"Category:", "Categoria:"},
    {"Hide \"Cannot run\"", "Ocultar \"Não roda\""},
    {"Dark mode", "Modo escuro"},
    {"Favorites only", "Só favoritos"},
    {"Compare with:", "Comparar com:"},
    {"Update models online", "Atualizar modelos online"},
    {"AI engine / model", "Motor / modelo de IA"},
    {"Category", "Categoria"},
    {"Can do", "Faz"},
    {"Params", "Parâm."},
    {"Memory needed", "Memória nec."},
    {"Result", "Resultado"},
    {"Mode", "Modo"},
    {"Compare", "Comparar"},
    {"Detected GPU (as found)", "GPU detetada (como encontrada)"},
    {"No GPU (CPU only)", "Sem GPU (só CPU)"},
    {"All categories", "Todas as categorias"},
    {"Smooth", "Fluido"},
    {"Usable", "Utilizável"},
    {"Slow", "Lento"},
    {"Cannot run", "Não roda"},
    {"Click a model in the list to see the minimum hardware needed to run it smoothly.",
     "Clique num modelo da lista para ver o hardware mínimo necessário para o correr com fluidez."},
    {"Showing %d of %d.   Smooth: %d   Usable: %d   Slow: %d   Cannot run: %d",
     "A mostrar %d de %d.   Fluido: %d   Utilizável: %d   Lento: %d   Não roda: %d"},
    {"   |   Best smooth LLM: %s (%.1f tok/s)", "   |   Melhor LLM fluido: %s (%.1f tok/s)"},
    {"   |   +%d custom models", "   |   +%d modelos adicionais"},
    {"The benchmark could not run (not enough free memory?).",
     "Não foi possível executar o teste (pouca memória livre?)."},
    {"Select a model in the list first.", "Selecione primeiro um modelo da lista."},
    {"Copied to clipboard:\n\n", "Copiado para a área de transferência:\n\n"},
    {"Cannot write the file.", "Não foi possível escrever o ficheiro."},
    {"This is not a valid profile file.", "Este não é um ficheiro de perfil válido."},
    {"Downloading the latest model list failed. Check your internet connection.",
     "Falhou a transferência da lista de modelos. Verifique a ligação à internet."},
    {"The downloaded file is not a valid model list.", "O ficheiro transferido não é uma lista de modelos válida."},
    {"Model list is already up to date.", "A lista de modelos já está atualizada."},
    {"Added %d new models from the online list.", "Foram adicionados %d modelos novos da lista online."},
    {"Hardware-AI report", "Relatório Hardware-AI"},
    {"Used for calculation: %d cores, %.0f GB RAM, %.0f GB VRAM, %s, context %d\r\n\r\n",
     "Usado no cálculo: %d núcleos, %.0f GB RAM, %.0f GB VRAM, %s, contexto %d\r\n\r\n"},
    {"Model", "Modelo"},
    {"Memory", "Memória"},
    // Capability tags
    {"Image", "Imagem"},
    {"Video", "Vídeo"},
    {"Video (from image)", "Vídeo (a partir de imagem)"},
    {"Search", "Pesquisa"},
    {"Speech to text", "Voz para texto"},
    {"Voice", "Voz"},
    {"Cloning", "Clonagem"},
    {"Voice (text to speech)", "Voz (texto para voz)"},
    {"Music", "Música"},
    {"Sound", "Som"},
    {"Audio", "Áudio"},
    {"Source code", "Código-fonte"},
    {"Text", "Texto"},
    {"Image input", "Entrada de imagem"},
    {"Code", "Código"},
    {"Reasoning", "Raciocínio"},
    {"Agents", "Agentes"},
};

// Substring replacements for long generated text (hardware panel, recommendation, run command).
const Pair PHRASES[] = {
    {"Memory needed: about ", "Memória necessária: cerca de "},
    {"Memory needed: ", "Memória necessária: "},
    {"  (weights ", "  (pesos "},
    {" GB + context cache + overhead)", " GB + cache de contexto + overhead)"},
    {"Settings: ", "Configuração: "},
    {", context ", ", contexto "},
    {" GB (typical default precision)", " GB (precisão padrão típica)"},
    {"Download size: about ", "Tamanho do download: cerca de "},
    {"; free disk space: ", "; espaço livre em disco: "},
    {" - NOT ENOUGH, free some space first!", " - INSUFICIENTE, liberte espaço primeiro!"},
    {"Your selection: ", "A sua seleção: "},
    {" CPU cores, ", " núcleos de CPU, "},
    {" GB RAM, ", " GB de RAM, "},
    {" GB VRAM", " GB de VRAM"},
    {"Result: Smooth", "Resultado: Fluido"},
    {"Result: Usable", "Resultado: Utilizável"},
    {"Result: Slow", "Resultado: Lento"},
    {"Result: Cannot run", "Resultado: Não roda"},
    {", about ", ", cerca de "},
    {"Your hardware is enough to run this smoothly.", "O seu hardware é suficiente para correr isto com fluidez."},
    {"MINIMUM HARDWARE TO RUN SMOOTHLY (choose one):", "HARDWARE MÍNIMO PARA CORRER COM FLUIDEZ (escolha um):"},
    {"A) GPU with at least ", "A) GPU com pelo menos "},
    {" - you are missing ", " - faltam-lhe "},
    {"memory bandwidth about ", "largura de banda de memória de cerca de "},
    {" or more.", " ou mais."},
    {"B) CPU only: at least ", "B) Só CPU: pelo menos "},
    {"and RAM bandwidth about ", "e largura de banda da RAM de cerca de "},
    {"(dual-channel DDR5 is ~90, ", "(DDR5 dual-channel ronda os ~90, "},
    {"8-12 channel server boards 300+).", "placas de servidor de 8-12 canais 300+)."},
    {"This is only realistic on a server/workstation platform.",
     "Isto só é realista numa plataforma de servidor/workstation."},
    {"B) CPU only: ", "B) Só CPU: "},
    {" GB RAM and 8+ cores (works, but slower).", " GB de RAM e 8+ núcleos (funciona, mas mais lento)."},
    {" GB RAM", " GB de RAM"},
    {"Strong GPU recommended (250+ GB/s memory bandwidth) and 16 GB+ system RAM.",
     "GPU forte recomendada (250+ GB/s de largura de banda) e 16 GB+ de RAM do sistema."},
    {"CHEAPEST UPGRADE FROM YOUR CURRENT HARDWARE:", "UPGRADE MAIS BARATO A PARTIR DO SEU HARDWARE ATUAL:"},
    {"GPU: upgrade to ", "GPU: atualizar para "},
    {" GB/s class)", " GB/s de classe)"},
    {"budget: ~$", "orçamento: ~$"},
    {" (rough), power supply of at least ", " (aprox.), fonte de alimentação de pelo menos "},
    {", check case space and cooling", ", verifique o espaço na caixa e a refrigeração"},
    {"RAM: upgrade to ", "RAM: atualizar para "},
    {" (add ", " (adicionar "},
    {" GB, budget ~$", " GB, orçamento ~$"},
    {" rough)", " aprox.)"},
    {"No single upgrade of RAM or VRAM is enough; see the minimum hardware above.",
     "Nenhum upgrade isolado de RAM ou VRAM chega; veja o hardware mínimo acima."},
    {"Estimates are approximate; real speed depends on engine, quantisation and context.",
     "As estimativas são aproximadas; a velocidade real depende do motor, da quantização e do contexto."},
    {"Suggested llama.cpp command (button \"Copy run command\" copies it):",
     "Comando llama.cpp sugerido (o botão \"Copiar comando\" copia-o):"},
    {" does not fit this hardware.", " não cabe neste hardware."},
    {": run it with ", ": execute com "},
    {" class", " classe"},
    {"2x 24GB GPUs or ", "2x GPUs de 24GB ou "},
    {"or 3-4x 24GB GPUs", "ou 3-4x GPUs de 24GB"},
    {"multi-GPU workstation / server (several 80GB datacenter GPUs)",
     "workstation/servidor multi-GPU (várias GPUs de datacenter de 80GB)"},
    {"llama.cpp, Ollama or LM Studio (GGUF ", "llama.cpp, Ollama ou LM Studio (GGUF "},
    {"ComfyUI or Stable Diffusion WebUI Forge", "ComfyUI ou Stable Diffusion WebUI Forge"},
    {"ComfyUI video workflows", "workflows de vídeo no ComfyUI"},
    {"(speech-to-text) or Piper / XTTS (voice)", "(voz para texto) ou Piper / XTTS (voz)"},
    {"Ollama or llama.cpp (embedding mode)", "Ollama ou llama.cpp (modo embedding)"},
    {" with CPU build with AVX2", " com build de CPU com AVX2"},
    {" with CUDA backend", " com backend CUDA"},
    {" with Vulkan or ROCm/HIP backend", " com backend Vulkan ou ROCm/HIP"},
    {" with Vulkan or SYCL backend", " com backend Vulkan ou SYCL"},
    {" with Vulkan backend", " com backend Vulkan"},
    // Hardware panel
    {" cores / ", " núcleos / "},
    {":yes", ":sim"},
    {":no", ":não"},
    {" modules @ ", " módulos @ "},
    {"measured read bandwidth: ", "largura de banda medida (leitura): "},
    {"none detected", "nenhuma detetada"},
    {" (shared ", " (partilhada "},
    {" GPUs with VRAM: combined ", " GPUs com VRAM: total "},
    {" GB free for downloads", " GB livres para downloads"},
    {"Disk: ", "Disco: "},
};

std::string toAnsi(const std::string& utf8) {
    if (utf8.empty()) return utf8;
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wn);
    int an = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, nullptr, 0, nullptr, nullptr);
    if (an <= 0) return utf8;
    std::string a(an, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, &a[0], an, nullptr, nullptr);
    return a;
}

const std::map<std::string, const char*>& exactMap() {
    static std::map<std::string, const char*> m;
    if (m.empty())
        for (const auto& p : EXACT) m[p.en] = p.pt;
    return m;
}

const std::vector<Pair>& phrases() {
    static std::vector<Pair> v;
    if (v.empty()) {
        v.assign(std::begin(PHRASES), std::end(PHRASES));
        std::stable_sort(v.begin(), v.end(), [](const Pair& a, const Pair& b) {
            return strlen(a.en) > strlen(b.en);
        });
    }
    return v;
}

// Longest phrases first; the output of one replacement is never rescanned.
std::string replacePhrases(const std::string& in) {
    std::string out;
    const auto& ph = phrases();
    size_t i = 0;
    while (i < in.size()) {
        bool hit = false;
        for (const auto& p : ph) {
            size_t n = strlen(p.en);
            if (in.compare(i, n, p.en) == 0) {
                out += p.pt;
                i += n;
                hit = true;
                break;
            }
        }
        if (!hit) out += in[i++];
    }
    return out;
}

}  // namespace

void setPortuguese(bool on) { g_pt = on; }
bool isPortuguese() { return g_pt; }

std::string tr(const std::string& en) {
    if (!g_pt) return en;
    auto it = exactMap().find(en);
    if (it != exactMap().end()) return toAnsi(it->second);
    return toAnsi(replacePhrases(en));
}

std::string trTags(const std::string& en) {
    if (!g_pt) return en;
    std::string out, cur;
    auto flush = [&]() {
        auto it = exactMap().find(cur);
        out += it != exactMap().end() ? it->second : cur;
        cur.clear();
    };
    for (size_t i = 0; i < en.size(); ++i) {
        if (en[i] == ',' && i + 1 < en.size() && en[i + 1] == ' ') {
            flush();
            out += ", ";
            ++i;
        } else {
            cur += en[i];
        }
    }
    flush();
    return toAnsi(out);
}
