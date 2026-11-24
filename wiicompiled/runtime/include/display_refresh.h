#pragma once

#include <cstdint>

struct SDL_Window;

// Whether the screen the game's window is on follows the frames (G-Sync, FreeSync, HDMI VRR) or refreshes at a
// fixed rate, as it behaves now (Windows). A fixed screen spends a few percent of each refresh in its vertical
// blank; one that follows 60 frames a second at 144 or 240 Hz finishes scanning a frame early and waits in the
// blank for the next, most of the time. The graphics kernel's scan line (D3DKMTGetScanLine) shows which, sampled a
// few hundred times a second on a thread of its own.
namespace DisplayRefresh {

// Starts watching the screen `window` is on. Harmless off Windows (VariableRefresh() stays -1).
void Start(SDL_Window* window) noexcept;
void Stop() noexcept;

// 1: the screen follows the frames; 0: fixed refresh; -1: not known (yet, a 60-75 Hz screen, frames as fast as the
// refresh, or not Windows). A verdict holds until two one-second windows in a row say otherwise.
int VariableRefresh() noexcept;

// The screen's refresh rate (Hz, as Windows reports it), 0 when unknown.
double RefreshHz() noexcept;

// The judgement on one window of samples, exposed for tests: the share of samples in the vertical blank, frames
// presented a second and the screen's refresh. Same meaning as VariableRefresh().
int Judge(double vblankShare, double framesPerSecond, double refreshHz) noexcept;

}  // namespace DisplayRefresh
