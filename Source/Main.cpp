#include <JuceHeader.h>
#include "DjehutiMath.h"
#include "DspCheck.h"
#include "MathCheck.h"
#include "MathKnowledge.h"
#include "MathSession.h"
#include "ProtectedCredentialStore.h"
#include <cmath>
#include <cstdlib>
#include <deque>

namespace
{
bool foundationCheck(const juce::File& destination)
{
    juce::Array<juce::var> checks; bool passed = true;
    auto check = [&](bool ok, const juce::String& name, const juce::String& detail = {}) { auto* value = new juce::DynamicObject(); value->setProperty("name", name); value->setProperty("passed", ok); value->setProperty("detail", detail); checks.add(juce::var(value)); passed = passed && ok; };
    WorksheetDocument worksheet; worksheet.reset(); juce::String error;
    const auto file = destination.getSiblingFile("worksheet-check-" + juce::Uuid().toString() + ".djmath");
    const auto original = juce::JSON::toString(worksheet.toJson());
    check(worksheet.save(file, error), "Save versioned worksheet", error);
    WorksheetDocument reopened; check(reopened.load(file, error) && juce::JSON::toString(reopened.toJson()) == original, "Reopen worksheet preserves cell identities and sources", error);
    file.replaceWithText("{\"format\":\"other\"}"); check(!reopened.load(file, error) && juce::JSON::toString(reopened.toJson()) == original, "Reject invalid document without replacing current worksheet", error); file.deleteFile();
    const auto context = math_knowledge::contextForQuery("FRust worksheet scalar variables");
    check(context.contains("math_set_scalar") && !math_knowledge::tools().empty(), "Retrieve LiteSemRAG reference cards and matching live tools");
    // Drive the actual Run-cell command and worker. The test poster queues UI
    // notifications for this main thread, avoiding a second executable or GUI
    // automation dependency. Wait for notifications, not repeated process polls.
    auto& executor = frust_exec::Executor::instance();
    struct Queue { std::mutex mutex; std::deque<std::function<void()>> pending; juce::WaitableEvent changed; };
    auto queue = std::make_shared<Queue>();
    executor.setPoster([queue](std::function<void()> action) { { std::lock_guard lock(queue->mutex); queue->pending.push_back(std::move(action)); } queue->changed.signal(); });
    {
        auto live = std::make_shared<WorksheetDocument>(worksheet); WorksheetPanel panel(live);
        bool terminalDelivered = false;
        const auto notification = executor.addListener([&](const frust_exec::Snapshot& state) { if (!frust_exec::isActive(state.state)) terminalDelivered = true; });
        const bool started = panel.runCell(live->cells.back().id, error);
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (started && !terminalDelivered && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            queue->changed.wait(1000); std::deque<std::function<void()>> pending;
            { std::lock_guard lock(queue->mutex); pending.swap(queue->pending); }
            for (auto& action : pending) action();
        }
        std::deque<std::function<void()>> pending;
        { std::lock_guard lock(queue->mutex); pending.swap(queue->pending); }
        for (auto& action : pending) action();
        const auto completed = executor.snapshot();
        executor.removeListener(notification);
        check(started && completed.state == frust_exec::State::Completed && completed.result.ok && std::abs(MathSession::instance().scalar("answer") - 42.0) < 1.0e-12 && live->cells.back().output.contains("Completed"), "Actual Run-cell command compiles and executes FRust on the shared worker and publishes output", started ? juce::String(completed.result.output + completed.result.report()) : error);
    }
    executor.setPoster([](std::function<void()> action) { juce::MessageManager::callAsync(std::move(action)); });
    check(!MathSession::instance().setScalar("bad variable", 3.0) && std::isnan(MathSession::instance().scalar("missing")), "Invalid scalar names and missing values stay explicit");
    // A unique disposable dummy credential; never enumerate or read real profiles.
    const auto credential = "foundation-check/" + juce::Uuid().toString().toStdString(); std::string why, recovered;
    const bool stored = DjehutiCredentials::write(credential, "disposable-test-value", why);
    const bool restored = stored && DjehutiCredentials::read(credential, recovered, why);
    const bool removed = stored && DjehutiCredentials::remove(credential, why);
    check(restored && recovered == "disposable-test-value" && removed, "DPAPI dummy-key round trip in Math-specific storage", why);
    auto* report = new juce::DynamicObject(); report->setProperty("passed", passed); report->setProperty("checks", checks);
    destination.getParentDirectory().createDirectory(); destination.replaceWithText(juce::JSON::toString(juce::var(report), false)); return passed;
}
}
class DjehutiMathApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Djehuti Math"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String& commandLine) override
    {
        if (commandLine.startsWith("--foundation-check"))
        {
            const auto arguments = juce::StringArray::fromTokens(commandLine, true);
            const auto report = arguments.size() > 1 ? juce::File(arguments[1].unquoted()) : juce::File::getCurrentWorkingDirectory().getChildFile("build/foundation-check.json");
            std::exit(foundationCheck(report) ? 0 : 1);
        }
        if (commandLine.startsWith("--math-check"))
        {
            const auto arguments = juce::StringArray::fromTokens(commandLine, true);
            const auto report = arguments.size() > 1 ? juce::File(arguments[1].unquoted()) : juce::File::getCurrentWorkingDirectory().getChildFile("build/math-check.json");
            std::exit(mathCheck(report) ? 0 : 1);
        }
        if (commandLine.startsWith("--dsp-check"))
        {
            const auto arguments = juce::StringArray::fromTokens(commandLine, true);
            const auto report = arguments.size() > 1 ? juce::File(arguments[1].unquoted()) : juce::File::getCurrentWorkingDirectory().getChildFile("build/dsp-check.json");
            std::exit(dspCheck(report) ? 0 : 1);
        }
        window = std::make_unique<Window>();
    }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { frust_exec::Executor::instance().shutdown([this] { quit(); }, 3000); }
    void anotherInstanceStarted(const juce::String&) override { if (window != nullptr) { window->setMinimised(false); window->toFront(true); } }
private:
    class Window final : public juce::DocumentWindow
    {
    public:
        Window() : DocumentWindow("Djehuti Math", juce::Colour(0xff101720), allButtons)
        {
            setUsingNativeTitleBar(true); if (const auto icon = DjehutiMath::brandImage(64); icon.isValid()) setIcon(icon); setContentOwned(new DjehutiMath(), true); setResizable(true, true); setResizeLimits(1050, 700, 3840, 2160); centreWithSize(1500, 950); setVisible(true);
        }
        void closeButtonPressed() override { if (auto* app = dynamic_cast<DjehutiMath*>(getContentComponent())) app->confirmCloseThen([] { JUCEApplication::getInstance()->systemRequestedQuit(); }); }
    };
    std::unique_ptr<Window> window;
};
START_JUCE_APPLICATION(DjehutiMathApplication)
