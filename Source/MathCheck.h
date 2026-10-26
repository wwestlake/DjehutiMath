#pragma once

#include <JuceHeader.h>

// djehuti_math.exe --math-check <report.json>: focused checks of the structured mathematics (document model,
// linear input, persistence, layout and worksheet integration). Also writes math-gallery.png beside the report.
bool mathCheck(const juce::File& destination);
