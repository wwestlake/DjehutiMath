#pragma once

// Packages a compiled Djehuti Math effect as a Windows x64 VST3 bundle.
//
// The bundle is the precompiled plugin runtime (templates/vst3 beside the
// application, built from Source/GeneratedVst) with the effect supplied in
// Contents/Resources: effect.frust-il (the executable artifact), manifest.json
// and ui.json. The plugin links the artifact's native object at load time; no
// compiler runs inside the host.
//
// Limitation shared with the DEWB template export: every export carries the
// template's compiled VST3 class identity, so a host cannot tell two exports
// apart by class. Names, parameters, state and audio all come from the bundle.

#include "DspEffect.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace vst_exporter
{
struct ExportRequest
{
    dsp_effect::Artifact artifact;
    juce::File outputDirectory;     // the package folder is created inside it
    juce::File templateBundle;      // optional override of <exe>/templates/vst3/<template>.vst3
};

struct ExportResult
{
    bool ok = false;
    std::string error;
    juce::File packageDirectory;    // <out>/<stem>/
    juce::File bundleDirectory;     // <stem>.vst3
    juce::File binaryFile;          // <stem>.vst3/Contents/x86_64-win/<stem>.vst3
    juce::File artifactFile;        // Contents/Resources/effect.frust-il
    std::string identityLimitation;
    std::vector<std::string> files; // what was written, relative to the package folder
};

// The packaged runtime this build ships, or an empty File when it is missing.
juce::File templateBundle();
juce::String uiSchemaJson(const dsp_effect::Artifact& artifact);
juce::String manifestJson(const dsp_effect::Artifact& artifact);
juce::String safeStem(const std::string& productName);
ExportResult runExport(const ExportRequest& request);
}
