The desktop target consumes the `stale::win` value-type API. The Slint compiler,
C++ runtime, and `@material` library must come from the same pinned fork revision
configured by the root CMake project.

The UI uses the fork's Material controls and virtualized `ListView`. Its C++ model
stores source indexes and formats visible rows lazily. Scanning, cache I/O,
duplicate comparison, filtering, and sorting run on an owned worker. GUI updates
use weak handles; a generation counter rejects obsolete query results. Shutdown
cancels current work and joins the worker before destroying its state.

For a non-destructive executable smoke test:

```powershell
Stale.exe --smoke-test --root C:\path\to\disposable-fixture --screenshot overview.ppm
Stale.exe --smoke-test --root C:\path\to\disposable-fixture --page 2 --screenshot cleanup.ppm
Stale.exe --smoke-test --root C:\path\to\disposable-fixture --page 3 --screenshot duplicates.ppm
Stale.exe --smoke-test --root C:\path\to\disposable-fixture --load-cache
Stale.exe --smoke-test --root C:\path\to\large-fixture --cancel-after-ms 20
Stale.exe --smoke-test --root C:\path\to\large-fixture --close-after-ms 20
```

With no `--root`, smoke mode renders the empty state and never scans the default
home folder. `--page` accepts 0 (overview), 1 (browse), 2 (cleanup), 3 (duplicates),
or 4 (recycling activity). Page 3 performs real duplicate detection. Screenshots
are binary PPM, captured from Slint's window snapshot API. Exit codes are 0 for a
successful smoke run, 1 for a scan/load/startup failure, 2 for screenshot failure,
and 3 for the two-minute smoke timeout. `--close-after-ms` deliberately exits early
to exercise cancellation and worker teardown. No smoke option selects or recycles
files. Normal use opens with no automatic scan.

The file list supports arrows, Home/End, Enter to enter a folder, and Space to
toggle a cleanup candidate. Tab moves through Material controls. Recycling has a
Material confirmation dialog and shows individual outcomes in Activity. Recovery
opens Windows Recycle Bin; Stale does not claim to provide application-level Undo.
