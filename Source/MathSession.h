#pragma once
#include <JuceHeader.h>
#include <map>
#include <mutex>
#include <limits>

// Initial scalar workspace. Advanced mathematical values and solver backends
// will extend the domain layer, rather than being implemented in UI widgets.
class MathSession
{
public:
    static MathSession& instance();
    bool setScalar(const juce::String& name, double value);
    double scalar(const juce::String& name) const;
    juce::var inspect() const;
    void clear();
private:
    mutable std::mutex mutex;
    std::map<juce::String, double> scalars;
};
