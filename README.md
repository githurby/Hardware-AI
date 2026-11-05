# Hardware-AI

Win32 GUI (C++17, no external libraries) that detects your hardware and estimates which AI models/engines it can run smoothly.

## Features
- Auto-detects CPU (name, cores/threads, AVX/AVX2/AVX-512), RAM (size, module speed and estimated bandwidth from SMBIOS) and GPU(s) with dedicated VRAM (DXGI).
- Catalog of ~125 models (incl. Grok, Kimi K2, GLM, MiniMax, DeepSeek, Qwen3, Llama 4, Mistral...) in ordered categories: small / medium / large / huge LLMs, MoE LLMs (gpt-oss, Qwen3, Mixtral, Llama 4, DeepSeek), coding, vision, image (SD, SDXL, SD3.5, Flux), video (Wan, CogVideoX, Hunyuan), speech (Whisper, XTTS) and embeddings.
- Rows are colored Smooth / Usable / Slow / Cannot run, with mode (GPU, GPU+CPU, CPU) and estimated tokens/s. Click column headers to sort; filter by category; hide "Cannot run".
- Quantization (Q4/Q5/Q8/FP16) and context length (2K-128K, KV cache) change the memory needed.
- Click any model: minimum hardware to run it smoothly, the cheapest RAM/VRAM upgrade from your hardware, and suggested software/backend.
- What-if: edit cores, RAM, RAM GB/s, VRAM or pick a GPU preset (~50 GPUs) to test an upgrade.
- "Can do" column with short capability tags (Source code, Text, Reasoning, Image, Video, Speech to text, Voice/Cloning, Music, Search/RAG...).
- Optional dark mode (checkbox, remembered between runs).
- Export a full text report.

Estimates are heuristic (4-bit quantised models, memory-bandwidth bound speed); real results vary by engine and settings.

## Build (Code::Blocks + MinGW)
Open `Hardware-AI.cbp`, select Release, press Build & Run. Use a 64-bit MinGW (e.g. MinGW-w64) so VRAM above 4 GB is reported correctly.

Command line:

    windres res/app.rc -O coff -o app_res.o
    g++ -std=c++17 -O2 -mwindows -static -o Hardware-AI.exe src/*.cpp app_res.o -lcomctl32 -lcomdlg32 -ldwmapi

## Extra features
- **Benchmark RAM**: measures real memory bandwidth (more accurate speed estimates).
- **Sum all GPUs**: combines VRAM of multiple GPUs.
- **Disk check**: warns when the model download exceeds free disk space; "Download" column.
- **models.csv**: add your own models (see the sample file; searched next to the exe and up to 3 folders above).
- **Compare with**: second hardware column to compare against a GPU/RAM combo.
- **Upgrade advisor**: rough cost, PSU and cooling notes (estimates only).
- **Favorites**: double-click a row to star it; "Favorites only" filter (saved in registry).
- **Copy run command**: llama.cpp `llama-server` command for the selected LLM.
- **Profiles**: save/load hardware setups as `.hwai` files.
- **Languages**: English / Português (checkbox in Options; saved, defaults to the Windows UI language). Translates the UI, hardware panel, recommendations and reports.
- **Update models online**: downloads `models-online.csv` from this repository (cached in `%APPDATA%\Hardware-AI`) and adds new models without reinstalling. A copy next to the exe (or up to 3 folders above) is also loaded at startup.

## Manual
Full user guide (dark theme): [docs/manual.html](docs/manual.html) - download and open in any browser.
