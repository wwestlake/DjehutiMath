#pragma once

#include <JuceHeader.h>

#include <map>
#include <set>
#include <string>

// Detects an agent run that has stopped making progress: it keeps making the
// same tool call (same tool, equivalent arguments) and getting materially the
// same result back, with nothing new happening in between. This is not a
// call budget: any call whose result has not been seen before in the run is
// progress and resets the count, so long productive runs are unaffected.
namespace agent_progress
{
// The result with volatile fields (paths, times, durations, request ids)
// removed and re-serialised, so two runs of the same check compare equal.
inline juce::var stripVolatile(const juce::var& v)
{
    static const char* volatileKeys[] { "path", "file", "dir", "time", "when", "duration", "seconds", "timestamp", "requestid", "runid" };
    if (auto* object = v.getDynamicObject())
    {
        auto* copy = new juce::DynamicObject();
        for (const auto& prop : object->getProperties())
        {
            const auto key = prop.name.toString().toLowerCase();
            bool skip = false;
            for (auto k : volatileKeys)
                skip = skip || key.contains(k);
            if (!skip)
                copy->setProperty(prop.name, stripVolatile(prop.value));
        }
        return juce::var(copy);
    }
    if (auto* array = v.getArray())
    {
        juce::Array<juce::var> copy;
        for (const auto& item : *array)
            copy.add(stripVolatile(item));
        return copy;
    }
    return v;
}

inline juce::String normalise(const juce::String& text)
{
    const auto parsed = juce::JSON::parse(text);
    if (parsed.isObject() || parsed.isArray())
        return juce::JSON::toString(stripVolatile(parsed), true);
    return text.trim();
}

class NoProgressGuard
{
public:
    explicit NoProgressGuard(int repeatsToStop = 3) : limit(repeatsToStop) {}

    // Records one completed call; true when the run should stop.
    bool record(const juce::String& tool, const juce::String& argumentsJson, const juce::String& result)
    {
        const auto signature = (tool + "\n" + normalise(argumentsJson) + "\n" + normalise(result)).toStdString();
        if (seen.insert(signature).second)
        {
            counts.clear(); // something new happened
            counts[signature] = 1;
            return false;
        }
        const auto n = ++counts[signature];
        if (n < limit)
            return false;
        lastTool = tool;
        repeats = n;
        return true;
    }

    juce::String reason() const
    {
        return "Stopped for lack of progress: " + lastTool + " was called " + juce::String(repeats)
             + " times with the same arguments and returned the same result each time, and nothing else changed in between. "
               "Act on what the result reports (change the program, effect or settings) before calling it again.";
    }

private:
    int limit;
    std::set<std::string> seen;
    std::map<std::string, int> counts;
    juce::String lastTool;
    int repeats = 0;
};

// When an agent run is finished. A run that has used tools ends with an
// explicit outcome (agent_report_outcome: completed with evidence, failed,
// or blocked), not with any text reply: a statement of what the agent will do
// next is not an outcome. A text reply without an outcome is answered once
// with a request to continue or report; a second one with no work in between
// ends the run, marked as unreported. A "completed" claim is checked against
// each tool's latest result and pushed back once while failures remain.
// Questions answered without tools end normally.
class CompletionTracker
{
public:
    // A tool result: remembers whether each tool's latest call failed.
    void recordToolResult(const juce::String& tool, const juce::String& result)
    {
        const auto parsed = juce::JSON::parse(result);
        bool failed = false;
        juce::String why;
        if (parsed.isObject())
        {
            failed = (parsed.hasProperty("ok") && !(bool)parsed.getProperty("ok", true))
                  || (parsed.hasProperty("passed") && !(bool)parsed.getProperty("passed", true));
            why = parsed.getProperty("error", parsed.getProperty("interpretation", {})).toString();
        }
        latest[tool] = failed ? (why.isNotEmpty() ? why.substring(0, 300) : juce::String("failed")) : juce::String();
    }

    // Tools whose latest call failed: "tool: reason".
    juce::StringArray openFailures() const
    {
        juce::StringArray out;
        for (const auto& [tool, why] : latest)
            if (why.isNotEmpty())
                out.add(tool + ": " + why);
        return out;
    }

    // A reply that made tool calls.
    void onToolRound()
    {
        usedTools = true;
        workSinceRequest = true;
    }

    enum class TextReply { Final, AskForOutcome, FinalWithoutOutcome };

    // A reply with text and no tool calls.
    TextReply onTextReply()
    {
        if (!usedTools)
            return TextReply::Final;
        if (!requested || workSinceRequest)
        {
            requested = true;
            workSinceRequest = false;
            return TextReply::AskForOutcome;
        }
        return TextReply::FinalWithoutOutcome;
    }

    struct Review
    {
        bool accepted = false;
        juce::String message; // pushback when rejected, a caveat when accepted despite open failures
    };

    Review reviewOutcome(const juce::String& status, const juce::String& summary, const juce::String& evidence)
    {
        const auto s = status.trim().toLowerCase();
        if (s != "completed" && s != "failed" && s != "blocked")
            return { false, "status must be completed, failed or blocked." };
        if (summary.trim().isEmpty())
            return { false, "Give a summary of what was done and found." };
        if (s != "completed")
            return { true, {} };
        const auto failures = openFailures();
        const auto key = (failures.joinIntoString("\n") + (evidence.trim().isEmpty() ? "|no evidence" : "")).toStdString();
        if (failures.isEmpty() && evidence.trim().isNotEmpty())
            return { true, {} };
        if (pushedBack.insert(key).second)
        {
            juce::String why = "Not recorded as completed.";
            if (evidence.trim().isEmpty())
                why << " Give the evidence: the tool results that show the objective is met.";
            if (!failures.isEmpty())
                why << " These latest results still show failures: " << failures.joinIntoString("; ")
                    << ". Fix and re-verify them, or report status failed or blocked with the reason.";
            return { false, why };
        }
        return { true, "Reported as completed while these latest results still showed failures: " + failures.joinIntoString("; ") };
    }

private:
    std::map<juce::String, juce::String> latest;
    std::set<std::string> pushedBack;
    bool usedTools = false, requested = false, workSinceRequest = false;
};
}
