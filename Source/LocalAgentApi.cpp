#include "LocalAgentApi.h"

namespace
{
constexpr int maxRequestBytes = 1024 * 1024;

juce::var errorBody(const juce::String& message)
{
    auto* body = new juce::DynamicObject();
    body->setProperty("error", message);
    return juce::var(body);
}

juce::String headerValue(const juce::StringArray& lines, const juce::String& name)
{
    for (int index = 1; index < lines.size(); ++index)
    {
        const auto line = lines[index];
        const auto colon = line.indexOfChar(':');
        if (colon > 0 && line.substring(0, colon).trim().equalsIgnoreCase(name))
            return line.substring(colon + 1).trim();
    }
    return {};
}
}

struct LocalAgentApi::HttpRequest
{
    juce::String method, path, authorization, body;
};

LocalAgentApi::LocalAgentApi(juce::File discoveryFileOverride)
    : juce::Thread("Djehuti Math Local Agent API"), discoveryOverride(std::move(discoveryFileOverride))
{
}

LocalAgentApi::~LocalAgentApi() { stop(); }

juce::File LocalAgentApi::getDiscoveryFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("DjehutiMath").getChildFile("agent-api.json");
}

juce::File LocalAgentApi::activeDiscoveryFile() const
{
    return discoveryOverride == juce::File() ? getDiscoveryFile() : discoveryOverride;
}

juce::String LocalAgentApi::baseUrl() const
{
    return listener.getBoundPort() > 0 ? "http://127.0.0.1:" + juce::String(listener.getBoundPort()) : juce::String();
}

bool LocalAgentApi::start()
{
    if (isThreadRunning())
        return true;
    if (!listener.createListener(0, "127.0.0.1"))
        return false;
    token = juce::Uuid().toString();
    auto* discovery = new juce::DynamicObject();
    discovery->setProperty("schema", "djehuti-math-agent-api");
    discovery->setProperty("version", 1);
    discovery->setProperty("baseUrl", baseUrl());
    discovery->setProperty("token", token);
    discovery->setProperty("transport", "http");
    const auto file = activeDiscoveryFile();
    if (!file.getParentDirectory().createDirectory().wasOk() || !file.replaceWithText(juce::JSON::toString(juce::var(discovery), true)))
    {
        listener.close();
        return false;
    }
    startThread();
    return true;
}

void LocalAgentApi::stop()
{
    const bool wasRunning = isThreadRunning();
    signalThreadShouldExit();
    listener.close();
    stopThread(3000);
    if (!wasRunning)
        return;
    const auto file = activeDiscoveryFile();
    if (juce::JSON::parse(file.loadFileAsString()).getProperty("token", {}).toString() == token)
        file.deleteFile();
}

void LocalAgentApi::run()
{
    while (!threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> socket(listener.waitForNextConnection());
        if (socket != nullptr && !threadShouldExit())
            handleConnection(*socket);
    }
}

