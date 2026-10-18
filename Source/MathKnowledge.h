#pragma once
#include <JuceHeader.h>
#include <ai_provider/AiProvider.h>
namespace math_knowledge
{
juce::File root();
juce::String contextForQuery(const juce::String&, int maxCards = 6);
std::vector<ai_provider::ToolDefinition> tools();
}
