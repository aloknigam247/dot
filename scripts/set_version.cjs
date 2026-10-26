const { readFileSync, writeFileSync } = require("node:fs");

const cmakePattern = /^(project\(dot[ \t]+LANGUAGES[ \t]+CXX[ \t]+VERSION[ \t]+)([^ \t\r\n)]+)(\))/gm;
const manifestPattern = /^([ \t]*"version"[ \t]*:[ \t]*")([^"\r\n]+)(")/gm;
const stableVersionPattern = /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/;
const version = process.argv[2];

function isStableVersion(value) {
  return typeof value === "string" && value.match(stableVersionPattern)?.[0] === value;
}

if (process.argv.length !== 3 || !isStableVersion(version)) {
  throw new Error("Expected one stable MAJOR.MINOR.PATCH version");
}

const cmakeContent = readFileSync("CMakeLists.txt", "utf8");
const cmakeMatches = [...cmakeContent.matchAll(cmakePattern)];
const manifestContent = readFileSync("vcpkg.json", "utf8");
const manifest = JSON.parse(manifestContent);
const manifestMatches = [...manifestContent.matchAll(manifestPattern)];

if (cmakeMatches.length !== 1 || manifestMatches.length !== 1) {
  throw new Error("Expected exactly one dot project version and one vcpkg version field");
}

const currentVersion = cmakeMatches[0][2];
if (!isStableVersion(currentVersion) || currentVersion !== manifest.version
    || currentVersion !== manifestMatches[0][2]) {
  throw new Error("CMakeLists.txt and vcpkg.json must contain the same stable version");
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

const updatedCmake = cmakeContent.replace(cmakePattern, (_, prefix, current, suffix) => `${prefix}${version}${suffix}`);
const updatedManifest = manifestContent.replace(
  manifestPattern, (_, prefix, current, suffix) => `${prefix}${version}${suffix}`,
);

writeFileSync("CMakeLists.txt", updatedCmake, "utf8");
writeFileSync("vcpkg.json", updatedManifest, "utf8");
console.log(`Set CMake and vcpkg versions to ${version}`);
