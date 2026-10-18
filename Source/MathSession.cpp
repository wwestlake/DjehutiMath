#include "MathSession.h"
#include <cmath>
MathSession& MathSession::instance() { static MathSession value; return value; }
bool MathSession::setScalar(const juce::String& name, double value)
{
    if (name.isEmpty() || name.length() > 128 || !std::isfinite(value)) return false;
    for (auto c : name) if (!juce::CharacterFunctions::isLetterOrDigit(c) && c != '_') return false;
    std::lock_guard lock(mutex); scalars[name] = value; return true;
}
double MathSession::scalar(const juce::String& name) const
{
    std::lock_guard lock(mutex);
    const auto found = scalars.find(name);
    return found == scalars.end() ? std::numeric_limits<double>::quiet_NaN() : found->second;
}
juce::var MathSession::inspect() const
{
    std::lock_guard lock(mutex); auto* result = new juce::DynamicObject();
    for (const auto& [name, value] : scalars) result->setProperty(juce::Identifier(name), value);
    return juce::var(result);
}
void MathSession::clear() { std::lock_guard lock(mutex); scalars.clear(); }
