#pragma once

enum class Sample_Stage
{
    Started,
    TargetSelected,
    PolicyChecked,
    ModulePrepared,
    ActionWouldOccur,
    Completed,
};

using Sample_EventCallback = void(__cdecl*)(Sample_Stage stage, const char* message, void* context);

extern "C" __declspec(dllexport) int __cdecl Sample_RunSimulation(
    Sample_EventCallback callback,
    void* context) noexcept;
