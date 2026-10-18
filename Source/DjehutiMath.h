#pragma once
#include <JuceHeader.h>
#include <CreationDock/DockManager.h>
#include "Auth/DesktopAuthSession.h"
#include "Auth/ProfileAvatarComponent.h"
#include "DjehutiMenuBarComponent.h"
#include "WorksheetPanel.h"
#include "DjehutiAgent.h"
#include "FrustPanel.h"

class DjehutiMath final : public juce::Component, private juce::MenuBarModel, private juce::Timer
{
public:
    DjehutiMath();
    ~DjehutiMath() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void confirmCloseThen(std::function<void()>);
private:
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int, const juce::String&) override;
    void menuItemSelected(int, int) override;
    void timerCallback() override;
    void chooseFile(bool saveAs);
    void newWorksheet();
    void ifDiscardAllowed(std::function<void()>);
    void showAgentSettings();
    bool applyLayout(juce::var);
    void saveUserLayout();
    juce::var layoutCatalog() const;
    juce::String dispatch(const std::string&, const std::string&, std::shared_ptr<DjehutiAgentRequestState>);
    void refreshProject();
    juce::File stateDirectory() const;
    juce::ApplicationProperties properties;
    DesktopAuthSession auth { "djehuti-math" };
    ProfileAvatarComponent avatar { auth };
    DjehutiMenuBarComponent menu { this };
    std::unique_ptr<CreationDock::DockManager> dock;
    std::shared_ptr<WorksheetDocument> document = std::make_shared<WorksheetDocument>();
    WorksheetPanel* worksheet = nullptr;
    FrustPanel* frust = nullptr;
    juce::TextEditor* project = nullptr;
    juce::TextEditor* variables = nullptr;
    juce::TextEditor* diagnostics = nullptr;
    juce::File worksheetFile;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String lastLayout, persistedLayout, lastVariables;
    std::int64_t changedAt = 0;
};
