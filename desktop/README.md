# Native Windows desktop

The desktop consumes the `stale::win` value-type API. Slint's compiler, runtime,
and Material library come from the same pinned jethac/slint revision.

The title bar contains the scan target. Files browses live folders without
starting a recursive scan. A completed scan adds category summaries and measured
size bars at its root. Capacity colors describe the volume; blue file bars
compare measured allocated sizes with the containing folder's measured total.
Unknown sizes are never drawn as measured zeroes.

Cleanup has explicit **Current scan** and **Saved report** sources. Importing a
report preserves the current scan. Inspection is independent of deletion
selection; the inspector can be closed and overlays the list below 1150 logical
pixels. Space toggles a candidate, Enter opens a folder or inspects a candidate,
Ctrl+F focuses filtering, F5 scans the title-bar target, and Delete opens review.

Tick checkboxes, choose **Delete selected**, and confirm once. Deletion is
permanent. The in-window confirmation lists every selected path, including
filtered-out selections. Cancel receives initial focus; Tab cycles between the
two actions and Escape cancels. There is no agent confirmation command.

Successful deletion removes completed candidates from Cleanup. Unselected
candidates remain; failed candidates show their reason and require a rescan
before retrying. Activity carries per-path successes and failures. Counts and
saved sizes are observations, not guaranteed reclaimed space. A failed item
may be partly deleted. There is no deletion cancellation or application Undo.

Scanning/deletion, live directory reads, and view queries have separate owned
workers. Query inputs are immutable snapshots. Generation checks reject stale
results; workers join during shutdown. Browsing and filtering remain usable
while a scan runs. Starting another scan or changing deletion selection stays
disabled during operations.

See [AGENT_API.md](AGENT_API.md) for the local interface. `screenshot` captures a
PPM snapshot; `resize` supports 1000×660 through 3840×2160 logical pixels.
Normal startup does not scan automatically.

For non-destructive executable smoke runs, use `--smoke-test --root <fixture>`;
optional `--page 0..4`, `--screenshot <file.ppm>`, `--load-cache`,
`--cancel-after-ms`, and `--close-after-ms` exercise existing paths.
Smoke mode never selects or deletes files. Exit codes: 0 success, 1 scan/load/
startup failure, 2 screenshot failure, 3 timeout.
