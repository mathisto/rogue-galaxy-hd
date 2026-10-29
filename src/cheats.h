#pragma once

#include <string>

class PS2Runtime;

// Trainer-style cheats, the F1 menu's Cheats page (docs/CHEATS.md).
//
// Everything is a native hook or a per-frame write into guest memory, as the
// other options are; nothing replays a PCSX2 code. The addresses were found
// in the disassembly and checked against the running game (docs/CHEATS.md
// lists each one and whether it held):
//
// - GameData (0x4B8B30): +0x14 the playable characters' manager, whose pool
//   (manager +0x8) holds the ten characters (+0xC id 0 to 9, +0x10 level,
//   +0x14 HP, +0x18 AP, +0x20 maximum HP, +0x26 maximum AP, +0x1C4 action
//   gauge, +0x4B8 experience); +0x18 the party, money a 64-bit word at
//   +0x160; +0x1C the other units' manager, whose pool (manager +0x10) holds
//   the enemies (ids from 128, the same fields).
// - CharaSetHp (0x3B8040) is where damage lands, for characters and enemies.
// - ItemConsume (0x3D41B0), called from ItemUseMaybe (0x2EEF60) for an item
//   used in the field or in battle.
// - GameFrameUpdate (0x27C4C0) is the per-frame heartbeat.
// - Saving: McSaveStep (0x374110), the save screen's state machine, ends at
//   once with result 8 ("Save to file N failed.") while the master switch is
//   on; sceMcOpen with write flags, sceMcWrite, sceMcMkdir, sceMcDelete,
//   sceMcRename, sceMcSetFileInfo, sceMcFormat and sceMcUnformat fail as well,
//   so a game is never saved with cheated numbers in it.
//
// Off at every start: the master switch is not kept in settings.ini (the
// individual cheats are, and only act while the master is on).
namespace rg::cheats
{

// After loadELF().
void install(PS2Runtime &runtime);

// The master switch (any thread).
void setMaster(bool on);
bool master();
// The master is on and at least one cheat is switched on: the indicator.
bool active();
// Saving to the memory card is being refused (the master is on).
bool blockingSaves();
// Number of times a save command was refused since the start (for toasts).
unsigned refusedSaves();

// A cheat's switch, by its menu key without the "cheat_" prefix
// ("hp", "ap", "gauge", "items", "onehit", "money_hold").
void setSwitch(const std::string &name, bool on);
// A number: "money" (the amount held).
void setNumber(const std::string &name, int value);
// A one-shot action, run on the game's next frame: "max_money".
void fire(const std::string &name);

} // namespace rg::cheats
