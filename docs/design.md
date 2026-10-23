# Recording design rationale

Use this guide to explain why a native library's contracts and architecture
take their current form. It is for maintainers and reviewers familiar with the
project's goals, public interfaces, and contributor rules. The template has no
product design yet. These instructions establish a documentation workflow;
they do not select a product architecture or invent compatibility guarantees.
Replace this guide with the project's actual rationale as decisions are made.

## Record the first decision

Choose one consequential API or architecture decision. State the problem,
constraints, plausible alternatives, selected approach, and accepted costs.
Link the affected contract and the tests that demonstrate its observable
effects. This short record is enough to start; extend it when new evidence
changes the decision or exposes an unanswered question.

## Establish goals and constraints

Describe the intended callers and workflows, and how success will be judged.
Identify requirements such as supported platforms, resource limits,
interoperability, or response times. Distinguish requirements from preferences
and assumptions that still need validation.

Connect these constraints to the existing contributor rules. When a rule
meaningfully shapes the design, explain its effect on the chosen approach.
Do not silently introduce exceptions to repository requirements.

## Compare realistic alternatives

Describe the alternatives that could satisfy the same caller need. Compare
their relevant costs and benefits: complexity, resource use, failure behavior,
portability, compatibility, and maintenance effort. Include the option of
deferring a capability when the evidence does not yet justify it.

Explain why the selected approach meets the current goals. Record its costs,
limitations, and the conditions that would make another approach preferable.
Avoid presenting an alternative as unsuitable merely because it was not
chosen.

## Preserve evidence

Link research to the specific claim it supports. Prefer upstream
documentation, standards, and reproducible measurements. Record versions,
environment, and methodology when they affect a result. Clearly identify
assumptions and conclusions inferred from evidence.

Keep historical experiments separate from current requirements. Temporary
agent notes are not authoritative project documentation. Bring the relevant
evidence into the maintained rationale when it is needed to explain a decision.

## Connect rationale to contracts and tests

Use the [API contract](api.md) to state what callers may rely on. Use rationale
to explain why those promises exist. Link between the two documents instead
of maintaining competing descriptions of the same behavior.

Identify tests for the observable consequences of a decision. Internal
refactoring should preserve those expectations unless the intended contract
changes. Record compatibility or migration consequences when changing a
previously released decision.

## Review and revise

Before review, check that a reader can identify the decision, its supporting
evidence, the accepted tradeoffs, and any unresolved limitations. Verify
referenced files and sources, and check that the contract and tests describe
the same intended behavior.

If the rationale cannot explain the selected approach, revisit the goals or
comparison of alternatives. If evidence is missing, mark the assumption and
identify what would validate it. Revise the rationale when new evidence or
requirements change the decision, preserving useful context for reviewers.
