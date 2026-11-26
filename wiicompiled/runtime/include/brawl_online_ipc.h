#pragma once
namespace FrameInput { struct GcPad; }
namespace BrawlOnline {
void Loop(bool stageSeen);
void Stage();
void Poll();
void WaitForGo();
void SamplePads(FrameInput::GcPad* pads);
// The same without counting a harness poll (a look between the game's reads).
void PeekPads(FrameInput::GcPad* pads);
}
