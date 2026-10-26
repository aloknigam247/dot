const { readFileSync, writeFileSync } = require("node:fs");
const path = require("node:path");

const stableVersionPattern = /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;
const [component, version] = process.argv.slice(2);

function isStableVersion(value) {
  return typeof value === "string" && value.match(stableVersionPattern)?.[0] === value;
}

function validateVersionUpdate(currentVersion) {
  if (!isStableVersion(currentVersion)) {
    throw new Error(`Expected a stable current version, found ${currentVersion}`);
  }

  const currentParts = currentVersion.split(".").map(BigInt);
  const nextParts = version.split(".").map(BigInt);
  for (let index = 0; index < currentParts.length; index += 1) {
    if (nextParts[index] > currentParts[index]) {
      break;
    }
    if (nextParts[index] < currentParts[index]) {
      throw new Error(`Refusing to replace version ${currentVersion} with older version ${version}`);
    }
  }
}

function setAndroidVersion() {
  const codePattern = /^([ \t]*versionCode[ \t]*=[ \t]*)([0-9]+)([ \t]*\r?)$/gm;
  const namePattern = /^([ \t]*versionName[ \t]*=[ \t]*")([^"\r\n]+)("[ \t]*\r?)$/gm;
  const versionFile = path.join("android", "app", "build.gradle.kts");
  const content = readFileSync(versionFile, "utf8");
  const codeMatches = [...content.matchAll(codePattern)];
  const nameMatches = [...content.matchAll(namePattern)];

  if (codeMatches.length !== 1 || nameMatches.length !== 1) {
    throw new Error("Expected exactly one literal Android versionCode and versionName");
  }

  const currentVersion = nameMatches[0][2];
  validateVersionUpdate(currentVersion);
  const currentCode = BigInt(codeMatches[0][2]);
  const nextCode = currentVersion === version ? currentCode : currentCode + 1n;
  if (currentCode < 1n || nextCode > 2100000000n) {
    throw new Error("Android versionCode must remain between 1 and 2100000000");
  }

  const updated = content
    .replace(codePattern, (_, prefix, current, suffix) => `${prefix}${nextCode}${suffix}`)
    .replace(namePattern, (_, prefix, current, suffix) => `${prefix}${version}${suffix}`);
  writeFileSync(versionFile, updated, "utf8");
  console.log(`Set Android version to ${version} (versionCode ${nextCode})`);
}

function setDotVersion() {
  const cmakePattern = /^(project\(dot[ \t]+LANGUAGES[ \t]+CXX[ \t]+VERSION[ \t]+)([^ \t\r\n)]+)(\))/gm;
  const manifestPattern = /^([ \t]*"version"[ \t]*:[ \t]*")([^"\r\n]+)(")/gm;
  const cmakeContent = readFileSync("CMakeLists.txt", "utf8");
  const cmakeMatches = [...cmakeContent.matchAll(cmakePattern)];
  const manifestContent = readFileSync("vcpkg.json", "utf8");
  const manifest = JSON.parse(manifestContent);
  const manifestMatches = [...manifestContent.matchAll(manifestPattern)];

  if (cmakeMatches.length !== 1 || manifestMatches.length !== 1) {
    throw new Error("Expected exactly one dot project version and one vcpkg version field");
  }

  const currentVersion = cmakeMatches[0][2];
  if (currentVersion !== manifest.version || currentVersion !== manifestMatches[0][2]) {
    throw new Error("CMakeLists.txt and vcpkg.json must contain the same stable version");
  }
  validateVersionUpdate(currentVersion);

  const updatedCmake = cmakeContent.replace(cmakePattern, (_, prefix, current, suffix) => `${prefix}${version}${suffix}`);
  const updatedManifest = manifestContent.replace(
    manifestPattern, (_, prefix, current, suffix) => `${prefix}${version}${suffix}`,
  );

  writeFileSync("CMakeLists.txt", updatedCmake, "utf8");
  writeFileSync("vcpkg.json", updatedManifest, "utf8");
  console.log(`Set CMake and vcpkg versions to ${version}`);
}

if (process.argv.length !== 4 || !["android", "dot"].includes(component) || !isStableVersion(version)) {
  throw new Error("Expected android or dot followed by one stable MAJOR.MINOR.PATCH version");
}

if (component === "android") {
  setAndroidVersion();
} else {
  setDotVersion();
}
