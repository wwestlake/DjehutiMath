#include "ProfileAvatarComponent.h"

ProfileAvatarComponent::ProfileAvatarComponent(DesktopAuthSession& sessionToUse)
    : authSession(sessionToUse)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);

    authSession.onAuthenticated = [this](const DesktopAuthSession::SessionData&) {
        repaint();
        if (onSessionChanged)
            onSessionChanged();
    };

    authSession.onSessionCleared = [this] {
        repaint();
        if (onSessionChanged)
            onSessionChanged();
    };

    authSession.onStatusChanged = [this](const juce::String&) {
        repaint();
    };
}

ProfileAvatarComponent::~ProfileAvatarComponent() = default;

void ProfileAvatarComponent::mouseEnter(const juce::MouseEvent&)
{
    isHovered = true;
    repaint();
}

void ProfileAvatarComponent::mouseExit(const juce::MouseEvent&)
{
    isHovered = false;
    repaint();
}

void ProfileAvatarComponent::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isLeftButtonDown())
    {
        if (menuOpen)
        {
            juce::PopupMenu::dismissAllActiveMenus();
            menuOpen = false;
            repaint();
            return;
        }

        juce::PopupMenu::dismissAllActiveMenus();
        showAccountMenu();
    }
}

juce::String ProfileAvatarComponent::getInitials() const
{
    const auto& session = authSession.getSession();
    juce::String name = session.user.displayName.trim();
    if (name.isEmpty())
        name = session.user.email.trim();

    if (name.isEmpty())
        return "U";

    auto parts = juce::StringArray::fromTokens(name, " ", "");
    if (parts.size() >= 2)
    {
        juce::String first = parts[0].trim();
        juce::String last = parts[parts.size() - 1].trim();
        juce::String init;
        if (first.isNotEmpty())
            init << first.substring(0, 1).toUpperCase();
        if (last.isNotEmpty())
            init << last.substring(0, 1).toUpperCase();
        return init.isNotEmpty() ? init : "U";
    }

    return name.substring(0, 1).toUpperCase();
}

juce::String ProfileAvatarComponent::getDisplayLabel() const
{
    const auto& session = authSession.getSession();
    if (session.user.displayName.isNotEmpty())
        return session.user.displayName;
    if (session.user.email.isNotEmpty())
        return session.user.email.upToFirstOccurrenceOf("@", false, false);
    return "User";
}

int ProfileAvatarComponent::getPreferredWidth() const
{
    const bool signedIn = authSession.hasValidSession();
    juce::Font font("Segoe UI", signedIn ? 13.5f : 13.0f, juce::Font::plain);
    juce::String label = signedIn ? getDisplayLabel() : "Sign In";
    int textWidth = font.getStringWidth(label);
    // avatar (24) + gap (8) + text + gap (6) + chevron (12) + margins (14)
    return juce::jlimit(96, 240, 24 + 8 + textWidth + 6 + 12 + 14);
}

juce::String ProfileAvatarComponent::getTooltip()
{
    if (authSession.hasValidSession())
    {
        const auto& session = authSession.getSession();
        juce::String tip = "Signed in to LagDaemon as " + session.user.displayName;
        if (session.user.email.isNotEmpty())
            tip << " (" << session.user.email << ")";
        return tip;
    }
    return "LagDaemon Account - Click to sign in";
}

void ProfileAvatarComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    if (menuOpen)
    {
        g.setColour(juce::Colour(0x38ffffff));
        g.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
    }
    else if (isHovered)
    {
        g.setColour(juce::Colour(0x20ffffff));
        g.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
    }

    const bool signedIn = authSession.hasValidSession();
    const float avatarSize = juce::jmin(bounds.getHeight() - 6.0f, 24.0f);
    const float avatarY = bounds.getY() + (bounds.getHeight() - avatarSize) * 0.5f;
    const float avatarX = bounds.getX() + 6.0f;
    auto avatarBounds = juce::Rectangle<float>(avatarX, avatarY, avatarSize, avatarSize);

    if (signedIn)
    {
        // Avatar circle with vibrant accent
        g.setColour(juce::Colour(0xff2563eb)); // Tailwind blue-600
        g.fillEllipse(avatarBounds);

        // Initials in avatar
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font("Segoe UI", avatarSize * 0.52f, juce::Font::bold));
        g.drawText(getInitials(), avatarBounds, juce::Justification::centred, false);

        // User Display Name
        auto textBounds = juce::Rectangle<float>(
            avatarBounds.getRight() + 8.0f,
            bounds.getY(),
            bounds.getWidth() - (avatarBounds.getRight() + 8.0f) - 18.0f,
            bounds.getHeight());

        g.setColour(menuOpen || isHovered ? juce::Colour(0xffffffff) : juce::Colour(0xffdce9ee));
        g.setFont(juce::Font("Segoe UI", 13.5f, juce::Font::plain));
        g.drawText(getDisplayLabel(), textBounds, juce::Justification::centredLeft, true);
    }
    else
    {
        // Signed out silhouette circle
        g.setColour(juce::Colour(0xff334155)); // Slate-700
        g.fillEllipse(avatarBounds);

        // Draw simple head & shoulders silhouette
        g.setColour(juce::Colour(0xff94a3b8));
        const float cx = avatarBounds.getCentreX();
        const float cy = avatarBounds.getCentreY();
        const float headR = avatarSize * 0.22f;
        g.fillEllipse(cx - headR, cy - headR * 1.6f, headR * 2.0f, headR * 2.0f);
        g.fillEllipse(cx - avatarSize * 0.32f, cy + avatarSize * 0.05f, avatarSize * 0.64f, avatarSize * 0.45f);

        // Text "Sign In"
        auto textBounds = juce::Rectangle<float>(
            avatarBounds.getRight() + 8.0f,
            bounds.getY(),
            bounds.getWidth() - (avatarBounds.getRight() + 8.0f) - 18.0f,
            bounds.getHeight());

        g.setColour(menuOpen || isHovered ? juce::Colour(0xfff1f5f9) : juce::Colour(0xff94a3b8));
        g.setFont(juce::Font("Segoe UI", 13.0f, juce::Font::plain));
        g.drawText("Sign In", textBounds, juce::Justification::centredLeft, true);
    }

    // Dropdown chevron indicator at the right
    const float chevronX = bounds.getRight() - 11.0f;
    const float chevronY = bounds.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(chevronX - 3.5f, chevronY - 1.5f);
    chevron.lineTo(chevronX, chevronY + 2.0f);
    chevron.lineTo(chevronX + 3.5f, chevronY - 1.5f);
    g.setColour(menuOpen || isHovered ? juce::Colour(0xffcbd5e1) : juce::Colour(0xff94a3b8));
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
}

void ProfileAvatarComponent::showAccountMenu()
{
    menuOpen = true;
    repaint();

    juce::PopupMenu menu;
    if (authSession.hasValidSession())
    {
        const auto& session = authSession.getSession();
        juce::String header = session.user.displayName.isNotEmpty()
            ? session.user.displayName
            : session.user.email;
        menu.addSectionHeader(header);
        menu.addItem(1, "Account information");
        menu.addSeparator();
        menu.addItem(2, "Sign out");

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [this](int result)
            {
                menuOpen = false;
                repaint();

                if (result == 1)
                    showAccountInfoWindow();
                else if (result == 2)
                {
                    authSession.clearSession();
                    repaint();
                    if (onSessionChanged)
                        onSessionChanged();
                }
            });
    }
    else
    {
        menu.addItem(1, "Sign in to LagDaemon");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [this](int result)
            {
                menuOpen = false;
                repaint();

                if (result == 1)
                    authSession.beginLogin();
            });
    }
}

void ProfileAvatarComponent::showAccountInfoWindow()
{
    if (!authSession.hasValidSession())
        return;

    const auto& session = authSession.getSession();
    juce::String details;
    details << "Name: " << (session.user.displayName.isNotEmpty() ? session.user.displayName : "(not specified)") << "\n";
    details << "Email: " << (session.user.email.isNotEmpty() ? session.user.email : "(not specified)") << "\n";
    if (session.user.role.isNotEmpty())
        details << "Role: " << session.user.role << "\n";
    if (!session.user.entitlements.isEmpty())
        details << "Entitlements: " << session.user.entitlements.joinIntoString(", ") << "\n";
    if (session.expiresAt > 0)
    {
        juce::Time expiry(session.expiresAt);
        details << "Session Valid Until: " << expiry.formatted("%Y-%m-%d %H:%M:%S") << "\n";
    }

    auto* alert = new juce::AlertWindow("LagDaemon Account", details, juce::AlertWindow::InfoIcon);
    alert->addButton("Sign Out", 2);
    alert->addButton("Close", 1, juce::KeyPress(juce::KeyPress::escapeKey));

    alert->enterModalState(true, juce::ModalCallbackFunction::create([this, alert](int result) {
        std::unique_ptr<juce::AlertWindow> owner(alert);
        if (result == 2)
        {
            authSession.clearSession();
            repaint();
            if (onSessionChanged)
                onSessionChanged();
        }
    }));
}
