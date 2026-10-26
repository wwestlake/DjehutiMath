#pragma once

// A local, token-protected HTTP interface to the application's command
// service, so an external agent (Codex, Claude, a script) can do real work
// without simulated mouse and keyboard input. It runs the same tools the
// built-in agent uses; no model is involved. Adapted from DEWB's
// LocalAgentApi: Math exposes status, the tool catalog and direct tool calls.
//
//   GET  /v1/status        -> {"status":"ready", ...}
//   GET  /v1/tools         -> the tool definitions (name, description, JSON schema)
//   POST /v1/tools/call    -> {"name": tool, "arguments": {...}}  ->  {"tool", "result"}
//
// Every request needs "Authorization: Bearer <token>". The port, token and
// URL are written to %APPDATA%/DjehutiMath/agent-api.json while the app runs
// and removed when it exits. The socket listens on 127.0.0.1 only.

#include <JuceHeader.h>

#include <functional>
#include <memory>

class LocalAgentApi : private juce::Thread
{
public:
    using Completion = std::function<void(bool ok, const juce::String& resultJson)>;
    // Message thread. Runs one tool and reports its JSON result.
    using ToolHandler = std::function<void(const juce::String& name, const juce::var& arguments, Completion)>;

    explicit LocalAgentApi(juce::File discoveryFileOverride = {});
    ~LocalAgentApi() override;

    bool start();
    void stop();
    bool isRunning() const { return isThreadRunning(); }
    juce::String baseUrl() const;
    juce::String bearerToken() const { return token; }

    ToolHandler onToolCall;
    // The tool catalog for GET /v1/tools.
    std::function<juce::var()> onToolCatalog;

    static juce::File getDiscoveryFile();

private:
    struct HttpRequest;

    void run() override;
    void handleConnection(juce::StreamingSocket& socket);
    static bool readRequest(juce::StreamingSocket& socket, HttpRequest& request);
    static bool writeJson(juce::StreamingSocket& socket, int statusCode, const juce::String& statusText, const juce::var& body);
    juce::File activeDiscoveryFile() const;

    juce::StreamingSocket listener;
    juce::String token;
    juce::File discoveryOverride;
};
