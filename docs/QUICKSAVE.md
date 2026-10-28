# Quick save and quick load: not shipped, and why

Asked for (2026-10-27): hold Select, then L1 to quick-save and R1 to
quick-load into numbered slots, with keyboard equivalents and toasts. It is
**not in the build**. Neither route below could be made to work and be tested
end to end in the time there was, and a quick-save that sometimes restores a
broken game is worse than none. This page records what was found, so the
next attempt starts from it. The Select+L1/R1 buttons, F5/F8 and the slot
keys are still free (`src/hotkeys.h` is where they would go).

## Route A: a snapshot of the whole emulated machine

What an emulator's savestate is. Here it would have to hold:

| Part | Where it lives | Size or count (title screen, measured) |
|---|---|---|
| EE RAM, scratchpad | `PS2Memory` | 32 MiB + 16 KiB |
| Guest threads | `EeScheduler::GuestThread`: registers, wait state, `invocations`, a `std::function` `resumeCompletion` | 11 threads, **10 of them suspended part way through a guest call** (F10 report) |
| Their host stacks | one `EeFiber` each (Windows fibers, 8 MiB reserved) | the C++ frames of every recompiled function on the call chain |
| Kernel objects | semaphores, event flags, alarms, the ready queues, the vsync counter | dozens |
| VU0/VU1 | data and micro memory, registers, the VIF1 thread's queue (`src/vif1_thread.cpp`) and the GIF stage | 4+16+4+16 KiB plus queues on two host threads |
| GS | registers and 4 MiB of local memory, but the live copy is in the SDL GPU backend's render targets and texture cache on the GPU, at the internal resolution | up to 8x8 the PS2's 4 MiB |
| IOP | the native IOP (`ps2xIOP`): its RAM, its own threads, SPU2 RAM (2 MiB) and voices, the sound driver in step with the audio device | |
| Discs, card, MPEG | open file handles and positions, the movie demux and IPU threads, the card's state machine | |
| This repo's hooks | static state in `src/*.cpp` (movie audio queue, step fixes' accumulators, quick scenes, camera) | ~20 modules |

The blocker is the second and third rows. A recompiled game runs guest code
as host C++ functions: a guest `jal` is a host call. A guest thread that waits
on a semaphore is suspended *inside* that host call chain, on its own fiber
stack, and the scheduler resumes it there (`inGuestCall`). The runtime can
re-enter a function at its resume points from the saved pc
(`switch (ctx->pc)` in generated code, after syscalls and calls), but a thread
already in a guest call must be resumed, not re-entered. So restoring EE RAM
and registers is not enough: every suspended thread's host stack would have to
be restored too, byte for byte at the same address, with every pointer on it
(host heap objects, `std::function`s, hook frames from `src/return_hook.cpp`)
still valid. Within one process that is conceivable; across a restart it is
impossible (ASLR, heap layout), so a "save" could never outlive the session.
And all the other rows still have to be captured consistently while three
host threads (VIF1, GIF, GS worker) and the audio callback run.

A safe point would need every guest thread out of its guest calls, which
never happens while the game runs (the main thread is always deep in its
loop). The only design that could work is a different runtime: one where a
blocking syscall unwinds the host stack and the thread resumes from its
registers alone (the scheduler's old `EeDispatcherTransfer` path did unwind,
at about 10 us a switch, and was replaced by fibers because DQ8's movie
thread switches some 31,000 times a frame: `include/runtime/ee_fiber.h`). That is a framework project, not a feature.

**Verdict:** not feasible here; not attempted.

## Route B: the game's own save, driven from a hook

Rogue Galaxy saves at its save points (transporters) through its own menu.
The pieces are in the function maps and in RGDecomp:

- `GameDataSerialize` (0x3E6330) calls `SaveDataSerialize` (0x3E6100): the
  game state into the save image; `GameDataDeserialize` (0x3E5FE0) the
  reverse, then sets the party's area (`PartySetAreaMaybe`), purges the
  inventory and restores the play-time stamp.
- `McSaveSerialize` (0x375C60), `McSaveStep` (0x374110),
  `McSaveCreateStep` (0x375090), `McLoadStep` (0x373890): the card side.
- The transporter's save screen in the `normal` overlay: `TransSaveOpenState`
  (0x51A3C0), `TransSaveSlotListConfirm` (0x519C60), `TransSaveWriteTickCb`
  (0x518C60, a five-state machine) and the rest of `TransSave*`.
- Loading is the title screen's: `TitleStateCardCheck`, `TitleLoadCardScan*`
  in the `title` overlay, then the loading screen and the map.

So a quick-save could call the serializer on the game thread at a quiet
moment (exploring, not in an event or a battle) and write the image as the
card's own files into extra slot folders under `disc\mc0`. That part is
plausible. What is not:

1. **What a save holds.** The game saves at save points and loads you back
   at one: the image holds the area, not a free position, and no state of an
   event in progress. A save made anywhere else restores to the area's save
   point at best, and mid-event at worst with flags the story did not expect.
   That is not a quick-save in the emulator sense.
2. **Quick load.** There is no in-game path from the field back to a loaded
   game short of the title screen: the load is the title overlay's, and
   `GameDataDeserialize` alone restores the party and flags but not the map,
   characters and scripts already running. Driving a soft reset into the
   title, then its load menu, from a hook, is a long state machine to write
   and to keep from breaking.
3. **Testing.** The card on this PC is empty (`disc\mc0` has no save), and
   the first save point is past the opening's scenes. Nothing could be
   checked end to end.

**Verdict:** possible in part (a "save the game's normal way, from
anywhere, into an extra slot" could be built on `GameDataSerialize`), but it
would load back at the save point, not where it was made, and quick load
needs the title's load path. Not shipped untested.

## What would make it work

- A soft "save anywhere" first: call the game's own save screen
  (`TransSaveOpenState`) from a hotkey while exploring, so the game itself
  writes a normal save into a normal slot. It needs the overlay `normal`
  loaded and a check that the mode is exploring (`0x4B51A0 == 6`, as
  `src/quick_scenes.cpp` reads it). It is the game's save, with the game's
  limits, but it is honest and testable with any save card.
- For quick load, a hook that walks the title's load menu (as the benchmark
  scripts do with pad input, `tools/bench/walk.sh`) after a soft reset.
- A true savestate needs Route A's runtime change first.
