---
name: triage
description: Use when the user wants to triage a task/bug/idea into a well-formed GitHub issue for a future agent to implement. Discusses the task, investigates what needs doing and where, checks existing issues for duplicates (and warns) and for parents or blockers (and links them natively), classifies it — labelling investigation-only work `spike` — and creates a GitHub issue only after the user accepts. Also rewrites existing issues into the same format. Trigger phrases include "triage", "triage this", "create an issue", "file an issue", "turn this into a task", "raise a ticket", "rewrite this issue".
---

# Triage

Turn a rough task, bug, or idea into a **well-formed GitHub issue** that a *different agent* can implement later with no prior conversation context. The skill discusses the task, investigates the codebase to find what needs doing and where, classifies it, and — **only after the user explicitly accepts** — creates the issue in this project's GitHub repo.

## Principles

- **Discuss first, create last.** Never create issues before explicit user acceptance of the complete draft plan, including every parent, child, label, relationship, and dependency.
- **Check for duplicates, parents, and blockers first.** Before drafting, search the repo's existing issues (open *and*
  closed). If the task duplicates an existing issue, say so and steer the user away from filing a redundant one. Link
  parents and blockers natively (see **Relations**).
- **Always rubber-duck the draft.** Every triage runs the rubber-duck agent over the drafted issue to catch flawed root causes, invariant violations, and weak tests *before* the user sees it.
- **The issue is for another agent, at another time.** Write it as a self-contained task: enough context, file references, and acceptance criteria that an agent with zero conversation history can pick it up and implement it.
- **Anchor to stable identifiers, not coordinates.** An issue is implemented at an unknown later time against a changed tree. Reference symbols, string literals, config keys, and search patterns — never line numbers, occurrence counts, or verbatim multi-line snapshots that the codebase will invalidate. A frozen inventory goes stale *and* misleads: the implementing agent trusts it as exhaustive and skips rediscovering occurrences added after the issue was filed. The issue points the way; the implementing agent re-discovers the exact scope.
- **Match detail to task type.** For a narrow **bug**, preserve a tight snippet of the actual defect as investigation evidence. For **features / refactors / renames / broad sweeps**, give a discovery recipe (search patterns + stable anchors) and compat/invariant rules, not an enumerated file list that will go stale.
- **Ask for missing details.** Do not guess when scope, expected behavior, or acceptance criteria are ambiguous.

## Source complexity and decomposition

Default to low/medium implementation leaves. Assess production code, scripts, shipped configuration, and build behavior:
behavioral scope, component coupling, interfaces, state/lifecycle changes, migration/invariants, and unresolved design.

- **Low:** a localized, well-understood behavioral change.
- **Medium:** one coherent, bounded outcome across a small connected set of components, with clear contracts and
  manageable invariants.
- **High:** broad or tightly coupled behavior, interfaces, or state beyond these bounds; follow the policy below.

Do not use LOC, file-count, or time quotas. If uncertainty prevents a bounded estimate, investigate or clarify before
drafting implementation work. Unknown is not a rating; clarify genuinely unknown scope before assigning one.

Exclude test additions/updates, fixtures, harnesses, matrix breadth, coverage, test runtime, and test-writing effort
from complexity and splitting decisions. Keep tests and validation attached to each implementation slice.
Large test work alone does not justify splitting it into test-only work or omitting tests.
Do not split pure docs/tests/spikes solely for size.
Docs, research, and test-only tasks with no production changes have low **source** complexity, regardless of effort.

Every implementation leaf must deliver a concrete, independently usable or demonstrable outcome, with a named consumer,
intended use, and completion demonstration. This may be user behavior or an actually consumable API, CLI, library, or
artifact. Prerequisites are allowed, but no leaf may end as unused schema, scaffolding, or transport awaiting a later
integration issue to become useful. Passing tests/builds alone is not the feature deliverable. Docs and research
findings remain valid deliverables for those task types.

