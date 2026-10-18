#include "DjehutiMenuBarComponent.h"

//==============================================================================
DjehutiHeaderLookAndFeel::DjehutiHeaderLookAndFeel()
{
}

int DjehutiHeaderLookAndFeel::getDefaultMenuBarHeight()
{
    return 42;
}

juce::Font DjehutiHeaderLookAndFeel::getMenuBarFont(juce::MenuBarComponent&, int, const juce::String&)
{
    return juce::Font("Segoe UI", 14.5f, juce::Font::plain);
}

int DjehutiHeaderLookAndFeel::getMenuBarItemWidth(juce::MenuBarComponent& bar, int itemIndex, const juce::String& itemText)
{
    return getMenuBarFont(bar, itemIndex, itemText).getStringWidth(itemText) + 28;
}

void DjehutiHeaderLookAndFeel::drawMenuBarBackground(juce::Graphics& g, int, int, bool, juce::MenuBarComponent&)
{
    g.fillAll(juce::Colour(0xff1b2028));
}

void DjehutiHeaderLookAndFeel::drawMenuBarItem(juce::Graphics& g, int width, int height,
                                               int itemIndex, const juce::String& itemText,
                                               bool isMouseOverItem, bool isMenuOpen,
                                               bool, juce::MenuBarComponent& menuBar)
{
    if (isMenuOpen)
    {
        g.setColour(juce::Colour(0xff33424d));
        g.fillRoundedRectangle(juce::Rectangle<float>(2.0f, 5.0f, (float)width - 4.0f, (float)height - 10.0f), 4.0f);
        g.setColour(juce::Colours::white);
    }
    else if (isMouseOverItem)
    {
        g.setColour(juce::Colour(0xff26303c));
        g.fillRoundedRectangle(juce::Rectangle<float>(2.0f, 5.0f, (float)width - 4.0f, (float)height - 10.0f), 4.0f);
        g.setColour(juce::Colour(0xffe2e8f0));
    }
    else
    {
        g.setColour(juce::Colour(0xff94a3b8));
    }

    g.setFont(getMenuBarFont(menuBar, itemIndex, itemText));
    g.drawFittedText(itemText, 0, 0, width, height, juce::Justification::centred, 1);
}

//==============================================================================
class DjehutiMenuBarComponent::MenuHeadingComponent : public juce::Component
{
public:
    MenuHeadingComponent(DjehutiMenuBarComponent& ownerIn, int itemIndexIn, const juce::String& textIn)
        : owner(ownerIn), itemIndex(itemIndexIn), itemText(textIn)
    {
        setWantsKeyboardFocus(false);
        setMouseClickGrabsKeyboardFocus(false);
    }

    ~MenuHeadingComponent() override = default;

    void paint(juce::Graphics& g) override
    {
        auto& lf = owner.getLookAndFeel();
        const bool isOpen = (itemIndex == owner.getActiveMenuIndex());
        const bool isHovered = isMouseOver() && (owner.getActiveMenuIndex() < 0);
        lf.drawMenuBarItem(g, getWidth(), getHeight(), itemIndex, itemText, isHovered, isOpen, owner.isMouseOver(true), owner);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown())
        {
            if (owner.getActiveMenuIndex() == itemIndex)
            {
                owner.closeActiveMenu();
                return;
            }

            owner.openMenu(itemIndex);
        }
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        if (owner.getActiveMenuIndex() < 0)
            repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (owner.getActiveMenuIndex() < 0)
            repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        return owner.keyPressed(key);
    }

    int getItemIndex() const noexcept { return itemIndex; }
    const juce::String& getItemText() const noexcept { return itemText; }

private:
    DjehutiMenuBarComponent& owner;
    int itemIndex;
    juce::String itemText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MenuHeadingComponent)
};

//==============================================================================
DjehutiMenuBarComponent::DjehutiMenuBarComponent(juce::MenuBarModel* model)
    : juce::MenuBarComponent(model),
      headerLookAndFeel(std::make_unique<DjehutiHeaderLookAndFeel>())
{
    setLookAndFeel(headerLookAndFeel.get());
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);

    rebuildHeadings();
}

DjehutiMenuBarComponent::~DjehutiMenuBarComponent()
{
    setLookAndFeel(nullptr);
}

int DjehutiMenuBarComponent::getActiveMenuIndex() const noexcept
{
    return activeMenuIndex;
}

void DjehutiMenuBarComponent::setActiveMenuIndex(int idx)
{
    activeMenuIndex = idx;
    repaintHeadings();
}

int DjehutiMenuBarComponent::getItemIndexAt(juce::Point<int> p) const
{
    for (size_t i = 0; i < headingComponents.size(); ++i)
    {
        if (headingComponents[i]->getBounds().contains(p))
            return (int) i;
    }
    return -1;
}

juce::Rectangle<int> DjehutiMenuBarComponent::getItemBounds(int itemIndex) const
{
    if (juce::isPositiveAndBelow(itemIndex, (int) headingComponents.size()))
        return headingComponents[(size_t) itemIndex]->getBounds();

    return {};
}

