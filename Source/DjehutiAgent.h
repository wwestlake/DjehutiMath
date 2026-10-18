#pragma once

#include <JuceHeader.h>
#include <ai_provider/AiProvider.h>
#include <memory>
#include <thread>
#include <vector>

struct DjehutiAgentRequestState
{
    std::atomic_bool cancelled { false };
    std::atomic_bool alive { true };
};

class DjehutiAgentPanel final : public juce::Component
{
public:
    explicit DjehutiAgentPanel(juce::ApplicationProperties& properties,
                               std::function<std::string(const std::string&, const std::string&, std::shared_ptr<DjehutiAgentRequestState>)> dispatchTool);
    ~DjehutiAgentPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void sendMessage();
    void appendTranscript(const juce::String& speaker, const juce::String& text);
    void setBusy(bool value);
    juce::ApplicationProperties& properties;
    juce::TextEditor transcript;
    juce::TextEditor input;
    juce::TextButton send { "Send" }, cancelButton { "Cancel" };
    std::unique_ptr<std::thread> worker;
    std::shared_ptr<DjehutiAgentRequestState> request;
    std::vector<ai_provider::ChatMessage> history;
    std::function<std::string(const std::string&, const std::string&, std::shared_ptr<DjehutiAgentRequestState>)> dispatchTool;
    bool busy = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DjehutiAgentPanel)
};

class DjehutiAgentSettings final : public juce::Component
{
public:
    DjehutiAgentSettings(juce::ApplicationProperties& properties, std::function<void()> onSaved);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void save();
    juce::ApplicationProperties& properties;
    std::function<void()> onSaved;
    juce::Label providerLabel, profileLabel, modelLabel, keyLabel, status;
    juce::ComboBox provider;
    juce::TextEditor profile, model, key;
    juce::TextButton saveButton { "Save" }, removeButton { "Remove key" }, cancelButton { "Close" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DjehutiAgentSettings)
};