Prefer smaller complete functional slices, not artificial file/layer/test splits. If these cannot deliver useful
outcomes, propose merging the necessary tasks: explain exactly which scopes combine, the outcome/demo, honest
revised source complexity, and tradeoff. Use `ask_user` to approve the **specific merge and any complexity increase
before adopting or filing it**. A high-complexity leaf is allowed only for that approved merged scope; record its scope,
actual rating, rationale, and approval in its draft. Never downgrade a rating to fit or treat approval as a blanket
exception. If declined, revise toward another complete outcome or leave the split unresolved; clarify unresolved design
before proceeding.

Use **tracking-only parents**, explicitly not implementation assignments, to coordinate decomposed work. Reassess each
child against this policy, using nested tracking parents as needed. Parents track scope, completion criteria, and
children; rate their aggregate requested source scope, not coordination overhead or a sum of child ratings.
The non-overlapping leaves collectively cover the full request. Preserve real dependencies and allow
independent children to proceed in parallel. Parent containment does not mean blocked-by.
Search duplicates and related issues for each proposed child; reuse suitable existing work instead of duplicating it.

Every issue attached as a child, including under an existing parent or a nested tracking parent, needs the exact
`sub-issue` label in addition to category and complexity labels **and** a native GitHub parent/sub-issue relationship.
A parent gets `sub-issue` only if it is itself a child.

## Relations

Express relations only through GitHub native links, never in an issue body:

- Parent / sub-issue → GitHub sub-issues.
- Blocked by / blocking (prerequisites, "start after #N", "depends on #N") → GitHub issue dependencies.
- Every other relation (related to, see also, follow-up to) is dropped: no heading, footer, or comment.

Bodies carry no relation headings (Related, Dependencies, Links), no footer meta line, and no "Prerequisite #N" or
"Sub-issue #N" text. Keep an inline `#N` only where a sentence needs it to be understood, such as an out-of-scope
boundary or behavior another issue owns; delete any sentence whose only purpose is to declare a relation.

The helper's `-Parent` links a new child at creation (step 6); add every other link through the REST API, where `<id>`
is the issue database id from `gh api repos/<owner>/<repo>/issues/<number> --jq .id`.

```ps1
gh api -X POST repos/<owner>/<repo>/issues/<parent-number>/sub_issues -F sub_issue_id=<child-id>
gh api -X POST repos/<owner>/<repo>/issues/<dependent-number>/dependencies/blocked_by -F issue_id=<blocker-id>
gh api repos/<owner>/<repo>/issues/<parent-number>/sub_issues --jq ".[].number"
gh api repos/<owner>/<repo>/issues/<dependent-number>/dependencies/blocked_by --jq ".[].number"
```

A 422 is not success. After any failed add, re-read the matching list and continue only if it contains the exact edge;
otherwise stop with an explicit failure and leave every issue body untouched.

## Input

The user provides a task, bug report, or idea in their request. It may be vague (e.g., "the cursor blink phase resets on every nvim window event") or a feature ask.

## Steps

### 1. Understand and discuss the task

1. Restate the task in your own words to confirm understanding.
2. Determine the **type of work**: is it a bug, a new feature, a refactor, etc.? (This informs the category later.)
3. Ask the user any clarifying questions needed to write a complete task. Use the `ask_user` tool. Typical gaps to probe:
   - Expected vs. actual behavior (for bugs)
   - Which **layer(s)** are affected (msgpack-rpc transport, redraw dispatch in `NvimConnector`, `GridModel`/`HighlightTable`/`ModeInfo` state, the `GridItem`/`CursorItem` paint path, `InputHandler` key encoding, or the QML shell)
   - Scope boundaries (what is explicitly *out of scope*)
   - Acceptance criteria / definition of done
   - Any constraints (per-frame redraw cost, the paint-path purity rule, no `msgpack::object` copies, thread affinity of `QRawFont`-backed objects, backwards-compat with new redraw event variants)
   - What tests should add/validate the fix (triaged issues include their tests as part of the fix)
4. Do **not** proceed to investigation until the task is clear enough to investigate.

### 2. Investigate "what needs to be done, and where" (via subagent)

Launch a fresh **general-purpose** subagent (via the Task tool) with **no prior conversation context** — build the prompt from scratch. This keeps the main conversation clean.

