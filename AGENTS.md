# Project instructions

## Development and test workflow

- Use PowerShell 7 (`pwsh`) and a VS 2022 x64 developer environment with CMake 3.21+ and Node.js 22+. The extension uses Node's built-in runner and has no dependencies to install.
- For focused code changes, configure/build the Release preset, then run `scripts/test-changed.ps1`. Use `-List` to inspect selections, `-Base <ref>` (or `TEST_BASE`) to choose the Git base, or pass explicit repository-relative paths. Rebuild after changing C++ sources; the selector does not build.
- The selector runs smoke plus related tests. Build/resource changes select the complete CTest set; broad shared changes retain smoke coverage. Keep mappings and smoke coverage current when adding targets. Unmapped code must not silently skip checks.
- For development/release script changes, run `pwsh -NoProfile -File scripts/tests/<name>.test.ps1` for the relevant script. CI runs these small regression harnesses before building. They use temporary fixtures and need no extra test framework.
- Run relevant checks once after code is complete. Documentation/comment-only changes do not need another application test run; don't repeat passing checks after edits that cannot affect their result.
- Before handing over a Release, run `scripts/check-release.ps1`: configure/build, full CTest, extension tests, lifecycle smoke, imports, package/hash verification. Focused `-Skip...` runs are not release validation; skipping a validation check also requires `-SkipPackage`.
- CI runs changed tests for pull requests and complete Release validation for `master`/`main`, tags, and manual runs. Documentation-only changes skip CI. Keep local and CI commands synchronized.

## Validated release builds

- After a Release build passes the relevant checks, run `scripts/package-release.ps1` before reporting completion. The user expects the usable release in `dist/`, not only under `build/`.
- Keep `dist/Glint.exe`, both READMEs, `dist/docs/`, `dist/extension/`, the five runtime files in `dist/browser-extension/`, and the root `Glint-win-x64.zip` synchronized with the validated build and current sources.
- Preserve the `dist/browser-extension/` location because existing browser profiles may load their extension from that path.
- Verify the published EXE and packaged files match their sources. Link `dist/Glint.exe` in the final response.
- If a running process prevents replacement, report that explicitly; do not silently leave an old release in `dist/` or terminate a user process without authorization.
