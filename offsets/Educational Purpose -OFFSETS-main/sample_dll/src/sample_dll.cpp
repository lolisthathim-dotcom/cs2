#include "sample_dll.h"

namespace
{
    struct SimulationEvent
    {
        Sample_Stage stage;
        const char* message;
    };

    constexpr SimulationEvent events[] = {
        {Sample_Stage::Started, "Simulation started; no process was opened."},
        {Sample_Stage::TargetSelected, "Placeholder target: PlaceholderTarget.exe"},
        {Sample_Stage::PolicyChecked, "Authorization check passed for this offline demo."},
        {Sample_Stage::ModulePrepared, "Placeholder module: PlaceholderModule.dll (not loaded)."},
        {Sample_Stage::ActionWouldOccur, "A real process operation would occur here; intentionally omitted."},
        {Sample_Stage::Completed, "Simulation completed; no process or memory was modified."},
    };
}

extern "C" __declspec(dllexport) int __cdecl Sample_RunSimulation(
    Sample_EventCallback callback,
    void* context) noexcept
{
    if (callback == nullptr)
    {
        return 1;
    }

    for (const auto& event : events)
    {
        callback(event.stage, event.message, context);
    }

    return 0;
}