The subagent prompt must include:
- The **task description** as clarified in step 1
- The **task type** (bug/feature/refactor/etc.)
- The affected **layer(s)** if known
- The instruction to read the root `AGENTS.md` for the architecture (QML shell + `QQuickPaintedItem` grid renderer + embedded `nvim --embed` over msgpack-rpc), the "Top priorities" (correctness/performance) rules, the "Layout" map, and any nested `AGENTS.md` (`desktop/qml/AGENTS.md`, `desktop/src/AGENTS.md`, `desktop/tests/AGENTS.md`) for the subtree being touched before touching anything
- The **investigation goals** below

The subagent is responsible for:
1. Reading the root `AGENTS.md` (and the relevant nested `AGENTS.md`) for architecture, conventions, correctness/performance gates, and gotchas.
2. Using grep/glob/view to locate the work. **Scale the output to the task type** (see the "Match detail to task type" principle):
   - **Narrow bug:** capture the exact offending lines as a short verbatim snippet with a `file:line` ref as investigation evidence.
   - **Feature / refactor / rename / broad sweep:** do **not** enumerate every hit. Identify the **distinct surfaces/categories** the work touches, give **one representative entry point** per surface (as a stable anchor — a symbol, string literal, or config key), and record the **grep/glob patterns** that locate the rest. Prefer "find the remaining refs via `<pattern>`" over a frozen file list or an occurrence count.
3. Identifying the **component(s)** involved (e.g., `MsgpackRpc`, `NvimConnector`, `GridModel`, `HighlightTable`, `ModeInfo`, `GridItem`, `CursorItem`, `InputHandler`, a `*Model`, or a `desktop/qml/` component).
4. Confirming the **root cause** (for bugs) by tracing the actual code — not guessing. Remember the redraw event stream is the single source of truth; `ext_hlstate` makes `hl_attr_define` a 4-element event; `paint()` must be a pure function of (`GridModel`, `HighlightTable`, `ModeInfo`, cursor state) and must never reach back into RPC state.
5. Sketching a **proposed approach** consistent with existing patterns (redraw event handlers, the reactive `QObject`-proxy for Repeater delegates, `Q_PROPERTY`/`Q_SIGNAL` wiring, text-run batching by `(hl_id, font_state)`) and **without regressing per-frame redraw cost or breaking paint-path purity**.
6. Noting **testing implications**: which test tier maps to the changed source — Tier 1 `desktop/tests/unit/` (headless, `QTEST_GUILESS_MAIN`), Tier 2 `desktop/tests/integration/` (real `nvim --embed`, `minimal` QPA + software renderer), or Tier 3 `desktop/tests/qml/` (`qmltest`) — which **existing** tests will break and must be updated, and what **new** test cases would add/validate the fix (specific test function/method names and the behavior each asserts). For visual changes, note whether `desktop/scripts/screenshot_dot.ps1` / the `visual-validate-dot` skill is the authoritative gate.
7. Flagging any ambiguity or missing information the user still needs to resolve.
8. Applying **Source complexity and decomposition** to the actual source and proposing bounded slices when required.

The subagent must **return a structured report** containing:
- `surfaces` — the distinct categories of change the task touches (for a narrow bug this is usually one); for each, a short reason and its compat/invariant rules if any
- `affected` — representative entry points as `{file, symbol, why}`. For bugs, the specific site(s). For features/refactors/renames, **one canonical example per surface**, not an exhaustive inventory
- `searchStrategy` — the grep/glob patterns and stable anchors (symbol names, string literals like the `dot_yank` rpcnotify name, config keys) that let the implementing agent **re-discover the full scope** later. This replaces a frozen file enumeration
- `codeSnippet` — for a **narrow bug only**, a short verbatim excerpt of the offending lines with a `file:line` ref and language for fencing. Omit for features/refactors/renames (reference the pattern, not the lines)
- `components` — the component(s) involved
- `rootCause` — for bugs, the confirmed root cause with file:line references (or "n/a")
- `approach` — the proposed implementation approach
- `deliverables` — for each leaf, its concrete outcome, named consumer/use, and completion demonstration
- `mergeProposals` — any necessary merges under **Source complexity and decomposition**, with combined scopes, revised
  complexity, rationale, and tradeoff; proposals are not approval
- `slices` — if needed, the proposed tracking hierarchy, bounded child outcomes, source complexity/reasons, coverage,
  dependencies, and validation for each leaf
