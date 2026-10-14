# Stale for Windows

The Windows application uses a C++ filesystem engine and Material 3 Expressive
components from `jethac/slint`. The existing Makefile still builds the macOS app.

The custom title bar contains the scan path, folder picker, and Scan action.
Files includes an Explorer-style This PC tree showing all logical drives.
Expand folders to browse their immediate contents without starting a recursive
scan; use Scan in the title bar to measure them. Unmeasured folder sizes appear as
an em dash. Existing scan totals are identified in the folder details. Refresh
updates the drive inventory, including removable and mapped network drives.
The tree supports arrow keys, Home/End, Enter, and F6 to reach the file list.

## Cleanup

Tick the checkboxes beside folders, or use **Select all shown** for the current
category or filtered list. Click **Delete selected**, then **Delete permanently**.
Deletion bypasses the Recycle Bin, including for items too large to recycle.
Saved candidates are validated during deletion; selection is immediate.
Failures are reported per item in Activity. Recommendations, including models and manual-review folders, can be selected.
Unselected candidates remain after a batch. Failed candidates keep their reason
and require rescanning before retrying; a failure may mean partial deletion.
Current scan and Saved report are separate sources, and importing a report
preserves the current scan. Details overlay the list in narrower windows.

## Build

Install Visual Studio's **Desktop development with C++** workload, including the
CMake tools, and Rust 1.95 or newer with the `x86_64-pc-windows-msvc` toolchain.
From PowerShell in the repository:

```powershell
./scripts/build-windows.ps1
```

The script discovers Visual Studio, configures Ninja, builds, and runs CTest.
The first desktop build downloads and compiles the pinned Slint fork; allow time
and disk space for Rust dependencies. Fonts and UI images are embedded.

Run `build/windows/bin/Stale.exe`. To create a portable development ZIP:

```powershell
./scripts/package-windows.ps1
```

The ZIP is written to `dist/Stale-Windows-x64.zip`. The destination PC needs the
Microsoft Visual C++ x64 runtime. This build targets Windows 10/11 x64.

For engine development without Slint:

```powershell
./scripts/build-windows.ps1 -BackendOnly
```

To use an existing checkout of the pinned fork:

```powershell
./scripts/build-windows.ps1 -SlintSource B:/dev/forks/slint
```

Use revision `46f3b237fc6a746356e9cd5535601b6d799637c0` for reproducibility.
This includes the fork's fix for variable-font embedding after its `SourcePath` migration.
An optional `-SlintPackage <prefix>/lib/cmake/Slint` uses a prebuilt installation
of that same revision; runtime, compiler and Material sources must agree.

## Agent control

Launch with `Stale.exe --agent-pipe desktop` to let a local agent control that
visible window. The included `scripts/stale-agent.ps1` client exposes structured
state, pagination, navigation, scan/cancel, selection and screenshots. Agent
review opens the normal dialog; it cannot confirm deletion. See
[the agent protocol](../desktop/AGENT_API.md) for commands and report import.
Saved recommendations support immediate selection; imported validation tokens
are ignored. The confirmation and execution-time path checks still apply.

## Windows behavior

- Dates mean **last modified**, not last opened or proof of disuse.
- Scans run without elevation and report inaccessible entries.
- Directory reparse points are not traversed. Cloud placeholders are excluded
  from content reads and automatic cleanup.
- Recommendations are conservative; inspect their explanations before deletion.
- Cleanup validates candidates and reports failures per item. Deletion is permanent.
- Size totals cover file data streams, not alternate data streams or directory
  filesystem overhead. Duplicate checks omit unreadable or changing files.
- Saved indexes are snapshots. Use Rescan to reconcile changes; persistent USN
  journal replay is not implemented.

Broad app-leftover detection, application uninstallation, Docker/WSL pruning,
and automatic updates are outside this initial Windows implementation.
Cloud exclusion uses Windows file metadata; a live cloud provider has not been
tested. Filesystem scans and cleanup revalidation are not transactional.

## Validation

Backend tests create disposable fixtures and exercise scanning, persistence and
cleanup policy. They must not recycle existing user data. Desktop validation
should include keyboard navigation, high DPI, cancellation and closing during
background work. The Windows CI job builds and tests before uploading binaries.

The Windows artifact is a development build, not a signed installer. Keep its
runtime DLLs with the executable. Third-party runtime and font licenses remain
applicable; see the pinned Slint source's `LICENSE.md` and `LICENSES` directory.
