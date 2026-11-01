#pragma once

#include <string>

// The Setup and HD page of the F1 menu asks the install folder what it holds, and can open the setup window
// (tools/setup, docs/INSTALLER.md). The game runs from the install folder, so the paths are relative to it.
namespace rg::setupinfo
{

// The version rg-install.json records (the setup writes it), or "" when the folder was made another way.
std::string recordedRelease();

// True when RogueGalaxy-Setup.exe is in the game's folder.
bool setupProgramPresent();

// Opens the setup window (RogueGalaxy-Setup.exe) as a program of its own, which outlives the game. False when it
// could not be started.
bool openSetup();

} // namespace rg::setupinfo