- `sourceComplexity` — low/medium/high or unresolved, with a concise source-based rationale and stable anchors
- `testing` — test files/impact, the exact `ctest --preset release -R <selector>` (or targeted test binary), a concrete **regression test snippet** whose assertions pin *this specific* fixed behavior (see Testing requirements below), and whether a screenshot/`visual-validate-dot` gate is required
- `openQuestions` — anything still unclear

Reference stable anchors (symbols, string literals, config keys), **never line numbers or occurrence counts** in the enumerated scope — the issue is implemented later against a moved tree.

If the subagent returns `openQuestions`, resolve them with the user (via `ask_user`) before drafting the issue.
Resolve `mergeProposals` under **Source complexity and decomposition** before adopting them in the draft plan.

### 3. Check for duplicate and related issues

Before classifying and drafting, search the repo's existing issues so the new issue isn't redundant and gets its native
relations. Search **both open and closed** issues.
Repeat this check for every proposed child, including recursively nested slices; carry suitable existing issues into the
plan rather than drafting duplicate work.

1. Build a few search queries from the task's key terms — the affected component (e.g. `GridItem`, `InputHandler`, `CursorItem`), the symptom, and any distinctive keywords. Run them with `gh`:

   ```pwsh
   gh issue list --state all --search "<keywords>" --limit 20 --json number,title,state,url
   # broader full-text search across the repo:
   gh search issues --repo <owner>/<repo> "<keywords>" --state all --limit 20 --json number,title,state,url
   ```

2. Read the candidate issues (`gh issue view <n>`) closely enough to judge each one. Classify every genuine match as either:
   - **Duplicate** — same root cause / same requested change. The new issue would be redundant.
   - **Relation** — a distinct task that is the new issue's parent, blocks it, or is blocked by it. Any other overlap
     (same area, see also, follow-up) is dropped (see **Relations**).

3. **Handle duplicates.** If one or more open duplicates exist, **stop and inform the user** (via `ask_user`) with the issue number(s), title(s), and URL(s), and recommend *not* filing a new one — instead point them at the existing issue (add a comment/upvote there if they want). Only continue to draft a new issue if the user explicitly confirms it is meaningfully different. A closed issue covering the same thing is worth surfacing too (it may need reopening rather than a fresh issue).

4. **Record relations** as `{number, title, relation}`, where `relation` is `parent`, `blocked by`, or `blocking`, for
   native linking in step 6.

### 4. Classify the task

Assign **one or more categories** from the fixed set below (these map 1:1 to GitHub labels):

| Category      | Use case                                        |
|---------------|-------------------------------------------------|
| `bug`         | Something is broken or behaves incorrectly      |
| `feature`     | New capability                                  |
| `refactor`    | Restructuring without behavior change           |
| `test`        | Adding or updating tests                        |
| `docs`        | Documentation changes                           |
| `chore`       | Maintenance, dependencies, cleanup              |
| `performance` | Speed/memory/efficiency improvements            |
| `tech-debt`   | Paying down accumulated shortcuts               |
| `spike`       | Investigation/exploration only — no code or docs changes (research, feasibility, root-cause analysis) |

Pick the categories that genuinely apply (one to three). The title prefix (step 5) sets the primary label: `ci`→`chore`,
`feat`→`feature`, `fix`→`bug`, `perf`→`performance`, and any other prefix its same-named label. Never apply a `fix`
label; it duplicates `bug`.

**Complexity metadata.** Every issue needs exactly one of `complexity: low`, `complexity: medium`,
or `complexity: high`, matching its rating under **Source complexity and decomposition**.
These labels supplement categories and the optional `sub-issue` label.

**Spike issues.** Apply `spike` when the task is purely investigation, exploration, or research whose deliverable is
*findings* (a decision, root-cause analysis, feasibility answer, or proposed design) and that **does not itself change
source code or documentation**. A spike has no other category label; if code/doc changes are expected, use the matching
category instead. It has no testing requirements: its Deliverables and decision criteria name the artifact to produce
(a written findings summary, a decision, or a follow-up issue) and are met by findings, not by tests passing. Category
exclusivity permits complexity and `sub-issue` labels.

### 5. Draft the complete plan and present it for acceptance

