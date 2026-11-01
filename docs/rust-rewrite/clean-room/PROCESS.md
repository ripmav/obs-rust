# Clean-room process

Purpose: make it possible for any engineer **or AI agent** to implement a component of
OBS Studio in Rust from a written specification and an executable conformance suite,
**without reading the C implementation**, and for a reviewer to verify that this was the
case.

> This document is an engineering process, not legal advice. OBS Studio is licensed
> GPL-2.0-or-later and the Rust rewrite is planned to be distributed under
> GPL-compatible terms, so a clean-room process is **not required for licence
> compliance**. It is used to (a) keep provenance auditable, (b) keep the
> specification the single source of truth so that independent implementations agree,
> and (c) keep options open if relicensing is ever considered. Ask counsel before
> relying on it for (c).

## 1. Roles

| Role | May read | May not read | Produces |
|---|---|---|---|
| **Spec author** | everything, including the C implementation | – | specs (`docs/rust-rewrite/specs/S-*.md`), scripts and vectors (`test/conformance/`), reference runners (`test/conformance/gen/`) |
| **Implementer** (human or AI) | the spec of the component, the conformance README, scripts and vectors, the *published* API/ABI description in the spec, documentation of external standards and third-party libraries | any `.c`, `.cpp`, `.m`, `.mm`, `.swift` implementation file of the component or its callers in this repository, `git log -p`/`git blame` of those files, decompilations; the reference runners in `test/conformance/gen/` (they link the C implementation) | Rust crate, own conformance runner, `DEVIATIONS.md` |
| **Reviewer** | spec + implementation + provenance log | – | provenance sign-off (section 5) |

Public C headers: an implementer needs struct layouts, constants and function signatures
(the ABI). The spec reproduces everything required in its "Interface" sections, so
implementers **should not need to open the headers**. If they do (e.g. to double-check a
constant), they may read **declarations only** and must record it in the provenance log;
`static inline` function bodies in headers are implementation and are off-limits.

One person may play both roles for *different components*, never for the same one.

## 2. Allowed and forbidden paths (for tooling and AI agents)

Allowed for an implementer:

```
docs/rust-rewrite/specs/**            test/conformance/README.md
test/conformance/scripts/**           test/conformance/vectors/**
docs/rust-rewrite/clean-room/**       crates/**  (own work)
```

Forbidden for an implementer (enforce with sandbox/permissions where possible):

```
libobs/** libobs-*/** plugins/** frontend/** shared/** deps/**
test/cmocka/** test/test-input/** test/conformance/gen/**   (reference runner sources: harmless
                                                              but excluded to keep the rule simple)
```

See [AGENT-BRIEF.md](AGENT-BRIEF.md) for a ready-to-use instruction block for AI agents.

## 3. Workflow

1. **Spec author** writes/updates a spec from [SPEC-TEMPLATE.md](SPEC-TEMPLATE.md) in their own words:
   behaviours as rules, algorithms as original pseudocode, binary formats as tables,
   *no copied code*. They write scripts for every rule and generate vectors with
   `test/conformance/run.sh generate`. Every rule in the spec carries an ID (`S-002-R14`)
   and each rule is referenced by at least one script (coverage table at the end of the spec).
2. Spec status moves `Draft → Ready` only when a **second spec author** has checked the
   spec against the source and the vectors (see section 4).
3. **Implementer** picks a `Ready` spec, implements the crate, writes a runner for the
   script language, and runs all scripts. Output is compared per
   `test/conformance/README.md`.
4. **Questions** go to the spec author as a GitHub issue labelled `spec-question`.
   The answer is **always a change to the spec and, where possible, a new script**, never a
   private explanation. Implementers never receive pointers to source code.
5. **Deviations** (QUIRK cases deliberately not reproduced, or implementation-defined
   points) are listed in the crate's `DEVIATIONS.md` with a reason and a test.
6. **Review**: the reviewer checks the provenance log and runs the similarity scan (section 5).

## 4. Spec quality bar ("Ready" checklist)

A spec is Ready only if all are true:

- [ ] Every public symbol of the component is listed with signature, preconditions, effect, return value, error behaviour, ownership, thread-safety.
- [ ] Every binary or textual format is specified byte-by-byte (with a worked example whose bytes are in a vector).
- [ ] Every algorithm is specified precisely enough to produce the same observable result (including growth policies, ordering, tie-breaks, rounding) — or the spec states the point is *implementation-defined* and why it cannot be observed by callers.
- [ ] Every error path is listed, including what is logged (level) and whether the process terminates.
- [ ] Reference defects (crashes, UB, quirks) are classified DEFECT/QUIRK with the required behaviour.
- [ ] Locks, ordering and re-entrancy rules are stated for every function that can be called from callbacks.
- [ ] Every rule has at least one script; every script maps to rules (coverage table).
- [ ] A second author re-derived 10 random rules from the source and found no discrepancy.
- [ ] Vectors regenerate bit-identically twice (`run.sh generate` then `check`).

## 5. Provenance and review

Each implementation PR contains a **provenance log** (PR description or `PROVENANCE.md` in the crate):

```
Component:           S-002 calldata
Implementer:         <name or agent id and model>
Documents used:      specs/S-002-calldata.md @ <commit>, test/conformance/README.md
Vectors used:        scripts/calldata/*, vectors/calldata/*
Headers read:        none | list (declarations only)
Source files read:   none
Questions filed:     #123, #124
Deviations:          DEVIATIONS.md (n entries)
```

The reviewer then:

1. Confirms no forbidden path was read (agent transcripts/tool logs for AI work; attestation for humans).
2. Runs a similarity scan between the new crate and the C component (token-level, e.g. a
   winnowing/`moss`-style tool); any match above the agreed threshold is investigated.
3. Re-runs all vectors on all supported OSes.
4. Signs off in the PR.

## 6. What if the spec is wrong or incomplete?

- A vector fails and the spec is silent: **spec bug**. File `spec-question`; do not guess by
  looking at C.
- A vector fails and the implementer is sure the implementation follows the spec: the spec
  or the vector is wrong. Spec author re-runs the reference and fixes whichever is off.
- The reference behaves in a way the spec author considers a mistake: classify it QUIRK or
  DEFECT, state the intended behaviour, and add a script. The default is to **match the
  reference** for anything a plugin or saved file could depend on.

## 7. Versioning

Specs and vectors are versioned with the repository. Changing a vector is a spec change
and needs spec-author review. A crate states which spec commit it conforms to.
