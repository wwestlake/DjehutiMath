#pragma once

#include <JuceHeader.h>

// djehuti_math.exe --mathir-check <report.json>: focused checks of the S0 computational foundation (MathIR,
// frontend, units and dimensions, dependencies, revision-aware results, the solver interface, result display
// and document compatibility).
bool mathIrCheck(const juce::File& destination);
