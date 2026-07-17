# Iridium Engine Execution Plans

## Purpose

Each roadmap milestone must have an execution plan under `docs/milestones/` before implementation begins. The plan is the durable contract between the milestone lead, implementers, reviewers, and future tasks. It should be detailed enough that a fresh Codex task can resume the work by reading the repository rather than relying on chat history.

Plans are living documents. Update them when evidence changes the design, scope, ordering, risks, or acceptance criteria.

## Required plan structure

### Header

Record the milestone, status, owner or lead task, relevant ADRs, dependencies, and last-updated date.

### Objective and user-visible outcome

State what will be measurably better when the milestone is accepted. Prefer observable behavior over implementation activity.

### Current context

Summarize the relevant implementation, known defects, measurements, and constraints. Link to source files and captures when available. Reinspect the source before relying on this section.

### Invariants

List contracts that must remain true, such as RHI boundaries, linear-HDR composition, serialization compatibility, stable identity, or fallback behavior.

### Scope and non-goals

Define what is included and deliberately excluded. Deferred work must point to a roadmap milestone or issue rather than becoming an untracked promise.

### Design and data flow

Describe interfaces, ownership, lifetime, synchronization, memory layout, failure behavior, and migration. Include a diagram only when it makes a dependency or state transition materially clearer.

### Vertical slices

Break work into independently buildable and verifiable slices. Each slice records:

- preconditions and files or systems likely affected;
- exact behavior and interfaces to add or change;
- tests, captures, and performance measurements;
- rollback or fallback behavior;
- completion criteria.

Prefer slices that prove an end-to-end path over large horizontal rewrites. Keep the engine buildable after every merged slice.

### Delegation and integration

The milestone lead owns architectural coherence, interface decisions, integration, and final acceptance. Subagents may inspect or implement bounded, disjoint slices. The lead must review their work against the plan and current source before accepting it.

Avoid concurrent write-heavy changes to shared renderer headers, central serializers, build files, or shader interfaces. Use read-only investigations in parallel when boundaries are not yet stable.

### Verification

Specify build configurations, automated tests, Vulkan validation, scenes, captures, image thresholds, CPU/GPU timings, and memory counters. Record the hardware, resolution, output mode, and relevant quality settings for performance results.

### Risks, fallback, and rollback

Identify technical risks and define how the engine remains usable if an optional capability is unavailable or a new path underperforms.

### Decision log

Record material decisions made during implementation, including evidence and links. Promote decisions with lasting architectural consequences to ADRs.

### Completion report

When accepted, record changed behavior, verification results, before/after measurements, outstanding limitations, and roadmap/ADR updates.

## Slice task prompt template

Use this template when delegating a bounded slice:

```text
You are implementing slice <ID and name> for Iridium Engine milestone <milestone>.

Before acting, read AGENTS.md, ROADMAP.md, PLANS.md, docs/PROJECT_CONTEXT.md,
the milestone plan, and its referenced ADRs. Reinspect the current source and git
status; the repository may contain unrelated uncommitted work. Repository files are
authoritative over assumptions in this prompt.

Scope:
- <specific owned behavior/files>

Non-goals:
- <explicit exclusions>

Required contracts:
- <interfaces/invariants>

Verification:
- <builds/tests/captures/measurements>

Do not expand the architecture silently. Report conflicts to the milestone lead.
Return a concise implementation summary, verification evidence, performance or
memory deltas, and remaining risks.
```

## Starting and resuming a milestone

1. Read the durable context and inspect the current worktree.
2. Audit the proposed plan against current source and measurements.
3. Resolve or explicitly log open architectural choices.
4. Mark one slice `In Progress`; do not start all slices at once.
5. Implement, verify, review, and record evidence.
6. Integrate and update the plan before starting the next dependent slice.
7. Run the milestone acceptance gate and write the completion report.
