# dot — Setup & Build Runbook

> All steps run in **PowerShell 7 (`pwsh`)** on Windows. Versions below are the ones that actually built successfully (May 2026).

Desktop build, test, and coverage commands run from the checkout's `desktop\` directory.
For Android, see its [build instructions](android/README.md).

## 1. Install Visual Studio Build Tools 2026

> ⚠️ Don't use `winget install Microsoft.VisualStudio.BuildTools --override "--quiet ..."` — winget reports success once the bootstrapper downloads, but the actual workload install silently fails to land. Use the direct bootstrapper instead.

Download and run the bootstrapper directly:

```ps1
$dest = "$env:TEMP\vs_buildtools.exe"
Invoke-WebRequest -Uri "https://aka.ms/vs/18/stable/vs_buildtools.exe" -OutFile $dest -UseBasicParsing
Start-Process -FilePath $dest -ArgumentList @(
  "--passive","--norestart","--wait",
  "--add","Microsoft.VisualStudio.Workload.VCTools","--includeRecommended",
  "--add","Microsoft.VisualStudio.Component.Windows11SDK.26100"
) -Verb RunAs -Wait
```

UAC prompt → Yes. A VS installer UI appears with progress; leave it open until done (~15 min, ~7 GB).

Verify (`vswhere` needs `-prerelease` because VS 2026 is still flagged that way in some catalogs):

```ps1
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -all -prerelease -products * -format value -property installationPath
```

Should print `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`. The MSVC toolset lands at `…\18\BuildTools\VC\Tools\MSVC\14.51.36231\`.

## 2. Install CMake 4.3.x (required for the VS 2026 generator)

Your existing CMake 3.31.6 is too old — the `Visual Studio 18 2026` generator was added in CMake 4.2.

```ps1
winget install --id Kitware.CMake --version 4.3.2 --accept-package-agreements --accept-source-agreements
```

UAC → Yes. Verify:

```ps1
cmake --version   # should print 4.3.x
```

## 3. Bootstrap vcpkg

```ps1
git clone https://github.com/microsoft/vcpkg.git D:\vcpkg
& D:\vcpkg\bootstrap-vcpkg.bat
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "D:\vcpkg", "User")
```

**Open a new PowerShell window** so `VCPKG_ROOT` propagates.

## 4. Pin the vcpkg baseline

```ps1
Set-Location (Join-Path (git rev-parse --show-toplevel) "desktop")
$sha = git -C $env:VCPKG_ROOT rev-parse HEAD
(Get-Content .\vcpkg-configuration.json -Raw) -replace 'REPLACE_WITH_CURRENT_BASELINE_COMMIT_SHA', $sha | Set-Content .\vcpkg-configuration.json -NoNewline
```

> Note: the dependency in `vcpkg.json` is named **`msgpack`** (not `msgpack-cxx`). The vcpkg port "msgpack" v7.0.0 IS the C++ msgpack-cxx library; the CMake `find_package(msgpack-cxx CONFIG REQUIRED)` call in `CMakeLists.txt` still works.

## 5. First configure — slow (Qt compile)

```ps1
Set-Location (Join-Path (git rev-parse --show-toplevel) "desktop")
cmake --preset release   # or: cmake --preset dev  (Debug escape hatch)
```

The first configure runs vcpkg, which **compiles Qt 6.10.3 from source — expect 1–3 hours**. Subsequent configures reuse the cache (~10s).

Produces `build\release\dot.sln` (or `build\dev\dot.sln` for Debug) plus MSBuild project files.

## 6. Build

```ps1
cmake --build --preset release --parallel   # or: cmake --build --preset dev --parallel
```

`--parallel` (no number) uses all cores (MSBuild `/m`) — the same flag CI uses. To avoid typing it,
set `$env:CMAKE_BUILD_PARALLEL_LEVEL` in your PowerShell profile.

Produces `build\release\RelWithDebInfo\dot.exe` (Debug: `build\dev\Debug\dot.exe`) and the test binaries.
The post-build deployment in `CMakeLists.txt` copies the required Qt runtime next to the executable.

## 7. Run the tests

```ps1
ctest --preset release   # or: ctest --preset dev
```

- 4 unit tests (Tier 1, <2s)
- 6 integration tests with real `nvim --embed --clean` (Tier 2, ~15s)
- 1 QML test suite covering 3 components (Tier 3, ~5s)

Total wall clock: <30s. Expected result: **11/11 passing**.

## 8. Launch

```ps1
.\build\release\RelWithDebInfo\dot.exe   # or: .\build\dev\Debug\dot.exe
```

A window opens, attaches to nvim, and you'll see a `[No Name]` buffer with cursor in normal mode.

## 9. Coverage & CI toolchain (contributors)

These are only needed to run the coverage gate locally or to understand what CI does — a plain
build/test (steps 1–8) does not require them.

### CI trigger boundaries

[Desktop CI](.github/workflows/ci.yml) and [Android CI](.github/workflows/android-ci.yml) use native path filters following
the [repository layout](README.md#repository-layout). Shared changes run both workflows; documentation inside an included
component directory also runs its validation. Unrelated root documentation and design changes do not start validation.
The workflow YAML files define the exact filters; an included change runs the complete existing workflow.

Skipped workflows may leave required checks or coverage requirements unsatisfied. Check the effective
[repository rules](https://github.com/aloknigam247/dot/rules) and GitHub's
[native filter behavior](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow#using-filters-to-target-specific-paths-for-pull-request-or-push-events).
Report a merge-policy conflict instead of bypassing or weakening the requirements.

### Neovim on PATH (integration tier)

The Tier-2 integration tests spawn a real `nvim --embed` and locate it with
`QStandardPaths::findExecutable("nvim")`, so `nvim` must be on `PATH`:

```ps1
winget install --id Neovim.Neovim --accept-package-agreements --accept-source-agreements
nvim --version   # confirm it resolves on PATH (open a new shell if winget just added it)
```

### clang-tidy static analysis

The `tidy` CI job fails on any clang-tidy finding under `src/` or `include/`. `.clang-tidy` is the
source of truth for the enabled check set. Install `clang-tidy` on `PATH` as shown in the
[`tidy` CI job](.github/workflows/ci.yml), then generate the Ninja-based compile database:

```ps1
cmake --preset tidy                       # generates build/tidy/compile_commands.json
clang-tidy -p build/tidy --quiet (git ls-files 'src/*.cpp')
```

The `tidy` preset uses Ninja + `cl`, so run it from a Developer PowerShell (or after `vcvars64.bat`)
so `cl` is on `PATH`.

### Native code coverage

Coverage uses **`Microsoft.CodeCoverage.Console`**, which ships with the VS 2026 "Code coverage
tools" component (installed by the `--includeRecommended` VCTools workload in step 1). Locate it and
collect over a **Debug** build (RelWithDebInfo reports covered = 0 because of inlining). Generate
checkout-specific source filters from `cov.runsettings` before collecting:

```ps1
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$cc = Get-ChildItem "$vs\Common7\IDE\Extensions\Microsoft\CodeCoverage.Console\Microsoft.CodeCoverage.Console.exe" |
        Select-Object -First 1 -ExpandProperty FullName

