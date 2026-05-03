@echo off
echo [Cortex Build System] Starting Industrial Compilation...

REM Check for G++
where g++ >nul 2>nul
if %errorlevel% neq 0 (
    echo [Error] G++ not found in PATH. Please install MinGW-w64.
    exit /b 1
)

echo [1/3] Compiling Internal Mind (Port 9090)...
g++ -std=c++17 internal_brain/mind.cpp -o internal_brain/mind.exe -lws2_32 -pthread
if %errorlevel% neq 0 (
    echo [Error] Mind compilation failed.
    exit /b 1
)

echo [2/3] Compiling Cortex Core (Port 8080)...
g++ -std=c++17 cortex.cpp -o cortex.exe -lws2_32 -pthread
if %errorlevel% neq 0 (
    echo [Error] Core compilation failed.
    exit /b 1
)

echo [3/3] Compiling Standalone Mapper...
g++ -std=c++17 mapper.cpp -o mapper.exe -lws2_32 -pthread
if %errorlevel% neq 0 (
    echo [Error] Mapper compilation failed.
    exit /b 1
)

echo [Success] All components built and hardened for industrial use.
echo Launch mind.exe first, then cortex.exe.
