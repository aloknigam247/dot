const { execFileSync } = require("node:child_process");

function componentContext({ paths }, context) {
  const commits = context.commits.filter(({ hash }) => execFileSync("git", [
    "diff-tree", "--root", "--no-commit-id", "--name-only", "--no-renames", "-r", "-m", "-z",
    hash, "--", ...paths,
  ], { cwd: context.cwd, encoding: "utf8", env: context.env }).length > 0);

  return { ...context, commits };
}

exports.analyzeCommits = async (options, context) => {
  const { analyzeCommits } = await import("@semantic-release/commit-analyzer");
  return analyzeCommits(options, componentContext(options, context));
};

exports.generateNotes = async (options, context) => {
  const { generateNotes } = await import("@semantic-release/release-notes-generator");
  return generateNotes(options, componentContext(options, context));
};
