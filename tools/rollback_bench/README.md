# Rollback benchmark

`bench.py` measures rollback on EmissaryRecomp and on Brawl Online's Dolphin by replaying Brawl Online replays
(`.brep`). Playback rebuilds the recorded match. From then on, every frame rolls back a fixed number of frames and runs
them again with the recorded inputs. Both engines time this with the same probe (`UnifiedBenchmark.h`), and every run
must match the replay's own checksums. See the docstring in `bench.py` for details.

Linux only. It needs a brawlonline.net checkout at `../brawlonline.net` (with `origin/main` fetched
and its harness's `run/template-user`), and a Brawl Online install for its online card and netplay save.

```sh
python3 tools/fetch_brawl_online.py build/rbbench-core --repo . --commit <brawl_online.lock commit> --app-version <its appVersion>
python3 tools/rollback_bench/bench.py instrument build/rbbench-core
cmake -DBUILD_DIR=$PWD/build/rbbench -DBRAWL_ONLINE=$PWD/build/rbbench-core -P build.cmake
python3 tools/rollback_bench/bench.py dolphin-build
python3 tools/rollback_bench/bench.py prepare
python3 tools/rollback_bench/bench.py campaign --replay <.brep files or folders> --distances 0,2,4,7 --affinity <cpus>
python3 tools/rollback_bench/bench.py report
```

Distance 0 plays each frame once. Results go to `build/rollback-bench/` and name each replay by a hash of its
contents, not by its file name.
