# Hardware-AI

Win32 GUI (C++17, no external libraries) that detects your hardware and estimates which AI models/engines it can run smoothly.

## Features
- Auto-detects CPU (name, cores/threads, AVX/AVX2/AVX-512), RAM (size, module speed and estimated bandwidth from SMBIOS) and GPU(s) with dedicated VRAM (DXGI).
- Catalog of ~80 models in ordered categories: small / medium / large / huge LLMs, MoE LLMs (gpt-oss, Qwen3, Mixtral, Llama 4, DeepSeek), coding, vision, image (SD, SDXL, SD3.5, Flux), video (Wan, CogVideoX, Hunyuan), speech (Whisper, XTTS) and embeddings.
- Rows are colored Smooth / Usable / Slow / Cannot run, with mode (GPU, GPU+CPU, CPU) and estimated tokens/s. Click column headers to sort; filter by category; hide "Cannot run".
- Quantization (Q4/Q5/Q8/FP16) and context length (2K-128K, KV cache) change the memory needed.
- Click any model: minimum hardware to run it smoothly, the cheapest RAM/VRAM upgrade from your hardware, and suggested software/backend.
- What-if: edit cores, RAM, RAM GB/s, VRAM or pick a GPU preset (~50 GPUs) to test an upgrade.
- Export a full text report.

Estimates are heuristic (4-bit quantised models, memory-bandwidth bound speed); real results vary by engine and settings.

## Build (Code::Blocks + MinGW)
Open `Hardware-AI.cbp`, select Release, press Build & Run. Use a 64-bit MinGW (e.g. MinGW-w64) so VRAM above 4 GB is reported correctly.

Command line:

    g++ -std=c++17 -O2 -mwindows -static -o Hardware-AI.exe src/*.cpp -lcomctl32 -lcomdlg32
