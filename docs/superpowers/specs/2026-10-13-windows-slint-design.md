# Windows Stale with Material 3 Expressive

Implement the Windows-first scope accepted in the conversation with two Astra implementers, backend and frontend. Retain the existing macOS sources and build. Use a C++17 backend, C++20 desktop, Win32 adapters and the jethac/slint Material UI via Slint's C++ API.

## Behavior

Scan a user-selected folder or drive without elevation. Show actual allocated and logical sizes, last-modified dates, scan errors and cancellation. Never describe modification time as last use or infer never-opened. Do not traverse directory reparse points or hydrate cloud placeholders. Handle Unicode and long paths. Save complete indexes and load them for immediate display while allowing explicit/background refresh. Report stale data honestly; USN replay is deferred.

Browse and sort folders/files, search/filter results, inspect cleanup explanations, and select items to recycle. Implement conservative generated-output/agent-cache suggestions and content duplicates. Protect application/system trees and cloud data from automatic recommendations. Recheck before recycling, reject permanent-delete fallback, report individual failures. Recycle Bin recovery must be honest: expose Windows recovery if reliable application Undo is unavailable. No actual user files are deleted during validation.

Use the fork's Material components for navigation, cards, filters, controls and feedback. Supply a dense scalable file list with keyboard operation, background scanning, progress and cancellation. Empty/error/loading states must work. No mocked data in the shipped interface.

## Boundaries

`windows/core.h` is the shared backend/frontend API. Backend owns `windows/` implementations and `tests/windows/`; frontend owns `desktop/` including Slint C++ bindings and its CMake target. Root integrator owns root CMake, Windows build scripts, CI and documentation. Both workers share this branch with disjoint file ownership. Header changes require communication.

Backend results are plain C++ value types. UTF-8 strings cross the interface; Win32 adapters convert to UTF-16. Backend jobs run off the UI thread. GUI-thread callbacks update Slint models; stale generations and shutdown cannot mutate destroyed UI.

## Deferred

NTFS journal replay, broad application leftovers/uninstall integration, Docker/WSL pruning, installer auto-update and full macOS UI migration. Build a real executable and packageable Windows artifact before extending these.

## Decision-focused desktop revision

The user rejected the first interface and asked for Claude's frontend-design
skill, then explicitly encouraged custom Material widgets. The installed Claude
skill matches the local frontend-design skill. Claude Code supplied a focused
design critique via the local claude-design workflow; implementation remains in
Slint/C++.

Use one drive header and three panes: recommendation kinds/projects, compact
items, and a persistent inspector. Put browsing and recycling activity in the
left pane instead of six competing primary destinations. Preserve old agent
page names as compatibility aliases.

The palette is cool mineral `#F1F4F3`, ink `#1C252B`, verdigris `#2F6F6A`,
graphite `#66727C`, ochre `#946400`, and low-space red `#B3261E`. Use the fork's
Material/Roboto typography with deliberate 22px headings and 14px body text.
Custom storage-ring.slint supplies vector capacity geometry without minimum
sweep or rounded caps that exaggerate tiny free-space fractions. The chart is
for disk usage, never an aggregate of overlapping cleanup suggestions.
The user's revised ring colors keep free space pale and color used space green
below 50% usage, yellow from 50% to below 75%, and red at 75% or higher.
An unknown capacity stays pale.

The user subsequently replaced the mint palette with two switchable modes:
neutral white/gray light mode and charcoal dark mode, both with blue accents.
`desktop/theme.slint` defines all Material roles so fields and dialogs follow
the same palette. The toggle at the bottom of the navigation rail persists per Windows user; AX exposes
the same control through `theme` and reports it in `state.theme`. Storage-ring
usage thresholds retain their green/yellow/red semantics in both modes.

Appearance uses a labeled Material switch. Omit the technology/agent footer.
Show the recycling action bar only when validated folders are checked, with
an explicit "selected for recycling" count; inspecting a row is independent
of this destructive-action selection. Recycle Bin access also lives under
"Recycled this session" when no folders are checked.

The latest user direction restores a Material 3 Expressive navigation rail,
using the fork's ModalWideNavigationRail: Cleanup, Files, Duplicates, Activity.
Categories/projects appear only within Cleanup; the hamburger expands the rail
as an overlay without squeezing the file view. Activity contains Recycle Bin
access. Old AX page names remain compatible with the rail destinations.

The user explicitly requested removal of the native title bar. Use a 64px
custom header with the existing Stale icon, a 26px wordmark, and accessible
minimize/maximize/restore/close controls. WindowMoveArea and resize-border-width
delegate dragging and resizing to Slint's Windows backend. Double-click toggles
maximization. Preserve both appearance modes and the saved theme preference.

Group candidates by kind and project, preserving backend validation tokens.
Ancestor/descendant overlaps do not count twice. Models, potential photo copies,
and scratch observations remain review decisions; group totals describe bytes
present, not guaranteed savings. Hide absent modification dates instead of
printing repeated Unknown labels. The inspector explains why an item appears,
the evidence limits, and the next available action.

Saved observations remain read-only. A targeted scan obtains current candidates;
because ordinary cleanup excludes the scan root, scan the candidate's parent
when needed. Never turn imported report text into validation authority. Retain
the saved map across focused scans. Expose group/project navigation, current
storage measurements, and targeted scanning through the same visible controller
used by the human. Agent commands never confirm recycling.

## Desktop density refinement

After the all-drive tree landed, the user rejected the visual finish. A focused
Claude frontend-design review identified oversized form fields, too much space
above the first file, repeated file paths, and an empty inspector competing with
the content. Preserve the approved palettes, rail, title-bar scanning, and ring
semantics. Use a compact single-line toolbar input with an explicit focus ring,
a 76px capacity band with a 56px ring, and adjacent workspace panes separated
by quiet dividers. Keep the 64px custom title bar, with a 36px logo and 22px
wordmark. Files use 36px single-line rows with folder icons and a named size
column; recommendation rows retain their explanatory second line. Hide the
inspector until an item is inspected. Retain operation status and saved-scan
provenance rather than hiding errors or presenting saved sizes as live measures.

The tree gets a separate density treatment: 32px single-line rows, 13px labels,
16px computer/drive/folder icons, 14px indentation, and vector chevrons. Use a
subtle selection tint and a slim accent marker instead of large rounded pills.
Capacity and status detail remains in accessible labels and hover tooltips;
the tree's primary job is showing the folder hierarchy.

This PC presents drive capacity as 68px rows with a proportionate horizontal
bar, free-space label, used/total bytes, and percent used. The free portion is
pale; used portions follow the ring thresholds (green below 50%, yellow below
75%, red thereafter). Unknown capacity shows a neutral track with an explicit
unavailable label. Row inspection, folder opening, and keyboard navigation use
the same list model as browsing; AX exposes the same capacity figures.
