const path = require("node:path");

const componentName = process.env.RELEASE_COMPONENT;
const components = {
  android: {
    assets: ["android/CHANGELOG.md", "android/app/build.gradle.kts"],
    changelog: "android/CHANGELOG.md",
    paths: ["android", "common"],
  },
  dot: {
    assets: ["desktop/CHANGELOG.md", "desktop/CMakeLists.txt", "desktop/vcpkg.json"],
    changelog: "desktop/CHANGELOG.md",
    paths: ["common", "desktop"],
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
      message: `chore(release): ${componentName} \${nextRelease.version}`,
    }],
  ],
  tagFormat: `${componentName}-v\${version}`,
};
