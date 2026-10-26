#include "VstExporter.h"

namespace vst_exporter
{
namespace
{
constexpr const char* kTemplateName = "Djehuti Math Effect";
}

juce::File templateBundle()
{
    const auto bundle = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                            .getParentDirectory().getChildFile("templates").getChildFile("vst3")
                            .getChildFile(juce::String(kTemplateName) + ".vst3");
    return bundle.getChildFile("Contents").getChildFile("x86_64-win").getChildFile(juce::String(kTemplateName) + ".vst3").existsAsFile()
               ? bundle : juce::File();
}

juce::String safeStem(const std::string& productName)
{
    juce::String out;
    for (const auto c : juce::String(productName))
        out += (juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ' || c == '-' || c == '_' || c == '.') ? c : (juce::juce_wchar) '_';
    out = out.trim();
    while (out.endsWithChar('.')) out = out.dropLastCharacters(1);
    return out.isEmpty() ? juce::String("Djehuti Math Effect Export") : out.substring(0, 64);
}

juce::String uiSchemaJson(const dsp_effect::Artifact& artifact)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("title", juce::String(artifact.spec.name));
    juce::Array<juce::var> controls;
    for (const auto& p : artifact.spec.params)
    {
        auto* c = new juce::DynamicObject();
        c->setProperty("id", juce::String(p.id));
        c->setProperty("label", juce::String(p.name));
        c->setProperty("unit", juce::String(p.unit));
        c->setProperty("min", p.minValue);
        c->setProperty("max", p.maxValue);
        c->setProperty("default", p.defaultValue);
        controls.add(juce::var(c));
    }
    root->setProperty("controls", controls);
    root->setProperty("wrapper", juce::StringArray { "bypass", "input_gain_db", "output_level_db", "mix" }.joinIntoString(","));
    return juce::JSON::toString(juce::var(root), false);
}

juce::String manifestJson(const dsp_effect::Artifact& artifact)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("generator", "Djehuti Math");
    root->setProperty("productId", juce::String(artifact.spec.id));
    root->setProperty("productName", juce::String(artifact.spec.name));
    root->setProperty("version", juce::String(artifact.spec.version));
    root->setProperty("manufacturer", juce::String(artifact.spec.manufacturer));
    root->setProperty("pluginCode4CC", juce::String(artifact.pluginCode4CC));
    root->setProperty("vst3Uid", juce::String(artifact.vst3UidHex));
    root->setProperty("integrityHash", juce::String(artifact.integrityHash));
    root->setProperty("objectHash", juce::String(artifact.objectHash));
    root->setProperty("target", dsp_effect::kTargetArch);
    root->setProperty("workspaceSize", artifact.spec.workspaceSize);
    root->setProperty("parameterCount", (int) artifact.spec.params.size());
    return juce::JSON::toString(juce::var(root), true);
}

ExportResult runExport(const ExportRequest& request)
{
    ExportResult result;
    const auto& artifact = request.artifact;
    if (artifact.object.empty() || artifact.spec.name.empty())
    {
        result.error = "Nothing to export: compile the effect first.";
        return result;
    }
    dsp_effect::Artifact verified;
    if (!dsp_effect::parseArtifact(dsp_effect::serialize(artifact), verified, result.error))
        return result; // the artifact must survive the same validation the plugin applies

    const auto templateDir = request.templateBundle != juce::File() ? request.templateBundle : templateBundle();
    const auto stemOfTemplate = templateDir.getFileNameWithoutExtension();
    const auto templateBinary = templateDir.getChildFile("Contents").getChildFile("x86_64-win").getChildFile(stemOfTemplate + ".vst3");
    if (templateDir == juce::File() || !templateBinary.existsAsFile())
    {
        result.error = "The VST3 runtime template was not found beside the application (templates/vst3/"
                     + std::string(kTemplateName) + ".vst3). Build the DjehutiMathEffectVst target.";
        return result;
    }
    if (request.outputDirectory == juce::File() || (!request.outputDirectory.isDirectory() && !request.outputDirectory.createDirectory().wasOk()))
    {
        result.error = "The output folder cannot be created: " + request.outputDirectory.getFullPathName().toStdString();
        return result;
    }

    const auto stem = safeStem(artifact.spec.name);
    result.packageDirectory = request.outputDirectory.getChildFile(stem);
    result.bundleDirectory = result.packageDirectory.getChildFile(stem + ".vst3");
    const auto win64 = result.bundleDirectory.getChildFile("Contents").getChildFile("x86_64-win");
    const auto resources = result.bundleDirectory.getChildFile("Contents").getChildFile("Resources");
    if (!win64.createDirectory().wasOk() || !resources.createDirectory().wasOk())
    {
        result.error = "Could not create the bundle folders under " + result.packageDirectory.getFullPathName().toStdString();
        return result;
    }
    result.binaryFile = win64.getChildFile(stem + ".vst3");
    if (!templateBinary.copyFileTo(result.binaryFile))
    {
        result.error = "Could not copy the VST3 runtime to " + result.binaryFile.getFullPathName().toStdString()
                     + " (is a host using the previous export?)";
        return result;
    }
    result.artifactFile = resources.getChildFile("effect.frust-il");
    if (!result.artifactFile.replaceWithText(dsp_effect::serialize(artifact))
        || !resources.getChildFile("manifest.json").replaceWithText(manifestJson(artifact))
        || !resources.getChildFile("ui.json").replaceWithText(uiSchemaJson(artifact)))
    {
        result.error = "Could not write the effect into " + resources.getFullPathName().toStdString();
        return result;
    }
    // Development references next to the bundle (never read by the plugin).
    result.packageDirectory.getChildFile("effect.frust-il").replaceWithText(dsp_effect::serialize(artifact));
    if (!artifact.spec.source.empty())
        result.packageDirectory.getChildFile("effect.frust").replaceWithText(juce::String(artifact.spec.source));
    for (const auto& f : { result.binaryFile, result.artifactFile, resources.getChildFile("manifest.json"), resources.getChildFile("ui.json") })
        result.files.push_back(f.getRelativePathFrom(result.packageDirectory).toStdString());
    result.identityLimitation =
        "Hosts identify every Djehuti Math export by the runtime's compiled VST3 class ('" + std::string(kTemplateName)
        + "'). The product name, parameters, state and audio come from this bundle, but two different exports cannot be told apart by class.";
    result.ok = true;
    return result;
}
}
