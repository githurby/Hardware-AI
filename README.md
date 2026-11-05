# Hardware-AI

Win32 GUI (C++17, no external libraries) that detects your hardware and estimates which AI models/engines it can run smoothly.

## Features
- Auto-detects CPU (name, cores/threads, AVX/AVX2/AVX-512), RAM and GPU(s) with dedicated VRAM (via DXGI).
- Rates ~15 models (LLMs from 1B to 405B, Stable Diffusion, Flux) as **Smooth / Usable / Slow / Cannot run**, with mode (GPU, GPU+CPU, CPU) and estimated tokens/s.
- Optional: edit CPU cores, RAM and VRAM ("what-if") to test an upgrade, and pick a target model to see what hardware it needs and what is missing.

Estimates are heuristic (4-bit quantised models, memory-bandwidth bound speed); real results vary by engine and settings.

## Build (Code::Blocks + MinGW)
Open `Hardware-AI.cbp`, select Release, press Build & Run. Use a 64-bit MinGW (e.g. MinGW-w64) so VRAM above 4 GB is reported correctly.

Command line:

    g++ -std=c++17 -O2 -mwindows -static -o Hardware-AI.exe src/*.cpp -lcomctl32
