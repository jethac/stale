# Windows Stale with Material 3 Expressive

Current design after the 14 October audit against `upstream.jpg`. This supersedes
the original recycle-first and check-and-select proposals.

## Product and visual language

Windows-first C++ engine and native Slint desktop using the pinned jethac/slint
M3E library. Retain upstream macOS sources. Keep neutral light and charcoal dark
themes with blue accents, Roboto Flex, a custom draggable title bar, prominent
Stale branding, and native caption controls. Scan target, picker and Scan live
in the title bar.

Keep the M3E rail: Cleanup, Files, Duplicates, Activity. Its menu expands as an
overlay; the sun/moon switch remains at the bottom. Files includes the Explorer
all-drive tree and letter-sorted drive rows showing used/total, free space,
percentage and proportional bars. Used capacity is green below 50%, yellow below
75%, red thereafter; free space is a pale theme-aware track. Unknown capacity
is labelled, never represented as measured zero.

## Information hierarchy

One compact scope header carries the folder/drive identity and source date.
Show meaningful operation/error status without repeating instructions. The
scanned root presents category summaries linking to Cleanup; totals use
non-overlapping observations. File rows show blue bars only when measured.
Capacity and scanned allocation remain distinct. Never call modification time
last use, or infer never-opened files.

Cleanup selects Current scan or Saved report explicitly. Importing a report
preserves the current scan. Put provenance in the header and size column.
Categories group by kind and project; the central list is the primary workspace.
The inspector is dismissible and overlays instead of squeezing the list below
1150 logical pixels. Preserve inspected identity by path through query refresh.
Inspection and checkbox deletion selection are different concepts.

## Selection, deletion and recovery

Checkbox selection is immediate, including supported saved candidates. Select
all shown covers the entire filtered/category result, not just the viewport.
One visible confirmation includes every selected full path. Cancel receives
initial keyboard focus; Tab explicitly reaches Delete permanently. No typed
phrases, prevalidation ceremony, recycle fallback, or extra warning stages.

Permanent deletion checks paths and contents at execution. Drive/share/volume
roots and configured user/system roots remain protected. Do not follow reparse
points or hydrate cloud placeholders. No elevation, undo, or deletion cancel.
Progress describes actual work, not estimated bytes reclaimed. Keep unselected
candidates, remove successes, and retain failed items with reasons requiring
a rescan. Activity exposes structured path/reason outcomes. A failed tree may
be partly removed; do not claim it is untouched.

## Interaction and agent parity

Space toggles selection. Enter enters folders or inspects a row. F5 scans the
title-bar target; Ctrl+F filters; Escape closes transient UI; Delete opens review.
Focus outlines distinguish keyboard focus from inspected-row tint. Use readable
secondary text and comfortable controls at 1240×820 and 1000×660.

Browsing works without a scan and uses a separate worker during operations.
Immutable query inputs and generation checks prevent stale publication.
Navigation during scanning must not be overwritten when the scan completes.

The optional same-user named pipe shares the visible controller, source,
selection and confirmation. Stable path IDs and view revisions protect actions
against obsolete rows. Expose scope, overview, inspector/selection state,
progress and per-path results. Agents may open or dismiss review; no agent
deletion-confirmation endpoint exists.

## Boundaries

`windows/core.h` is the backend contract; `desktop/` owns presentation/controller.
CMake, scripts and GitHub Actions own builds/publication. USN replay, general
uninstall integration, Docker/WSL pruning, automatic updates, and macOS UI
migration remain deferred. Use actual backend data, not invented recency,
recovery guarantees or shipped demo data.
