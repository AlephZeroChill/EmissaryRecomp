// Benchmark-only probe, copied into isolated historical Dolphin checkouts.
// It changes no emulated state. All disk I/O occurs after the measurement window.
//
// EmissaryRecomp's tools/rollback_bench: bench.py copies this one file into both engines' core checkouts (the recomp's
// and the Dolphin worktree's), so both are timed by the same code. TimedSave/TimedLoad at the end time the core's
// engine calls (Engine::SaveFrame/LoadFrame: Dolphin's RollbackManager behind DolphinEngine, or EmissaryRecomp's
// snapshot store behind RecompEngine) with the same Span the historical runs put inside
// RollbackManager::SaveFrame/LoadFrame.
#pragma once

#include <utility>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace UnifiedRollbackBench
{
using Clock = std::chrono::steady_clock;
enum Kind { Save, Load, Shown, Resim, Loaded, Update };
struct Sample { Kind kind; long long frame; long long ns; };
struct State
{
  const char* output = std::getenv("PPR_UNIFIED_BENCH_OUTPUT");
  long long first = 600;
  long long last = 2400;
  long long frame = -1;
  long long update_frame = -1;
  bool open = false;
  bool resim = false;
  bool loaded = false;
  bool next_loaded = false;
  bool written = false;
  Clock::time_point pass_start{}, update_start{};
  std::vector<Sample> samples;
  State()
  {
    if (const char* v = std::getenv("PPR_UNIFIED_BENCH_FIRST")) first = std::atoll(v);
    if (const char* v = std::getenv("PPR_UNIFIED_BENCH_LAST")) last = std::atoll(v);
    if (output) samples.reserve(300000);
  }
};
inline State state;
inline long long Ns(Clock::time_point a, Clock::time_point b)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}
inline bool InWindow(long long frame)
{
  return state.output && !state.written && frame >= state.first && frame < state.last;
}
inline void Add(Kind kind, long long frame, long long ns)
{
  if (InWindow(frame)) state.samples.push_back({kind, frame, ns});
}
inline void Write()
{
  if (!state.output || state.written) return;
  state.written = true;
  const std::string partial = std::string(state.output) + ".partial";
  if (FILE* f = std::fopen(partial.c_str(), "w"))
  {
    static const char* names[] = {"save", "load", "shown_frame", "resim_frame", "loaded_frame", "update"};
    std::fprintf(f, "kind,game_frame,ns\n");
    for (const auto& s : state.samples)
      std::fprintf(f, "%s,%lld,%lld\n", names[s.kind], s.frame, s.ns);
    std::fclose(f);
    std::rename(partial.c_str(), state.output);
  }
}
inline void EndPass()
{
  if (!state.output || !state.open) return;
  const auto now = Clock::now();
  state.open = false;
  const auto ns = Ns(state.pass_start, now);
  Add(state.resim ? Resim : Shown, state.frame, ns);
  if (state.loaded) Add(Loaded, state.frame, ns);
}
inline void BeginPass(bool resim, long long game_frame)
{
  if (!state.output || state.written) return;
  const auto now = Clock::now();
  if (!resim)
  {
    if (state.update_frame >= 0)
      Add(Update, state.update_frame, Ns(state.update_start, now));
    if (game_frame >= state.last)
    {
      Write();
      return;
    }
    state.update_frame = game_frame;
    state.update_start = now;
  }
  state.frame = game_frame;
  state.resim = resim;
  state.loaded = state.next_loaded;
  state.next_loaded = false;
  state.open = true;
  state.pass_start = now;
}
class Span
{
public:
  explicit Span(Kind kind) : m_kind(kind), m_frame(state.frame), m_on(InWindow(m_frame))
  {
    if (m_on) m_start = Clock::now();
  }
  ~Span()
  {
    if (m_on) Add(m_kind, m_frame, Ns(m_start, Clock::now()));
    if (m_kind == Load && state.output) state.next_loaded = true;
  }
private:
  Kind m_kind;
  long long m_frame;
  bool m_on;
  Clock::time_point m_start{};
};
// The core's snapshot calls (core/src/Rollback/GameplaySession.cpp), timed as one span each.
template <class E, class T>
inline void TimedSave(E& engine, T tag)
{
  Span span(Save);
  engine.SaveFrame(tag);
}
template <class E>
inline bool TimedLoad(E& engine, int frames_back)
{
  Span span(Load);
  return engine.LoadFrame(frames_back);
}
}
