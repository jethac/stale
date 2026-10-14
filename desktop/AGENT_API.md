# Controlling the visible Stale application

Start an instance with an explicit local endpoint:

```powershell
build/windows/bin/Stale.exe --agent-pipe desktop
./scripts/stale-agent.ps1 -Request '{"action":"capabilities"}'
./scripts/stale-agent.ps1 -Request '{"action":"scan","path":"B:\\"}'
./scripts/stale-agent.ps1 -Request '{"action":"state"}'
```

The agent and human share one window, controller, scan, selection, and review.
This interface does not launch a separate headless scan. Normal launches expose
no endpoint. `state.agent_enabled` reports whether agent control is enabled.

Protocol version 1 uses local Windows named pipe `stale-<name>`, restricted to
the launching Windows user. Remote pipe clients are rejected. Send a UTF-8 JSON
object followed by newline (maximum 64 KiB). Read one JSON response line, then
send `ACK\n` before closing. The included PowerShell client handles this framing.
One connection handles one request; response size is limited to 1 MiB.

Use `capabilities` for available actions. After an asynchronous action, poll
`state` until `busy=false` and `rows_ready=true`. Errors return `ok=false` and a
message. Inspect state after a timeout before retrying an action.

```powershell
./scripts/stale-agent.ps1 -Request '{"action":"page","name":"cleanup"}'
./scripts/stale-agent.ps1 -Request '{"action":"rows","offset":0,"limit":50}'
./scripts/stale-agent.ps1 -Request '{"action":"filter","text":"target"}'
./scripts/stale-agent.ps1 -Request '{"action":"sort","order":"largest"}'
```

Rows have stable path IDs, byte counts, reasons, and selection eligibility.
For cleanup rows, `selectable` means the checkbox is available now; `validated`
separately reports existing scan evidence. Saved candidates can be selectable
without being validated. Imported tokens never establish that evidence.
The Files view browses live folders independently of a scan. Use `drives` to list
every logical drive, `browse` with an absolute `path` (or `this-pc`), and `tree`
to read the visible flattened tree. `tree-open` takes a node `id`;
`tree-expand` takes `id` and `open`; `refresh-drives` refreshes the inventory.
Tree pagination uses `offset` and `limit` (1–100). Poll `tree_loading=false`
after expansion; folder navigation completes when `browse_busy=false` and
`rows_ready=true`. `browser_error` exposes inaccessible or missing folders.
Live rows expose `live` and `size_known`: an unmeasured directory is unknown,
not zero bytes. Folder sizes from an existing scan are labeled accordingly.
Network and removable drives remain navigable even when `ready=false`, which
can mean their capacity has not been probed. Browsing does not hydrate cloud
files or traverse reparse points.
This PC rows additionally expose `drive`, `capacity_known`, `used_fraction`,
`capacity_label`, and `free_label`, matching the visible per-drive capacity bars.
The `drives` response supplies the underlying byte counts. Unknown capacity is
explicitly labeled and is not represented as a zero-percent-full drive.

Enumerate a row before `inspect`, `open`, or `select`. Pass its `view_revision`
to reject requests from an obsolete view. Pagination allows 1–100 rows per call.
Scan and duplicate progress, cancellation, selection, errors, and visible review
state are exposed. Cleanup also exposes `cleanup_candidate_count` and `cleanup_ready_count`; its review bar remains visible before selection. `screenshot` saves the actual window as a PPM image.

The recommendation pane has matching structured controls:

```powershell
./scripts/stale-agent.ps1 -Request '{"action":"groups"}'
./scripts/stale-agent.ps1 -Request '{"action":"open-group","id":"builds"}'
```

`groups` returns byte totals, counts, and up to 100 projects per kind, with the
full project count. Use `open-project` with a returned project ID, or
`expand-group` with `id` and `open` to expand its visible rows. Group totals
exclude overlapping ancestor/descendant candidates; they describe observed
data, not guaranteed recoverable space. `state` includes `active_group` and
`active_project`.

