const path = require("node:path");

const androidPaths = [
  ".github/workflows/android-ci.yml",
  "android",
  "common/icons/android",
  "common/icons/owl*",
  "common/icons/png/owl*",
  "design/icons/android",
  "design/icons/owl*",
  "design/icons/png/owl*",
];
const sharedPaths = [
  ".github/release",
  ".github/workflows/semantic-release.yml",
  ".gitignore",
  "AGENTS.md",
  "LICENSE",
  "README.md",
  "common/icons/.gitattributes",
  "common/icons/README.md",
  "common/icons/export_icons.py",
  "common/icons/requirements.txt",
  "common/release",
  "common/scripts/coverage_patch.ps1",
  "design/icons/.gitattributes",
  "design/icons/README.md",
  "design/icons/export_icons.py",
  "design/icons/requirements.txt",
  "docs/design/DESIGN.md",
  "docs/design/implementation.md",
  "scripts/coverage_patch.ps1",
  "scripts/set_version.cjs",
];
const componentName = process.env.RELEASE_COMPONENT;
const components = {
  android: {
    assets: ["android/CHANGELOG.md", "android/app/build.gradle.kts"],
    changelog: "android/CHANGELOG.md",
    paths: [...androidPaths, ...sharedPaths],
  },
  dot: {
    assets: ["desktop/CHANGELOG.md", "desktop/CMakeLists.txt", "desktop/vcpkg.json"],
    changelog: "desktop/CHANGELOG.md",
    paths: [".", ...androidPaths.map((entry) => `:(exclude)${entry}`)],
  },
};

if (!Object.hasOwn(components, componentName)) {
  throw new Error("Set RELEASE_COMPONENT to android or dot");
}

const component = components[componentName];

module.exports = {
  branches: ["main"],
  plugins: [
    [require.resolve("./component_commits.cjs"), {
      paths: component.paths,
      preset: "conventionalcommits",
      presetConfig: {
        types: [
          { section: "Added", type: "feat" },
          { section: "Fixed", type: "fix" },
          { section: "Changed", type: "perf" },
          { section: "Reverted", type: "revert" },
        ],
      },
    }],
    [require.resolve("@semantic-release/changelog"), {
      changelogFile: component.changelog,
    }],
    [require.resolve("@semantic-release/exec"), {
      prepareCmd: `node "${path.join("common", "release", "set_version.cjs")}" ${componentName} \${nextRelease.version}`,
    }],
    [require.resolve("@semantic-release/git"), {
      assets: component.assets,
      message: `chore(release): ${componentName} \${nextRelease.version} [skip ci]`,
    }],
  ],
  tagFormat: `${componentName}-v\${version}`,
};
