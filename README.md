# 🧠 Cortex: Industrial-Scale Engineering Mind

Cortex has evolved from a simple archetype expander into a high-performance, **Dual-Layer Engineering Engine**. It is designed to handle enterprise-scale codebases (up to 10GB) with a focus on autonomous research, impact analysis, and constructive feature grafting.

## 🚀 The Dual-Brain Architecture

Cortex operates using two distinct but synchronized layers:

### 1. The Subconscious (Internal Mind)
Located in `internal_brain/`, this is the high-performance research engine.
- **Port**: 9090 (Private)
- **Parallel Scanning**: Multi-threaded worker pool that scales with your CPU cores.
- **Blast Radius Analysis**: Predicts how a change in one file will affect the entire global dependency graph.
- **Semantic Intent**: Remembers the "Why" behind code, tracking business logic and security constraints.
- **Self-Healing**: Autonomous verification loops that check system integrity after every change.

### 2. The Frontal Lobe (Cortex Core)
The primary entry point for code expansion and feature integration.
- **Port**: 8080
- **Risk Assessment**: Consults the Internal Mind before every expansion to ensure zero breaking changes.
- **Constructive Grafting**: Merges new features into existing UI patterns instead of just generating isolated files.

---

## ✨ Key Features

- **Industrial Scale**: Optimized to scan 10GB+ projects in seconds using parallel C++ streams.
- **Fail-Safe Reliability**: Hardened with pervasive exception handling and binary-mode streaming to prevent crashes on malformed files.
- **Chronos History**: Maintains a persistent, timestamped log of every engineering decision and simulation.
- **Meta-Consciousness**: Includes a `/meta_scan` feature where Cortex audits its own architecture for loopholes.
- **User Tooling**: Includes a standalone `mapper.cpp` utility for manual codebase exploration and documentation generation.

## 🛠️ Technical Stack

- **Language**: C++17 (Hardened for concurrency)
- **Networking**: `yhirose/cpp-httplib` with timeout protection.
- **Data Engine**: `nlohmann/json` for complex state management.
- **Concurrency**: Custom `WorkerPool` for high-throughput file analysis.

## 🚦 Getting Started

### Prerequisites
- C++17 compatible compiler (GCC 9+, Clang, or MSVC).
- `lws2_32` (Winsock) for networking.

### Industrial Compilation
```bash
# Compile the Mind
g++ -std=c++17 internal_brain/mind.cpp -o internal_brain/mind.exe -lws2_32

# Compile the Core
g++ -std=c++17 cortex.cpp -o cortex.exe -lws2_32

# Compile the User Tool
g++ -std=c++17 mapper.cpp -o mapper.exe -lws2_32
```

### Running the Ecosystem
For full "Constructive Engineering" mode, launch the Mind first:
1. `./internal_brain/mind.exe` (Initializes the subconscious)
2. `./cortex.exe` (Activates the primary expansion engine)

---

## 📂 Project Structure

- `/internal_brain`: The private engine (subconscious) for research and memory.
- `cortex.cpp`: The core API for archetype expansion.
- `mapper.cpp`: Standalone industrial-scale codebase mapper for the user.
- `archetypes.json`: The pattern database for UI and system structures.

---
*Cortex: Moving beyond vibe-coding into autonomous, system-aware engineering.*
