# Iridium Engine

High-end C++20/Vulkan game engine using ECS and Jolt.

## Launch on Windows

After building, double-click `launch-engine.bat` at the repository root. The
launcher selects the current Release build, falls back to Debug when necessary,
and establishes the repository root as the working directory.

From a Visual Studio x64 developer PowerShell:

```powershell
cmake --preset x64-release
cmake --build out/build/x64-release
.\launch-engine.bat
```

The canonical executable is
`out/build/x64-release/bin/IridiumEngine.exe`. Older suffixed build directories
such as `x64-release-m1` and `x64-release-m3` are historical milestone builds and
should not be used for normal launches.