void DjehutiMenuBarComponent::openMenu(int itemIndex)
{
    if (itemIndex < 0)
    {
        closeActiveMenu();
        return;
    }

    auto* m = getModel();
    if (m == nullptr)
        return;

    const auto names = m->getMenuBarNames();
    if (!juce::isPositiveAndBelow(itemIndex, names.size()) || itemIndex >= (int) headingComponents.size())
    {
        closeActiveMenu();
        return;
    }

    if (activeMenuIndex < 0)
        m->handleMenuBarActivate(true);

    // Dismiss any currently open popup menu
    juce::PopupMenu::dismissAllActiveMenus();

    activeMenuIndex = itemIndex;
    repaintHeadings();

    const int seq = ++currentMenuSequence;

    auto menu = m->getMenuForIndex(itemIndex, names[itemIndex]);
    menu.setLookAndFeel(&getLookAndFeel());

    auto* targetComp = headingComponents[(size_t) itemIndex].get();
    const auto itemBounds = targetComp->getBounds();
    const auto screenBounds = localAreaToGlobal(itemBounds);

    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(targetComp)
            .withTargetScreenArea(screenBounds)
            .withMinimumWidth(itemBounds.getWidth()),
        [safeThis = juce::Component::SafePointer<DjehutiMenuBarComponent>(this), itemIndex, seq](int result)
        {
            if (safeThis == nullptr || seq != safeThis->currentMenuSequence)
                return;

            safeThis->menuDismissed(itemIndex, result);
        });
}

void DjehutiMenuBarComponent::closeActiveMenu()
{
    ++currentMenuSequence;
    const bool wasActive = (activeMenuIndex >= 0);
    activeMenuIndex = -1;
    juce::PopupMenu::dismissAllActiveMenus();
    repaintHeadings();

    if (wasActive)
    {
        if (auto* m = getModel())
            m->handleMenuBarActivate(false);
    }
}

void DjehutiMenuBarComponent::menuDismissed(int itemIndex, int result)
{
    if (activeMenuIndex == itemIndex)
    {
        activeMenuIndex = -1;
        repaintHeadings();

        if (auto* m = getModel())
            m->handleMenuBarActivate(false);
    }

    if (result != 0)
    {
        if (auto* m = getModel())
            m->menuItemSelected(result, itemIndex);
    }
}

void DjehutiMenuBarComponent::paint(juce::Graphics& g)
{
    auto& lf = getLookAndFeel();
    lf.drawMenuBarBackground(g, getWidth(), getHeight(), isMouseOver(true), *this);
}

void DjehutiMenuBarComponent::resized()
{
    juce::MenuBarComponent::resized();

    auto* m = getModel();
    if (m == nullptr)
        return;

    auto& lf = getLookAndFeel();
    const auto names = m->getMenuBarNames();
    int x = 0;

    for (size_t i = 0; i < headingComponents.size(); ++i)
    {
        if (i < (size_t) names.size())
        {
            const int w = lf.getMenuBarItemWidth(*this, (int) i, names[(int) i]);
            headingComponents[i]->setBounds(x, 0, w, getHeight());
            x += w;
        }
    }
}

void DjehutiMenuBarComponent::menuBarItemsChanged(juce::MenuBarModel*)
{
    rebuildHeadings();
    repaintHeadings();
}

void DjehutiMenuBarComponent::rebuildHeadings()
{
    headingComponents.clear();

    auto* m = getModel();
    if (m == nullptr)
        return;

    const auto names = m->getMenuBarNames();
    for (int i = 0; i < names.size(); ++i)
    {
        auto heading = std::make_unique<MenuHeadingComponent>(*this, i, names[i]);
        addAndMakeVisible(*heading);
        headingComponents.push_back(std::move(heading));
    }

    resized();
}

void DjehutiMenuBarComponent::repaintHeadings()
{
    for (auto& h : headingComponents)
        h->repaint();
    repaint();
}

void DjehutiMenuBarComponent::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo(this).getPosition();
    const int item = getItemIndexAt(p);
    if (item >= 0)
    {
        if (activeMenuIndex == item)
            closeActiveMenu();
        else
            openMenu(item);
    }
    else
    {
        closeActiveMenu();
    }
}

void DjehutiMenuBarComponent::mouseUp(const juce::MouseEvent&)
{
}

void DjehutiMenuBarComponent::mouseMove(const juce::MouseEvent&)
{
}

void DjehutiMenuBarComponent::mouseDrag(const juce::MouseEvent&)
{
}

void DjehutiMenuBarComponent::mouseEnter(const juce::MouseEvent&)
{
}

void DjehutiMenuBarComponent::mouseExit(const juce::MouseEvent&)
{
}

void DjehutiMenuBarComponent::handleCommandMessage(int commandId)
{
    const int lastIdx = (activeMenuIndex >= 0 ? activeMenuIndex : 0);
    closeActiveMenu();

    if (commandId != 0)
    {
        if (auto* m = getModel())
            m->menuItemSelected(commandId, lastIdx);
    }
}

bool DjehutiMenuBarComponent::keyPressed(const juce::KeyPress& key)
{
    auto* m = getModel();
    if (m == nullptr)
        return false;

    const auto names = m->getMenuBarNames();
    const int numMenus = names.size();
    if (numMenus <= 0)
        return false;

    if (activeMenuIndex >= 0)
    {
        if (key.isKeyCode(juce::KeyPress::leftKey))
        {
            openMenu((activeMenuIndex + numMenus - 1) % numMenus);
            return true;
        }

        if (key.isKeyCode(juce::KeyPress::rightKey))
        {
            openMenu((activeMenuIndex + 1) % numMenus);
            return true;
        }

        if (key.isKeyCode(juce::KeyPress::escapeKey))
        {
            closeActiveMenu();
            return true;
        }
    }

    return false;
}
