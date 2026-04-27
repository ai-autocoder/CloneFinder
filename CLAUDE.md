# CloneFinder

A Win32 GUI tool that finds files similar to a target file within one or more folder trees. Similarity is measured by line-diff count using an LCS (Longest Common Subsequence) algorithm. Lines are normalized (whitespace stripped, lowercased) before comparison.

## How it works

1. The user picks a **target file** and one or more **container folders** via the GUI.
2. All files matching the configured extension are collected recursively (or non-recursively).
3. The target file is read and normalized once, then compared in parallel against every collected file using a thread pool sized to `hardware_concurrency()`.
4. Results are sorted by "Diff Lines" or "Lines Removed" and displayed in a ListView (top 500 matches).

## Project structure

| File | Purpose |
|---|---|
| `clone-finder.cpp` | All application code — core logic + Win32 GUI |
| `clone-finder.manifest` | Declares Common Controls v6 dependency (enables Windows 10/11 visual styles) |
| `clone-finder.rc` | Embeds the manifest as `RT_MANIFEST` resource |
| `clone-finder-res.o` | Compiled resource object (generated, not committed) |
| `clone-finder.exe` | Built binary (generated, not committed) |
| `clone-finder.ini` | Persisted user settings, created next to the .exe at runtime |
| `mingw64/` | Bundled MinGW-w64 toolchain (winlibs GCC 15.2.0) |

## Build

Requires the bundled MinGW-w64 toolchain in `mingw64/`. Run from the project root:

```bat
mingw64\bin\windres.exe clone-finder.rc -o clone-finder-res.o
mingw64\bin\g++.exe -O2 -std=c++17 -static -mwindows clone-finder.cpp clone-finder-res.o -o clone-finder.exe -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -pthread
```

The `windres` step only needs to be re-run if `clone-finder.manifest` or `clone-finder.rc` change.

## Config persistence

Settings are saved automatically to `clone-finder.ini` (next to the `.exe`) on exit and reloaded on startup. The file is plain INI text and can be edited manually:

```ini
[Settings]
TargetFile=C:/path/to/file.htm
Extension=.htm
Recursive=1
SortMode=0
Folders=C:/Projects/src/|C:/other/path/
```

Multiple folders are separated by `|`.