void LocalAgentApi::handleConnection(juce::StreamingSocket& socket)
{
    HttpRequest request;
    if (!readRequest(socket, request))
    {
        writeJson(socket, 400, "Bad Request", errorBody("Invalid HTTP request."));
        return;
    }
    if (request.authorization != "Bearer " + token)
    {
        writeJson(socket, 401, "Unauthorized", errorBody("A valid bearer token is required."));
        return;
    }
    if (request.method == "GET" && request.path == "/v1/status")
    {
        auto* body = new juce::DynamicObject();
        body->setProperty("status", "ready");
        body->setProperty("service", "Djehuti Math Agent API");
        writeJson(socket, 200, "OK", juce::var(body));
        return;
    }
    if (request.method == "GET" && request.path == "/v1/tools")
    {
        struct Wait { juce::WaitableEvent done; juce::var catalog; };
        auto wait = std::make_shared<Wait>();
        auto source = onToolCatalog;
        if (!source)
        {
            writeJson(socket, 503, "Service Unavailable", errorBody("The tool catalog is unavailable."));
            return;
        }
        juce::MessageManager::callAsync([source, wait] { wait->catalog = source(); wait->done.signal(); });
        if (!wait->done.wait(10000))
        {
            writeJson(socket, 504, "Gateway Timeout", errorBody("Timed out reading the tool catalog."));
            return;
        }
        auto* body = new juce::DynamicObject();
        body->setProperty("tools", wait->catalog);
        writeJson(socket, 200, "OK", juce::var(body));
        return;
    }
    if (request.method == "POST" && request.path == "/v1/tools/call")
    {
        const auto parsed = juce::JSON::parse(request.body);
        const auto name = parsed.getProperty("name", {}).toString().trim();
        if (!parsed.isObject() || name.isEmpty())
        {
            writeJson(socket, 400, "Bad Request", errorBody("Body must be {\"name\": tool, \"arguments\": {...}}."));
            return;
        }
        auto handler = onToolCall;
        if (!handler)
        {
            writeJson(socket, 503, "Service Unavailable", errorBody("Direct tool calls are unavailable."));
            return;
        }
        auto arguments = parsed.getProperty("arguments", {});
        if (!arguments.isObject())
            arguments = juce::var(new juce::DynamicObject());
        struct Wait { juce::WaitableEvent done; bool ok = false; juce::String result; };
        auto wait = std::make_shared<Wait>();
        juce::MessageManager::callAsync([handler, name, arguments, wait] {
            handler(name, arguments, [wait](bool ok, const juce::String& result) {
                wait->ok = ok;
                wait->result = result;
                wait->done.signal();
            });
        });
        if (!wait->done.wait(125000))
        {
            writeJson(socket, 504, "Gateway Timeout", errorBody("Timed out waiting for the tool to finish."));
            return;
        }
        auto* body = new juce::DynamicObject();
        body->setProperty("tool", name);
        body->setProperty("result", juce::JSON::parse(wait->result));
        writeJson(socket, wait->ok ? 200 : 409, wait->ok ? "OK" : "Conflict", juce::var(body));
        return;
    }
    writeJson(socket, 404, "Not Found", errorBody("Unknown endpoint."));
}

bool LocalAgentApi::readRequest(juce::StreamingSocket& socket, HttpRequest& request)
{
    juce::MemoryBlock bytes;
    char buffer[4096] {};
    int headerEnd = -1, expectedSize = -1;
    for (int attempt = 0; attempt < 100 && static_cast<int>(bytes.getSize()) <= maxRequestBytes; ++attempt)
    {
        if (socket.waitUntilReady(true, 100) <= 0)
            continue;
        const auto count = socket.read(buffer, sizeof(buffer), false);
        if (count <= 0)
            break;
        bytes.append(buffer, static_cast<size_t>(count));
        const auto text = juce::String::fromUTF8(static_cast<const char*>(bytes.getData()), static_cast<int>(bytes.getSize()));
        if (headerEnd < 0)
        {
            headerEnd = text.indexOf("\r\n\r\n");
            if (headerEnd >= 0)
            {
                const auto headers = juce::StringArray::fromLines(text.substring(0, headerEnd));
                expectedSize = headerEnd + 4 + headerValue(headers, "Content-Length").getIntValue();
            }
        }
        if (headerEnd >= 0 && static_cast<int>(bytes.getSize()) >= expectedSize)
            break;
    }
    if (headerEnd < 0 || expectedSize < 0 || expectedSize > maxRequestBytes || static_cast<int>(bytes.getSize()) < expectedSize)
        return false;
    const auto text = juce::String::fromUTF8(static_cast<const char*>(bytes.getData()), expectedSize);
    const auto headers = juce::StringArray::fromLines(text.substring(0, headerEnd));
    if (headers.isEmpty())
        return false;
    const auto requestLine = juce::StringArray::fromTokens(headers[0], " ", "");
    if (requestLine.size() < 2)
        return false;
    request.method = requestLine[0].toUpperCase();
    request.path = requestLine[1].upToFirstOccurrenceOf("?", false, false);
    request.authorization = headerValue(headers, "Authorization");
    request.body = text.substring(headerEnd + 4);
    return true;
}

bool LocalAgentApi::writeJson(juce::StreamingSocket& socket, int statusCode, const juce::String& statusText, const juce::var& body)
{
    const auto json = juce::JSON::toString(body, false);
    const auto response = "HTTP/1.1 " + juce::String(statusCode) + " " + statusText + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: " + juce::String(json.getNumBytesAsUTF8()) + "\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n" + json;
    const auto utf8 = response.toUTF8();
    const auto total = static_cast<int>(utf8.sizeInBytes() - 1);
    int written = 0;
    while (written < total)
    {
        const auto count = socket.write(utf8.getAddress() + written, total - written);
        if (count <= 0)
            return false;
        written += count;
    }
    return true;
}
