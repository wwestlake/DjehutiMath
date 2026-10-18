#pragma once

#include <JuceHeader.h>
#include "DesktopAuthSession.h"

class ProfileAvatarComponent final : public juce::Component,
                                     public juce::TooltipClient
{
public:
    explicit ProfileAvatarComponent(DesktopAuthSession& sessionToUse);
    ~ProfileAvatarComponent() override;

    void paint(juce::Graphics& g) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;

    int getPreferredWidth() const;
    juce::String getTooltip() override;

    bool isMenuOpen() const noexcept { return menuOpen; }
    void setMenuOpen(bool open) { menuOpen = open; repaint(); }

    void showAccountMenu();
    void showAccountInfoWindow();

    std::function<void()> onSessionChanged;

private:
    DesktopAuthSession& authSession;
    bool isHovered = false;
    bool menuOpen = false;

    juce::String getInitials() const;
    juce::String getDisplayLabel() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProfileAvatarComponent)
};
