# Shared assets and tooling

## Icons

[icons/README.md](icons/README.md) defines editable masters, generated outputs, and export/check commands.
[export_icons.py](icons/export_icons.py) owns generation; [requirements.txt](icons/requirements.txt) owns dependencies.

Consumers are [android/app/build.gradle.kts](../android/app/build.gradle.kts),
[desktop/resources/dot.qrc](../desktop/resources/dot.qrc), and [desktop/resources/dot.rc](../desktop/resources/dot.rc).
Check the consuming manifests when changing asset paths.

## Release tooling

- [component_commits.cjs](release/component_commits.cjs): filters commits by component paths for analysis and notes.
- [package.json](release/package.json): release-tool dependencies and Node requirements.
- [release.config.cjs](release/release.config.cjs): component paths, tags, changelogs, and version-update assets.
- [set_version.cjs](release/set_version.cjs): validates and updates the component's persisted version fields.

The [semantic-release workflow](../.github/workflows/semantic-release.yml) invokes these from the repository root.
Use [README versioning](../README.md#versioning) for release policy; do not hand-maintain another version list here.
Shared-path changes may affect both components according to the release configuration and CI path filters.

## Coverage

[coverage_patch.ps1](scripts/coverage_patch.ps1) checks changed executable lines against Cobertura or JaCoCo reports.
Its parameter help defines report/path mapping; use the consuming [Android](../.github/workflows/android-ci.yml) and
[desktop](../.github/workflows/ci.yml) workflow calls for component-specific arguments.
