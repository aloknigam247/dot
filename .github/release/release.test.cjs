const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const { randomUUID } = require("node:crypto");
const { copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } = require("node:fs");
const { tmpdir } = require("node:os");
const path = require("node:path");
const { test } = require("node:test");
const { pathToFileURL } = require("node:url");
const config = require("./release.config.cjs");

const cmake = "cmake_minimum_required(VERSION 4.2)\nproject(dot LANGUAGES CXX VERSION 1.2.3)\n";
const manifest = '{\n  "name": "dot",\n  "version": "1.2.3",\n  "dependencies": ["qtbase"]\n}\n';
const versionScript = path.resolve(__dirname, "..", "..", "scripts", "set_version.cjs");

function fixture(t, cmakeContent = cmake, manifestContent = manifest) {
  const directory = path.join(tmpdir(), `dot-version-${randomUUID()}`);
  mkdirSync(directory);
  t.after(() => rmSync(directory, { force: true, recursive: true }));
  writeFileSync(path.join(directory, "CMakeLists.txt"), cmakeContent);
  writeFileSync(path.join(directory, "vcpkg.json"), manifestContent);
  return directory;
}

function run(directory, command, args, env = process.env) {
  const result = spawnSync(command, args, { cwd: directory, encoding: "utf8", env });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  return result.stdout.trim();
}

for (const [name, newline] of [["CRLF", "\r\n"], ["LF", "\n"]]) {
  test(`version updates preserve ${name} formatting and unrelated metadata`, (t) => {
    const cmakeContent = cmake.replaceAll("\n", newline);
    const manifestContent = manifest.replaceAll("\n", newline);
    const directory = fixture(t, cmakeContent, manifestContent);
    run(directory, process.execPath, [versionScript, "2.0.0"]);
    assert.equal(readFileSync(path.join(directory, "CMakeLists.txt"), "utf8"),
      cmakeContent.replace("1.2.3", "2.0.0"));
    assert.equal(readFileSync(path.join(directory, "vcpkg.json"), "utf8"),
      manifestContent.replace("1.2.3", "2.0.0"));
  });
}

for (const version of ["1.2.3", "1.2.4", "1.10.0", "2.0.0"]) {
  test(`accepts a non-decreasing stable version ${version}`, (t) => {
    const directory = fixture(t);
    run(directory, process.execPath, [versionScript, version]);
    assert.equal(JSON.parse(readFileSync(path.join(directory, "vcpkg.json"), "utf8")).version, version);
  });
}

test("the checked-in project files accept their existing version unchanged", (t) => {
  const root = path.resolve(__dirname, "..", "..");
  const cmakeContent = readFileSync(path.join(root, "CMakeLists.txt"), "utf8");
  const manifestContent = readFileSync(path.join(root, "vcpkg.json"), "utf8");
  const version = JSON.parse(manifestContent).version;
  const directory = fixture(t, cmakeContent, manifestContent);
  run(directory, process.execPath, [versionScript, version]);
  assert.equal(readFileSync(path.join(directory, "CMakeLists.txt"), "utf8"), cmakeContent);
  assert.equal(readFileSync(path.join(directory, "vcpkg.json"), "utf8"), manifestContent);
});

for (const args of [
  [],
  [""],
  ["01.2.3"],
  ["1.2"],
  ["1.2.3\n"],
  ["1.2.3+build"],
  ["1.2.3-beta.1"],
  ["1.2.3", "unexpected"],
  ["v1.2.3"],
]) {
  test(`rejects invalid arguments ${JSON.stringify(args)} without modifying files`, (t) => {
    const directory = fixture(t);
    const result = spawnSync(process.execPath, [versionScript, ...args], { cwd: directory, encoding: "utf8" });
    assert.ifError(result.error);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Expected one stable MAJOR\.MINOR\.PATCH version/);
    assert.equal(readFileSync(path.join(directory, "CMakeLists.txt"), "utf8"), cmake);
    assert.equal(readFileSync(path.join(directory, "vcpkg.json"), "utf8"), manifest);
  });
}

for (const [name, cmakeContent, manifestContent, version, error] of [
  ["duplicate CMake project", cmake + cmake, manifest, "2.0.0", /Expected exactly one/],
  ["duplicate manifest version", cmake, manifest.replace('"name": "dot",', '"version": "1.2.3",'),
    "2.0.0", /Expected exactly one/],
  ["major downgrade", cmake, manifest, "0.9.9", /older version/],
  ["malformed JSON", cmake, "{", "2.0.0", /SyntaxError/],
  ["minor downgrade", cmake, manifest, "1.1.9", /older version/],
  ["missing CMake project", "", manifest, "2.0.0", /Expected exactly one/],
  ["missing manifest version", cmake, '{"name":"dot"}', "2.0.0", /Expected exactly one/],
  ["patch downgrade", cmake, manifest, "1.2.2", /older version/],
  ["unstable current version", cmake.replace("1.2.3", "1.2.3-beta.1"),
    manifest.replace("1.2.3", "1.2.3-beta.1"), "2.0.0", /same stable version/],
  ["version mismatch", cmake, manifest.replace("1.2.3", "1.2.4"), "2.0.0", /same stable version/],
]) {
  test(`rejects ${name} without modifying either file`, (t) => {
    const directory = fixture(t, cmakeContent, manifestContent);
    const result = spawnSync(process.execPath, [versionScript, version], { cwd: directory, encoding: "utf8" });
    assert.ifError(result.error);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, error);
    assert.equal(readFileSync(path.join(directory, "CMakeLists.txt"), "utf8"), cmakeContent);
    assert.equal(readFileSync(path.join(directory, "vcpkg.json"), "utf8"), manifestContent);
  });
}

