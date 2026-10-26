#pragma once

#include <JuceHeader.h>

// djehuti_math.exe --dsp-check <report.json>: functional checks of the node, audio, DSP and VST3 capabilities.
bool dspCheck(const juce::File& destination);
