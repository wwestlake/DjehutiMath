#pragma once

// The mathematical analysis commands of the command service. User actions and agent tools reach them through
// the same dispatcher (DjehutiMath::dispatch); this module holds their behaviour so it can also run without
// the GUI. All of them read the worksheet through immutable snapshots and never modify the document.
//
//   worksheet_math_analyze       diagnostics, types, dimensions and MathIR of one math cell
//   worksheet_math_dependencies  names defined and used, dependencies, order, cycles, conflicts, result status
//   worksheet_math_evaluate      evaluates a cell (and the definitions it needs) with the registered engines
//   math_solver_capabilities     what the registered engines can do

#include "WorksheetPanel.h"
#include "MathWorkspace.h"

namespace math_commands
{
bool handles(const std::string& name);
// Brings the workspace in line with the document's math cells (adds, updates, removes). Cheap when nothing changed.
void sync(const WorksheetDocument& document, math_workspace::Workspace& workspace);
// Runs a command; `args` is the JSON object of its arguments. Returns the JSON result text.
juce::String execute(const std::string& name, const juce::var& args, const WorksheetDocument& document, math_workspace::Workspace& workspace);
}
