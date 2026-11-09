# Sample Windows DLL: Offline Process-Flow Simulation

This x64 DLL demonstrates a high-level, callback-driven sequence using
placeholder names. It is an offline simulation only: it does not open or modify
another process, load a module into one, or perform process injection.

The exported `Sample_RunSimulation` function reports these illustrative stages:

1. Simulation started
2. Placeholder target selected
3. Authorization check passed for the demo
4. Placeholder module prepared (not loaded)
5. Operation intentionally omitted
6. Simulation completed

## Build

Open a Visual Studio Developer PowerShell, then run these commands from this
directory:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The DLL will be at `build\Release\sample_dll.dll`. The C++ API declaration and
callback types are in `src\sample_dll.h`.