test("release configuration only prepares version files and tags", () => {
  assert.deepEqual(config.branches, ["main"]);
  assert.equal(config.tagFormat, "v${version}");
  assert.deepEqual(config.plugins.map(([plugin]) => plugin), [
    "@semantic-release/commit-analyzer",
    "@semantic-release/release-notes-generator",
    "@semantic-release/changelog",
    "@semantic-release/exec",
    "@semantic-release/git",
  ].map((plugin) => require.resolve(plugin)));
  assert.deepEqual(config.plugins.at(-1)[1].assets, ["CHANGELOG.md", "CMakeLists.txt", "vcpkg.json"]);
  assert.equal(config.plugins.at(-1)[1].message, "chore(release): ${nextRelease.version} [skip ci]");
});

test("conventional commits select standard SemVer bumps", async (t) => {
  const [plugin, options] = config.plugins[0];
  const { analyzeCommits } = await import(pathToFileURL(plugin).href);
  for (const [message, expected] of [
    ["chore(release): 1.2.3 [skip ci]", null],
    ["docs: update documentation", null],
    ["feat: add a feature", "minor"],
    ["feat!: change the API", "major"],
    ["fix: correct a defect", "patch"],
    ["fix: change defaults\n\nBREAKING CHANGE: defaults are incompatible.", "major"],
    ["perf: reduce allocations", "patch"],
    ["refactor: simplify internals", null],
    ["revert: remove a feature", null],
    ["revert: remove a feature\n\nThis reverts commit abcdef0123456789.", "patch"],
  ]) {
    await t.test(message.split("\n")[0], async () => {
      const actual = await analyzeCommits(options, {
        commits: [{ hash: "1234567", message }],
        cwd: __dirname,
        logger: { log() {} },
      });
      assert.equal(actual, expected);
    });
  }
});

test("semantic-release pushes synchronized version commits and tags to a local remote", (t) => {
  const directory = fixture(t, cmake.replace("1.2.3", "0.1.0"), manifest.replace("1.2.3", "0.1.0"));
  const env = {
    ...process.env,
    GH_TOKEN: "",
    GITHUB_ACTIONS: "true",
    GITHUB_EVENT_NAME: "pull_request",
    GITHUB_REF: "refs/pull/1/merge",
    GITHUB_TOKEN: "",
    GIT_TERMINAL_PROMPT: "0",
  };
  const remote = path.join(directory, "remote.git");
  const source = path.join(directory, "source");
  mkdirSync(source);
  mkdirSync(path.join(source, "scripts"));
  for (const file of ["CMakeLists.txt", "vcpkg.json"]) {
    copyFileSync(path.join(directory, file), path.join(source, file));
  }
  copyFileSync(versionScript, path.join(source, "scripts", "set_version.cjs"));
  const git = (...args) => run(source, "git", args, env);
  const cli = path.join(path.dirname(require.resolve("semantic-release")), "bin", "semantic-release.js");
  // --no-ci disables CI verification, but branch discovery still reads Actions metadata.
  const release = () => run(source, process.execPath, [
    cli, "--no-ci", "--extends", path.join(__dirname, "release.config.cjs"),
    "--repository-url", pathToFileURL(remote).href,
  ], { ...env, GITHUB_ACTIONS: "" });

  git("init", "--quiet", "--bare", "--initial-branch=main", remote);
  git("init", "--quiet", "--initial-branch=main");
  for (const [key, value] of [
    ["commit.gpgSign", "false"],
    ["core.autocrlf", "false"],
    ["core.hooksPath", path.join(source, ".git", "hooks")],
    ["tag.gpgSign", "false"],
    ["user.email", "version-test@example.invalid"],
    ["user.name", "Version test"],
  ]) {
    git("config", key, value);
  }
  git("add", ".");
  git("commit", "--quiet", "-m", "feat: start the product");
  git("remote", "add", "origin", remote);
  git("push", "--quiet", "--set-upstream", "origin", "main");

  const releaseOutput = release();
  assert.equal(git("log", "-1", "--format=%s"), "chore(release): 1.0.0 [skip ci]", releaseOutput);
  assert.equal(JSON.parse(readFileSync(path.join(source, "vcpkg.json"), "utf8")).version, "1.0.0");
  assert.equal(readFileSync(path.join(source, "CMakeLists.txt"), "utf8"), cmake.replace("1.2.3", "1.0.0"));
  assert.match(readFileSync(path.join(source, "CHANGELOG.md"), "utf8"), /### Added/);
  assert.match(readFileSync(path.join(source, "CHANGELOG.md"), "utf8"), /start the product/);
  assert.equal(git("status", "--porcelain"), "");
  assert.deepEqual(git("diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD").split(/\r?\n/),
    ["CHANGELOG.md", "CMakeLists.txt", "vcpkg.json"]);
  assert.equal(git("rev-parse", "v1.0.0"), git("rev-parse", "HEAD"));
  assert.equal(git("--git-dir", remote, "rev-parse", "main"), git("rev-parse", "HEAD"));
  assert.equal(git("--git-dir", remote, "rev-parse", "v1.0.0"), git("rev-parse", "HEAD"));

  const releasedHead = git("rev-parse", "HEAD");
  release();
  assert.equal(git("rev-parse", "HEAD"), releasedHead);
  git("commit", "--quiet", "--allow-empty", "-m", "docs: clarify usage");
  git("push", "--quiet");
  const docsHead = git("rev-parse", "HEAD");
  release();
  assert.equal(git("rev-parse", "HEAD"), docsHead);
  assert.equal(git("tag", "--list"), "v1.0.0");
});
