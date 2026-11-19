const { execFileSync } = require("node:child_process");

function componentContext({ otherPaths, paths, sharedPaths }, context) {
  const commits = context.commits.filter(({ hash }) => {
    const touches = (pathspec) => execFileSync("git", [
      "diff-tree", "--root", "--no-commit-id", "--name-only", "--no-renames", "-r", "-m", "-z",
      hash, "--", ...pathspec,
    ], { cwd: context.cwd, env: context.env }).length > 0;
    return touches(paths) || (touches(sharedPaths) && !touches(otherPaths));
  });

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