Apply the following draft/review/viewer workflow to **every** proposed parent and child, using separate drafts.
Keep implementation approaches and their tests in the leaves, not in tracking parents.
Show provisional child names and dependencies before numbers exist; show numbers for reused issues and existing parents.

1. Compose the issue **title** in Conventional Commit style: `<type>(scope): <short description>` (e.g., `fix(GridItem): cursor blink phase resets on nvim window event`). Use the component as the scope where it helps. For a spike, use `spike(scope): <question to answer>`.
   The prefix picks the body's heading set and its primary label (step 4).
2. Compose the issue **body** as a short intro paragraph stating what changes and why (never a Summary heading), then
   the title prefix's heading set as `##` headings, in order. Relations stay out of the body (see **Relations**).

   | Type (title prefixes) | Headings, in order |
   | --- | --- |
   | Bug (`bug`, `fix`) | Current behavior · Root cause · Proposed fix · Acceptance and validation |
   | Docs (`docs`) | Documentation gap · Proposed update · Review criteria |
   | Feature (`feat`, `feature`) | Problem · Proposed solution · Acceptance and validation |
   | Maintenance (`chore`, `ci`, `refactor`, `tech-debt`, `test`) | Current problem · Proposed change · Behavior to preserve (only when something must not change) · Validation |
   | Performance (`perf`, `performance`) | Current performance · Proposed improvement · Measurement and success criteria |
   | Spike (`spike`) | Questions to answer · Investigation scope · Deliverables and decision criteria |
   | Tracking parent (has sub-issues, any prefix) | Outcome (first line: "Tracking only — not an implementation assignment.") · Done when · Decisions (optional; only rules that bind several children) |

   Add Alternatives considered, Out of scope (what this task must not touch), or Risks to any type only when they carry
   real content. A tracking parent never lists its children: they exist only as native sub-issues, ordered only by
   blocked-by links between them.

   Fill a leaf's headings from the investigation report:
   - **First heading** (except spikes) — why this matters: the reported symptom or motivation and the affected
     layer(s). For a feature/refactor/rename, describe the *pattern* and the surfaces involved — do not paste a frozen
     multi-line snapshot or an occurrence count that the tree will invalidate.
   - **Root cause** — the confirmed `rootCause`, with the offending code as a fenced snippet (from the subagent's
     `codeSnippet`) under a `file:line` caption so the future agent sees exactly what code the task refers to.
   - **Proposed section** — the subagent's `approach`; the leaf's outcome, named consumer/use, and completion
     demonstration from `deliverables`; the source rating's rationale under **Source complexity and decomposition**,
     including any approved merged scope and its recorded approval; and any invariant that must **not** break
     (paint-path purity, per-frame cost, no `msgpack::object` copies, render-thread affinity, well-bracketed nvim
     keycodes), under Behavior to preserve for maintenance. Then the affected surfaces: for a narrow bug, the specific
     `file` -> `symbol` -> reason sites; for a feature/refactor/rename, the **surfaces/categories** (from the
     subagent's `surfaces`) with **one representative entry point each** plus the **discovery command** that finds the
     rest (from `searchStrategy`), prefixed with this standing note to the implementing agent: *"This is a starting
     map, not an exhaustive inventory — re-run the discovery searches below and treat their output as authoritative;
     occurrences may have been added since this issue was filed."* Reference the "Layout" / "Where to extend" guidance
     in `AGENTS.md` where relevant.
   - **Last heading** — a `- [ ]` checklist of concrete, verifiable outcomes, followed by the testing requirements.
   - **Testing requirements** — **required for any issue that changes code or docs.** Spell out the concrete test
     changes needed to add and/or validate the fix, so a future agent (or the user) can verify the change is done.
     Triaged issues are expected to include their tests as part of the fix — do **not** add a "tests only when
     explicitly requested" caveat here. Cover:
     - The exact test tier/target that maps to the changed source (Tier 1 `desktop/tests/unit/`, Tier 2 `desktop/tests/integration/`, or Tier 3 `desktop/tests/qml/`), and the test binary name where known (e.g., `test_input_handler`, `test_qml`).
     - **Existing tests that will break** and must be updated — name the test target and function, and say how. Reference test functions by name, not line number.
     - **New test cases** that add/validate the fix — proposed test function/method names and the specific behavior/assertion each covers. Include a concrete **regression test code snippet** (fenced) that the future agent can drop in. Use `QSignalSpy` for redraw assertions and `dumpAscii()` for grid snapshots per repo convention.
     - **Pin the fix, not incidental state.** Each new test's assertions must fail today (before the fix) and pass only once *this specific* change is made, and must target the fixed behavior narrowly — assert the exact grid/highlight/cursor/keycode value that changes, not a broad snapshot or unrelated surrounding state — via a **hard `QVERIFY`/`QCOMPARE`**, never a `qWarning` or soft pixel-band heuristic (which respond to kerning/sub-pixel changes and produce false positives).
     - The exact command to run from `desktop/` for validation, e.g. `ctest --preset release -R 'attach_and_render|resize|insert_and_quit'` for paint-path/redraw changes, `ctest --preset release -R test_input_handler` for `InputHandler`, `ctest --preset release -R test_qml` for QML. Note that the build must pass under `/W4 /permissive-` with warnings treated as bugs.
     - **Visual gate.** For paint-path / font / cursor / selection / highlight changes, require capturing the rendered window via `desktop/scripts/screenshot_dot.ps1` (or the `visual-validate-dot` skill) and inspecting it — a green ctest run alone is not proof.

   Write each fact once, with no filler, history, or narrative. Hard-wrap prose at 120 characters; leave tables and URLs
   unwrapped. Prefer tables for comparisons (Today / After) and `- [ ]` checklists for acceptance and validation. Tag
   every code fence with a language; PowerShell fences use `ps1`.

   Add a Mermaid diagram only when it makes the change clearer than prose (data flow, before/after structure, state).
   Use `flowchart LR`; explicit `classDef` with fill, stroke, and color; label lines of at most 24 characters, split
   with `<br/>`; no `#` or `{}` inside labels; short subgraph titles; pipe edge labels.
3. Create the git-ignored `tmp\` directory if needed. Write each title/body draft to its own `tmp\triage_issue_<guid>.md` for user editing.
   Keep the path for each issue; use that draft's path wherever `<draft-path>` appears below.
4. **Validate every draft with the rubber-duck agent — always, no exceptions.** Before presenting the plan, launch the **rubber-duck** agent (via the Task tool) to review all draft paths. The prompt must include:
   - Every full drafted issue (or its path).
   - The subagent's investigation report (`surfaces`, `affected`, `searchStrategy`, `codeSnippet`, `rootCause`, `approach`, `testing`) so it can check the draft against the actual findings.
   - All parent/child drafts, `deliverables`, `mergeProposals`, `sourceComplexity`, `slices`, and recorded merge
     approvals. Apply **Source complexity and decomposition**: reject missing usable outcomes, named consumers/uses or
     demos, orphaned integration, leaves useful only after later integration, coverage gaps/overlap, testing-derived
     sizing, unbounded leaves, and high-complexity leaves without specific merge approval. Check approved merged leaves
     against their recorded scope, actual rating, and rationale; also reject missing native relationship/label plans
     and false dependencies. Require each draft's complexity label to match its source rating.
   - The instruction to catch: incorrect or unverified root cause, `file:line` references that don't match reality, **a frozen file/line inventory or occurrence count where a discovery recipe belongs** (features/refactors/renames must anchor to patterns and stable identifiers, not coordinates that will go stale), an approach that violates a dot invariant (paint-path purity, per-frame redraw cost, no `msgpack::object` copies, render-thread affinity, well-bracketed nvim keycodes, 4-element `hl_attr_define`), acceptance criteria that aren't concretely verifiable, and testing requirements that use soft assertions (`qWarning`/pixel-band) instead of hard `QVERIFY`/`QCOMPARE`, target the wrong test tier, or fail to pin *this specific* fix.
   - The instruction to report only substantive issues (bugs, logic/design flaws, missing coverage) — not style/wording nits.

   Resolve substantive findings, reinvestigating factual gaps as needed, before presenting the plan.
5. Show every proposed **title**, **categories/labels**, **source rating**, parent/child link, dependency, reused issue, and leaf outcome/demo. Highlight approved merged scopes and actual ratings; note the passed rubber-duck review.
   Open each draft with mdview in the next step.
6. **Always open the rendered draft with mdview before asking the user to accept.** Launch the viewer on the draft so the user reviews the formatted markdown, not the raw file:

   ```ps1
   D:\mdview\target\release\mdview.exe <draft-path>
   ```

7. **Ask the user to accept, edit, or reject** the plan with `ask_user`. For edits, wait for confirmation and re-read changed drafts/categories. **Reopen changed drafts with mdview** before asking again; repeat until acceptance or rejection.
   Acceptance must explicitly cover the **whole plan** before any creation. Re-review and obtain renewed acceptance for
   scope changes, including changes discovered after acceptance.

### 6. Create the GitHub issue — only if accepted

**Only run this step if the user accepted in step 5.** If rejected, delete the draft files and stop.

Use the bundled helper script for each new issue. It ensures labels exist, extracts the title/body, assigns `@me`, and
prints the title and URL. Pass the accepted source rating with `-Complexity` and category labels as an array:

```ps1
& .\.github\skills\triage\scripts\new_issue.ps1 `
  -Complexity <accepted-rating> -Draft <draft-path> -Label @("<cat1>", "<cat2>")
```

Include `spike` when applicable. Create tracking parents before their new descendants; for **every child**, add
`-Parent <parent-number>`. The helper adds the matching complexity label and, for children, `sub-issue`.
It splits and trims comma-separated label entries before validation. Conflicts fail before remote calls;
same-rating duplicates collapse to the canonical label.
It creates native links using `gh issue create --parent`, for existing parents and nested children alike.
For accepted reuse of an existing issue, ensure the label exists and apply
`gh issue edit <child-number> --add-label sub-issue`. Read its native parent first
(`gh api repos/<owner>/<repo>/issues/<child-number>/parent`); if it has none, attach it with the sub-issue call in
**Relations**. Preserve existing parents; reuse work elsewhere through a real dependency, rather than duplicating or
reparenting it.

For accepted reuse/backfill, read current labels and ensure the accepted complexity label exists with `gh label create`
only if missing. Add it with `gh issue edit <number> --add-label "<complexity-label>"` if absent.
Remove only conflicting complexity labels with `--remove-label`. Preserve all other labels and relationships;
never replace the full label list.

Replace provisional references with the created issue numbers. For each accepted dependency, including order between
children, add the blocked-by link in **Relations**.
If creation/linking fails, stop and inspect the resulting issue/relationship before retrying;
never blindly rerun creation after a partial success.

Read back labels with `gh issue view <number> --json labels`: require exactly one complexity label matching the
accepted rating, alongside planned categories and, for children, `sub-issue`. Verify each child's native parent with
`gh api repos/<owner>/<repo>/issues/<child-number>/parent` and accepted dependencies with the blocked-by list in
**Relations**. Apply the same readback to reused/backfilled issues.
Confirm the accepted hierarchy, coverage and dependencies; report all issue URLs and unresolved failures.

### 7. Cleanup

1. Delete the draft files and any leftover body file.
2. Show a short summary of all issue URLs, titles, assigned categories/labels, and verified relationships.

## Rewriting an existing issue

Rewrite the body into the step 5 shape for the issue's type; when the title has no prefix, infer the type from its
labels and content and keep the title unchanged. Never lose information: every fact, decision, constraint, acceptance
item, code block, path, command, and number must survive. Dropping relations other than parent/sub-issue and
blocked-by is user-approved and is not a loss.

1. Write the candidate body (no title line) to its own `tmp\triage_rewrite_<guid>.md`.
2. Before publishing, launch a fresh agent with no prior context as an independent judge. Give it the original body, the
   candidate, and the approved relation drops, and have it report every substantive fact, constraint, decision, or
   criterion that is lost or changed. On any finding, revise the candidate and judge again; never publish over a
   finding.
3. After a clean judgment, open the candidate with mdview and get the user's acceptance.
4. Make sure every parent or blocked-by relation in the original text has its native link, creating missing ones (see
   **Relations**). Then publish with `gh issue edit <number> --body-file <candidate-path>` and delete the candidate.
