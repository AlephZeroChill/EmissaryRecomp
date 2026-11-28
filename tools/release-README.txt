EmissaryRecomp {VERSION}
========================

Super Smash Bros. Brawl, statically recompiled into a native PC program, with Brawl Online's online core
({CORE}) built in: https://github.com/kalomaze/EmissaryRecomp

Brawl Online's launcher downloads, updates and starts this release (Settings > Add-ons). It holds the game program
only: it plays the files of your own Brawl disc (USA, RSBE01), which the launcher extracts. No game data is included.

WiiCompiled is the program. On Windows it starts engine_usa.dll, or on a processor without AVX2 engine_usa_v2.dll,
which comes in a download of its own (EmissaryRecomp-{VERSION}-Windows-OldCPU.zip: unzip it into this folder too;
the launcher does that itself). `WiiCompiled.exe --engine` prints which of the two this computer plays with.
Sys holds Brawl Online's rollback region sets. release.json says which release this is and what it was built from.

The engine is GPLv3 (LICENSE.txt); credits are in CREDITS.txt and the bundled libraries' licenses in licenses.
