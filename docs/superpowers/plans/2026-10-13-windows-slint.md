# Windows Slint Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. User explicitly selected two Astra agents, backend and frontend, with root integration/review.

**Goal:** A buildable Windows disk explorer and conservative cleanup application using jethac/slint Material 3 Expressive.

**Architecture:** Platform-specific Windows engine behind a plain C++ API, with a Slint C++ desktop shell. Existing macOS build remains intact.

**Tech Stack:** C++17 backend, C++20 desktop, Win32, CMake, Slint fork and Material library.

**Spec:** `docs/superpowers/specs/2026-10-13-windows-slint-design.md`

## Global Constraints

- No elevation or destructive validation against real user files.
- UTF-8 interface, UTF-16 filesystem operations, last-modified wording.
- Do not traverse reparse directories or hydrate cloud placeholders.
- Pin compatible Slint runtime/compiler/Material sources.
- Exactly two Astra implementation agents; root integrates and reviews.

## Review Focus

- Unicode/long paths and inaccessible folders: readable results or explicit errors.
- Cancellation and closing UI during scans: no stale mutation or use-after-free.
- Hard links/cloud placeholders: no inflated reclaim estimates or hydration.
- Changed cleanup candidates: revalidate and refuse unsafe operations.
- Huge result lists: lazy/virtualized display, background work, stable selection.

## Task 1: Backend (Astra)

Files: `windows/core.h`, `windows/*.cpp`, `windows/CMakeLists.txt`, `tests/windows/*`.
Produces: the declared `stale::win` API in `windows/core.h`, library target `stale_windows_core`, backend CLI/test targets.

- [x] Add fixture tests for scan totals, Unicode, cancellation, reparse skipping, index roundtrip/corruption, duplicate content and cleanup exclusions.
- [x] Implement enumeration, metadata, rollups, persistence, finder policies and recycle operations.
- [x] Build/run Windows tests; report commands and results.

## Task 2: Frontend (Astra)

Files: `desktop/*`.
Consumes: `windows/core.h`; produces target `stale_desktop` and functional Material UI.

- [x] Implement Material navigation, folder input/picker, overview, file list, filtering, sorting, details and cleanup workflow.
- [x] Wire async engine operations, cancellation, GUI-thread model updates and lifecycle protection.
- [x] Compile using the fork; exercise empty/loading/error states and render/smoke test when available.

## Task 3: Integration (root)

Files: `CMakeLists.txt`, Windows build scripts, README, CI and packaging.

- [x] Discover toolchain and provide repeatable build instructions.
- [x] Build integrated executable and run backend tests.
- [x] Review both implementations and route corrections to owners.
- [x] Smoke-test executable against disposable fixtures; record actual limitations.

## Coordination ledger

Backend/frontend share only the header: frozen value-type API, changes coordinated through root. Each task's file ownership and tests agree with the spec. User's explicit implementation instruction and two-agent selection authorize execution of the discussed scope without another approval round. Working on `feat/windows-slint` in the current clean checkout; no additional worktree needed for disjoint ownership.

- Backend agent: `/root/backend` (Astra). Frontend agent: `/root/frontend` (Astra). No additional agents.
- Root review requested local partial-scan error handling, hardlink cleanup exclusion, scalable candidate indexing, real agent-cache providers, and managed-directory duplicate exclusions. Backend implemented these and fixture tests; final backend-only CTest passed.
- Backend reports successful opt-in recycling of a self-created fixture, plus passing long-path/Unicode, sparse/hardlink, ACL-denial and reparse tests. Root final integrated CTest passed (1/1, 1.02 seconds).
- Frontend integrated executable built at `build/windows/bin/Stale.exe`. Root inspected `build/overview.ppm` and requested denser control heights to preserve file-list space. Layout corrected and screenshots reviewed, including long Unicode candidate reasons; frontend smoke/lifecycle checks passed.
- Slint runtime/compiler bootstrap installed at `build/slint-install`; pinned source export is `build/slint-source`.
- User requested fixing the Slint fork directly, then opening a PR using STE. Fix committed and pushed as `46f3b237fc6a746356e9cd5535601b6d799637c0`, branch `fix/vector-font-source-path`, PR https://github.com/jethac/slint/pull/79. Three-line SourcePath correction; compiler failure reproduced before and Release runtime/compiler build passed after. No merge requested or performed. Stale now pins this commit; no local compatibility patch remains.
- Root verified final integrated build/CTest, corrected screenshots, and packaged executable smoke cases (empty=0, real duplicates=0, invalid root=1). ZIP: dist/Stale-Windows-x64.zip. Native keyboard/mouse interactions were not externally automated; clean-machine installation and CI have not run. Stale remains on the local feature branch. Existing macOS sources remain unchanged.
