# Contributing

This fork preserves the original macOS application and adds a Windows desktop
application. Keep platform-specific changes isolated unless a shared change is
intentional.

## Commits

Use Conventional Commits for new work:

- `feat(windows): add a capability`
- `fix(desktop): correct a behavior`
- `test(agent): cover a protocol case`
- `ci(windows): update the release pipeline`
- `docs: explain a workflow`

Keep commits focused. Preserve upstream history rather than rewriting it.
Do not commit scan reports, caches, build products, credentials, or local data.

## Windows checks

Install Visual Studio with Desktop development with C++ and its CMake tools,
and Rust 1.95 or newer. Run:

```powershell
./scripts/build-windows.ps1
./scripts/test-agent.ps1
./scripts/package-windows.ps1
```

The build script runs CTest. The agent checks run a disposable app and filesystem
fixture; they never confirm recycling. See [Windows documentation](docs/WINDOWS.md)
and [the agent protocol](desktop/AGENT_API.md).

## CI and preview releases

GitHub-hosted Windows runners build the pinned Slint source, run CTest and the
agent checks, then upload the portable ZIP and its SHA-256 checksum. The existing
macOS workflow continues to build and test the original application.

To publish a Windows preview, tag a reviewed commit on `main` with a new,
unique `windows-v<version>-preview.<number>` tag and push that tag:

```powershell
git tag -a windows-v0.1.0-preview.3 -m 'chore(release): Windows 0.1.0 preview 3'
git push origin windows-v0.1.0-preview.3
```

Use the next unused version; the commands above are an example. The Windows
workflow publishes a GitHub prerelease only after all Windows checks pass.
Failed tags publish nothing; fix the failure in a semantic commit and use a new
tag. Do not move previously published tags. Tags beginning with `v` belong to
the original macOS release workflow.

Windows preview binaries are unsigned and require the Microsoft Visual C++ x64
runtime. Release publishing uses the workflow's scoped GitHub token; no personal
access token or signing credentials are required.
