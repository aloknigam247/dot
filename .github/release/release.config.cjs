const path = require("node:path");

module.exports = {
  branches: ["main"],
  plugins: [
    [require.resolve("@semantic-release/commit-analyzer"), {
      preset: "conventionalcommits",
    }],
    [require.resolve("@semantic-release/release-notes-generator"), {
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
      changelogFile: "CHANGELOG.md",
    }],
    [require.resolve("@semantic-release/exec"), {
      prepareCmd: `node "${path.join("scripts", "set_version.cjs")}" \${nextRelease.version}`,
    }],
    [require.resolve("@semantic-release/git"), {
      assets: ["CHANGELOG.md", "CMakeLists.txt", "vcpkg.json"],
      message: "chore(release): ${nextRelease.version} [skip ci]",
    }],
  ],
  tagFormat: "v${version}",
};
