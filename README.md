# 🧠 Cortex: Archetype Expansion Server

Cortex is a lightweight, C++ powered backend designed to bridge the gap between architectural "archetypes" and functional boilerplate code. It serves as an intelligent expansion engine that identifies code patterns and generates the necessary scaffolding for modern UI components.

## 🚀 What is Cortex?

At its core, Cortex is an API server that takes a component name and a brief description, then uses a set of pre-defined **Archetypes** to determine the best structure for that component. 

Instead of writing every `useState` or `useEffect` by hand, you tell Cortex what you want, and it returns the "injected" logic, required imports, and state management stubs needed to get the job done.

## ✨ Key Features

- **Pattern Matching Engine**: Scans component names and descriptions against keywords to find the most relevant architectural archetype (e.g., Dashboards, Forms, List Views).
- **Dynamic Scaffolding**: Automatically injects logic and state variables based on the matched archetype.
- **Dependency Awareness**: Provides a list of required imports (like React hooks or external libraries) specific to the generated code.
- **Ultra-Lightweight**: Built with `yhirose/cpp-httplib`, it's a single-binary server with zero heavy dependencies.

## 🛠️ Technical Stack

- **Language**: C++17
- **Server**: [cpp-httplib](https://github.com/yhirose/cpp-httplib) for high-performance HTTP handling.
- **JSON Engine**: [nlohmann/json](https://github.com/nlohmann/json) for seamless data interchange.

## 🚦 Getting Started

### Prerequisites
- A C++17 compatible compiler (GCC, Clang, or MSVC).
- `winsock2` (for Windows users).

### Compilation
To compile the server on Windows (MinGW/GCC):
```bash
g++ -std=c++17 cortex.cpp -o cortex.exe -lws2_32
```

### Running the Server
1. Ensure `archetypes.json` is in the same directory.
2. Launch the binary:
   ```bash
   ./cortex.exe
   ```
3. The server will start at `http://localhost:8080`.

## 📂 Project Structure

- `cortex.cpp`: The core server logic and expansion engine.
- `archetypes.json`: The "brain" containing the templates and keywords for different code structures.
- `test.html`: A built-in UI to test expansions directly in your browser.
- `httplib.h` / `json.hpp`: Essential headers for networking and data handling.

## 🤝 Contributing

Cortex is built to be modular. You can extend its capabilities simply by adding new patterns and templates to `archetypes.json`—no recompilation required for new architectural patterns!

---
*Built with ❤️ for engineers who value architectural consistency.*
