#pragma once

#include <JuceHeader.h>
#include <vector>
#include <memory>

class DjehutiHeaderLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DjehutiHeaderLookAndFeel();
    ~DjehutiHeaderLookAndFeel() override = default;

    int getDefaultMenuBarHeight() override;
    juce::Font getMenuBarFont(juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;
    int getMenuBarItemWidth(juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;
    void drawMenuBarBackground(juce::Graphics&, int width, int height, bool isMouseOverBar, juce::MenuBarComponent&) override;
    void drawMenuBarItem(juce::Graphics&, int width, int height,
                         int itemIndex, const juce::String& itemText,
                         bool isMouseOverItem, bool isMenuOpen,
                         bool isMouseOverBar, juce::MenuBarComponent&) override;
};

class DjehutiMenuBarComponent : public juce::MenuBarComponent
{
public:
    class MenuHeadingComponent;

    explicit DjehutiMenuBarComponent(juce::MenuBarModel* model = nullptr);
    ~DjehutiMenuBarComponent() override;

    int getActiveMenuIndex() const noexcept;
    void setActiveMenuIndex(int idx);

    int getItemIndexAt(juce::Point<int> p) const;
    juce::Rectangle<int> getItemBounds(int itemIndex) const;

    void openMenu(int itemIndex);
    void closeActiveMenu();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void menuBarItemsChanged(juce::MenuBarModel* newModel) override;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void handleCommandMessage(int commandId) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void rebuildHeadings();
    void repaintHeadings();

private:
    void menuDismissed(int itemIndex, int result);

    std::unique_ptr<DjehutiHeaderLookAndFeel> headerLookAndFeel;
    std::vector<std::unique_ptr<MenuHeadingComponent>> headingComponents;
    int activeMenuIndex = -1;
    int currentMenuSequence = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DjehutiMenuBarComponent)
};
