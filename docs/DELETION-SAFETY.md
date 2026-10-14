# Deletion safety audit

Source review: 2026-10-14. Scope: Windows desktop selection, imported reports,
agent controls, path handling, and permanent deletion in `windows/core.cpp`.
This is an engineering audit, not a security certification or a guarantee against
loss. It does not audit the upstream macOS implementation or Windows Shell itself.

## Read before deleting

Stale Material permanently deletes the selected paths and their contents. It does
not use the Recycle Bin or provide undo. Keep independent backups, review the full
paths in the confirmation, and avoid deleting trees being modified by another
program. A recommendation, old modification date, or a name such as `build` does
not prove that everything inside is disposable.

## Findings addressed in this revision

| Hazard | Finding and change |
| --- | --- |
| Entire drive, share, or volume deletion | The old drive-length check did not explicitly protect UNC share roots. The deletion boundary now rejects drive roots, UNC server/share roots, and detected mounted-volume roots before scanning or deleting. |
| Home or system tree deletion | Explicitly reject the current user profile, public profile, configured system/program roots, and their ancestors. Existing protected-path checks remain. This is not an exhaustive inventory of valuable user folders. |
| Ambiguous Windows paths | Reject relative/drive-relative paths, device namespace paths, dot segments, and trailing-dot/space components for deletion. Existing short-name aliases are expanded for root protection. Ordinary absolute paths remain supported. |
| Imported reports selecting broad roots | Reject unsafe candidate paths and duplicate normalized case-insensitive candidate paths during import. A report may still recommend an ordinary personal folder: it is untrusted advice, not evidence that deletion is appropriate. |
| Hidden selection | Confirmation now lists every selected full path, including items hidden by the current filter. Selection intentionally persists while filtering. |
| Checkbox refresh changing selection | The live confirmation check exposed checkbox state notifications toggling a row during list reconstruction. Checkbox events now change selection only when the requested state differs from the model. |
| Stale filter list | Remove the delay between filter editing and invalidating the old result rows, so a bulk-selection click cannot select the previous filter's list during that delay. |
| Permission errors treated as missing files | Shell callbacks tolerate only file/path-not-found results. Other metadata lookup failures stop that item rather than silently passing. Existing ancestors are checked for newly introduced reparse/cloud attributes without rescanning contents. |
| Misleading success | Check that the selected path no longer exists before reporting success. A failed or unverifiable result explicitly warns that contents may already have been removed. |

The confirmation remains a single step. These changes do not turn selection into
a separate validation workflow or change permanent deletion into recycling.

## Known failure modes and remaining risks

| Risk | Current behavior / limitation |
| --- | --- |
| Wrong selection or misleading report | An ordinary folder containing valuable data can still be deliberately selected and deleted. The app cannot infer intent. Bulk selection covers all rows in the current category/filter, not just the rows visible on screen. Review the confirmation's path list. |
| Concurrent path changes (TOCTOU) | Scanning and deletion are separate operations. Callback checks reduce exposure but do not pin every directory to a filesystem handle. A hostile or concurrent process may replace paths between checks. Do not treat Stale as safe against an adversarial same-user process; stop writers before cleanup. A handle-based deletion design would be needed for stronger guarantees. |
| Interrupted or partially failed deletion | No transaction or rollback. A process kill, crash, permission error, locked file, or device disconnection can leave a partly deleted tree. Activity is in memory and can be lost when the app exits. Rescan; do not infer that a failed folder is untouched. |
| No deletion cancellation | The current deletion batch has progress but no supported Cancel control. Force-killing it can lose results and leave partial work. Cancellable deletion with persistent per-item outcomes remains outstanding. |
| Reparse points, cloud and protected content | These can reject a selected tree. The app does not follow or intentionally delete through them. Cloud provider and remote filesystem behavior are not comprehensively qualified. |
| Hard links | Explicit deletion removes the selected directory entry. Other links remain; disk space may not be reclaimed until the last link is gone. Automatic cleanup recommendations are more conservative than explicit selection. |
| Live builds, caches and models | Generated folders may contain hand-edited files or active build state. Deleting them may break an application, require rebuilding or downloading, or race an active process. |
| Stale sizes | Imported summaries and saved scans can be outdated. Selected-byte totals are estimates, not a promise of freed space; hard links, compression and filesystem overhead also affect the result. |
| Alias and filesystem coverage | The root guard handles standard drive/UNC paths and detected mounted roots. Unusual namespace aliases, nonstandard filesystems, and concurrent mount changes are not comprehensively qualified. Failure to enumerate is not permission to proceed. |
| Agent access | Opt-in, local named pipe; remote clients are rejected. The API can select items and open the review but has no deletion-confirm command. This is a UX boundary, not isolation from other software running as the same Windows user. |

## Earlier failures relevant to this audit

- An invisible Slint popup blocked interaction while appearing not to respond.
  Confirmation is now rendered inside the app window.
- Hard-linked Rust artifacts were rejected with a misleading protected/cloud/link
  error. Explicit permanent deletion now allows hard links.
- Revalidating the entire tree inside Windows deletion callbacks caused repeated
  scans and failures after earlier entries had already disappeared. Callbacks now
  perform bounded path checks, allow missing entries from recursive deletion,
  and report progress. A disposable actual-deletion reproduction succeeded while
  preserving a hard link outside the selected tree.

## Verification scope

This revision is built locally. Root/path guards are inspected with a read-only
probe; the deletion reproduction uses only a newly created disposable tree.
The confirmation is checked in the running UI. No real drive, share, user profile,
or user-selected batch is deleted as part of the audit. Existing unit tests are
preserved; no new unit-test suite is added. Remaining risks above are not claimed
resolved by a successful build or a small reproduction.