`check-select` takes a row `id` and optional `view_revision` and checks and selects
that saved candidate in one operation. Enumerate rows first. `select-visible`
takes an optional `view_revision` and selects all supported candidates in the current
filtered cleanup view, including rows beyond the current pagination page. Both
preserve existing selections, skip unsupported candidates, and never delete.
Poll `state` until idle; `cancel` retains completed selections.

After enumerating and inspecting an unchecked item, `scan-detail` starts the
legacy targeted validation without selecting the result. Generated output is checked
by scanning its parent project. Exact matching fresh backend candidates can
then be selected in the saved recommendations; imported claims never supply
validation tokens. Models remain a separate review decision.

`state.drive_total_bytes`, `state.drive_free_bytes`, and `state.storage_slices`
describe the live disk-capacity ring, separate from saved scan sizes. Slices
carry IDs, labels, starts and fractions. `resize` accepts logical `width` and
`height` from 1000×660 through 3840×2160 for visual inspection.

`review` opens the visible confirmation dialog. There is no agent command to
confirm deletion. `dismiss-review` closes it; selection and view changes are
refused while review is open. The normal backend still revalidates items when
the human confirms. Agent support does not bypass cleanup validation.

## Saved recommendations

The Expressive rail has four destinations. Use `navigate` with `section` set
to `cleanup`, `files`, `duplicates`, or `activity`. Cleanup opens the saved map
when present, otherwise current scan results. `navigation-menu` with `open`
expands/collapses the rail; selecting a destination collapses it. State exposes
`navigation_index` (0–3) and `navigation_expanded`. Existing `page` aliases remain.

`window` accepts `operation: minimize`, `maximize`, or `restore`, matching the
custom caption controls. State exposes `window_maximized` and `window_minimized`.

`theme` with `mode` set to `light` or `dark` operates the same appearance
control as the navigation-rail toggle. `state.theme` reports the current mode. The
preference is saved per Windows user and restored on launch.

`load-report` with `path`, or startup `--report <path>`, opens a v1 JSON summary
in the Recommendations page. The report contains `version`, `root`, `scanned_at`
(Unix seconds), `files`, `errors`, `entries`, and `candidates`. Entries have
`path`, `name`, `parent` (earlier entry index; root is -1), `directory`,
`logical_bytes`, `allocated_bytes`, and optional `modified`. Candidates have
`path`, `reason`, `bytes`, and optional `modified`.

Reports are limited to 16 MiB and 100,000 entries/candidates. Paths must remain
under the declared absolute root. Imported recommendations support immediate checkbox selection but do not
establish duplicate evidence. Validation runs when deletion is confirmed.
Loading a report preserves the current scan; Current scan and Saved report
select independent sources.

Run `scripts/test-agent.ps1` to exercise the real app with disposable fixtures.
CTest also covers pipe framing, request bounds, endpoint collisions and shutdown.

The vendored JSON parser is nlohmann/json 3.11.3, MIT licensed. Header SHA-256:
`9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`.

## Permanent deletion UI

Cleanup checkboxes and `select` select supported saved candidates immediately.
`select-visible` selects the current filtered view without scanning. Validation
runs when the human confirms **Delete permanently**. `review` opens this dialog;
there is no agent command to confirm it. Deletion bypasses the Recycle Bin.
The legacy `check-select` action remains available for explicit prevalidation.

## Overview, inspection and recovery

`state.scan_overview` identifies the scanned-root summary, `scope` names the
displayed location, and `inspector_visible` / `detail_selected` distinguish
inspection from deletion selection. `close-inspector` dismisses details without
changing checkboxes. `overview-group` accepts a category `id` from the current
scan's `groups` and opens that category in Cleanup when the overview is shown.

Browse, navigate, filter, sort and inspect remain available during scans.
Wait for `rows_ready` / `browse_busy` independently of the operation's `busy`.
Changing deletion selection and starting another operation remain disabled.
Scan completion preserves navigation made during the scan.

After deletion, Cleanup retains unselected candidates and failures. A failure's
`deletion_error` explains why it needs a rescan before retrying. Activity rows
have stable path IDs, separate `path`, `success`, and `message` fields. Failures
may describe partially removed folders; successful rows are removed from Cleanup.
Activity is session-only. No agent endpoint confirms deletion.