cmake --preset dev; cmake --build --preset dev
pwsh -NoProfile -File scripts\coverage_settings.ps1 -OutPath _cov\coverage.runsettings
& $cc collect --output _cov\full.cobertura --output-format cobertura `
    --settings _cov\coverage.runsettings -- ctest --preset dev
```

Every test executable statically links `dot_lib`, so the report's own line-rate double-counts each
source line once per test module. `scripts/coverage_union.ps1` collapses that into a real union
figure over `src/` + `include/` and fails if it regresses below the floor:

```ps1
pwsh -NoProfile -File scripts\coverage_union.ps1 -CoberturaPath _cov\full.cobertura
```

Use `-MinRatio` to set the required coverage ratio. Covered-line counts are informational, not a separate gate.

### CI vcpkg binary cache (one-time, maintainer)

`.github/workflows/ci.yml` uses a **public GitHub Packages NuGet feed**
(`nuget.pkg.github.com/aloknigam247`) as vcpkg's binary cache, so Qt is compiled once per ABI and
restored on every later run. The first run (or any run after a vcpkg baseline / MSVC-toolset bump)
cold-builds Qt (1–3 h) and then publishes the blob. Two one-time requirements:

- The workflow's `GITHUB_TOKEN` already has `packages: write` (set in `ci.yml`); no PAT needed.
- After the first publish, open the package under **github.com/users/aloknigam247/packages** and set
  its visibility to **Public** — otherwise a private package hits the 500 MB free quota and later
  runs fail to restore. Public packages on a public repo have unlimited storage and transfer.

## Troubleshooting

| Symptom | Likely fix |
|---------|-----------|
| `Generator: Visual Studio 18 2026 not found` | CMake too old. Upgrade to 4.2+ (step 2). |
| vcpkg complains the baseline doesn't contain `msgpack-cxx` | The vcpkg port is named `msgpack` (not `msgpack-cxx`). Already corrected in `vcpkg.json`. |
| `find_package(Qt6 6.11 ...)` version mismatch | vcpkg baseline ships Qt 6.10.3, not 6.11.x. `CMakeLists.txt` is set to `6.10`. |
| Test executables hang at exactly 60s timeout | DLL not found at launch. `windeployqt` post-build step should prevent this; verify it ran. |
| MSVC Runtime Library error dialog at launch | Missing platform plugin. Confirm `qwindowsd.dll` exists next to `dot.exe` under `platforms/`. |
| `dot.exe` runs but window doesn't appear (Hwnd=0) | QML failed to load. Run with `QT_FORCE_STDERR_LOGGING=1 QT_ASSUME_STDERR_HAS_CONSOLE=1` and read stderr. |
| `Module "Dot" plugin "dot_libplugin" not found` | The plugin needs to be statically linked into the consumer. Both `dot` and `test_qml` link it in `CMakeLists.txt`. |
| Orphan `nvim.exe` processes after dot crashes | Normal — kill them: `Get-Process nvim \| Stop-Process -Force`. The main app now wires `disconnected → app.quit()`, so this only happens on crashes. |

## Updating dependencies later

```ps1
git -C $env:VCPKG_ROOT pull
$sha = git -C $env:VCPKG_ROOT rev-parse HEAD
(Get-Content .\vcpkg-configuration.json -Raw) -replace '"baseline": "[a-f0-9]+"', "`"baseline`": `"$sha`"" | Set-Content .\vcpkg-configuration.json -NoNewline
cmake --preset release   # vcpkg picks up the new baseline  (or: cmake --preset dev)
```
