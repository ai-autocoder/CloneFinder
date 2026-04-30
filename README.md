# CloneFinder

A Windows GUI tool that finds files similar to a target file across one or more folder trees. Similarity is measured by line-diff count (lines added + removed) using an LCS (Longest Common Subsequence) algorithm. Lines are normalized — whitespace stripped and lowercased — before comparison.

Results show the top 500 closest matches, sortable by any column.

---

## Usage

### 1. Pick a target file

Click **Browse...** next to the *Target file* field and select the file you want to compare against.

### 2. Add search folders

Click **Add...** to open the folder dialog, where you can either type a path directly or click **Browse...** to use the folder picker. Click **Edit...** (or double-click an entry) to modify an existing path. Click **Remove** to delete the selected entry.

Folders support a single `*` wildcard segment, e.g.:

```
C:\Projects\*\src
```

This expands to every direct subfolder of `C:\Projects\` that contains a `src` sub-path, so new version folders are picked up automatically. Wildcard patterns can be typed directly in the Add/Edit dialog.

### 3. Set extension and recursion

- **Extension** — only files with this extension are scanned (default: `.htm`).
- **Recursive** checkbox — when checked, subfolders are traversed; uncheck to scan only the top level of each folder.

### 4. Run the scan

Click **Start**. The progress bar tracks comparison progress. Click **Cancel** to abort at any time.

### 5. Work with results

| Action | Result |
|--------|--------|
| Click a **column header** | Sort by that column (ascending); click again to toggle descending. Active column shows a ▲/▼ arrow. |
| **Double-click** a row | Opens the file in its associated application. |
| **Right-click** a row | Copies the full file path to the clipboard. |

Sortable columns: **File** (alphabetical), **Similarity %**, **Diff Lines** (added + removed), **Added**, **Removed**.

---

## Settings persistence

All settings are saved automatically to `clone-finder.ini` (next to the `.exe`) on exit and restored on startup. The file can be edited manually:

```ini
[Settings]
TargetFile=C:/path/to/file.htm
Extension=.htm
Recursive=1
SortColumn=1
SortAscending=0
Folders=C:/Projects/*/src|C:/other/path/
```

| Key | Values |
|-----|--------|
| `Recursive` | `1` = recursive, `0` = top-level only |
| `SortColumn` | `0` = File, `1` = Similarity %, `2` = Diff Lines, `3` = Added, `4` = Removed |
| `SortAscending` | `1` = ascending, `0` = descending |
| `Folders` | Pipe-separated list of paths; `*` wildcard supported in one segment |

---

## How similarity is measured

The tool computes an LCS between the normalized lines of the target file and each candidate file:

- **Similarity %** = `max(0, 1 − Diff Lines / target line count) × 100` — 100% means identical, 0% means no overlap (or more diff lines than the target has lines). This is the default sort column, descending, so the best matches appear first.
- **Diff Lines** = lines added + lines removed (lower = more similar)
- **Added** = lines in the candidate not in the target
- **Removed** = lines in the target not in the candidate

Comparison runs in parallel across all CPU cores.

---

## Build (Windows, MinGW-w64)

Requires the bundled toolchain in `mingw64/` (winlibs GCC 15.2.0).

```bat
mingw64\bin\windres.exe clone-finder.rc -o clone-finder-res.o
mingw64\bin\g++.exe -O2 -std=c++17 -static -mwindows clone-finder.cpp clone-finder-res.o -o clone-finder.exe -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -pthread
```

The `windres` step only needs to be re-run if `clone-finder.manifest` or `clone-finder.rc` change.
