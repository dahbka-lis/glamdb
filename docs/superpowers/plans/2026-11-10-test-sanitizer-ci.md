# Test and sanitizer CI implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement these tasks in order.

**Goal:** Run repository tests automatically, with independent native sanitizer jobs and actionable failure logs.

**Architecture:** GitHub Actions matrix jobs run ordinary CTest plus ASan, UBSan, MSan, TSan and LSan independently (`fail-fast: false`). Python, Node and browser suites use their native discovery. Native dependency prefixes are isolated per profile; MSan/TSan also build a pinned, instrumented libc++ runtime.

**Tech Stack:** GitHub Actions, Clang/LLVM 18, CMake/CTest, pytest, Node, Playwright.

**Spec:** User request in this chat: update main from official/main; create a new fork branch and PR; run all repository tests and independent sanitizer checks; preserve failure logs; avoid runs for changes that cannot affect tests.

## Global constraints

- Use the existing checkout and branch `ci/test-sanitizer-checks`, based on updated `official/main`.
- Keep Actions SHA-pinned and PR permissions read-only; do not change repository security settings.
- Use native GitHub step logs, verbose failing CTest output, and sanitizer stack traces. Log pipelines must preserve failure status.
- Exclude documentation-only changes conservatively; new code directories must trigger tests automatically.
- Do not run upstream test suites. Rebuild native dependencies when instrumentation requires it.
- Preserve existing local build interfaces and ordinary developer presets.

## Review focus

- A new test file or GoogleTest case must be discovered without editing CI paths.
- A failed test must make its job fail and leave diagnostic output.
- A sanitizer failure must not cancel other profiles or be suppressed.
- MSan must not accidentally link the uninstrumented C++ standard library or dependencies.
- Compatibility probes that intentionally mix instrumentation must use their intended baseline.

### Task 1: Test discovery

- [x] Update CMake source discovery for test suites and tooling discovery, preserving integration drivers.
- [x] Broaden Python discovery to project roots while excluding vendor/generated/native-driver tests.
- [x] Use recursive Node unit-test discovery and keep Playwright discovery.
- [x] Verify a newly added temporary nested test is collected; run existing tooling tests.

### Task 2: Native profiles and logs

- [x] Add a CI-only profile toolchain/runner for normal, address, undefined, memory, thread, and leak modes.
- [ ] Build pinned libc++/libc++abi for memory/thread; propagate instrumentation to the dependency tree.
- [ ] Run all CTest suites and discover standalone probe projects, retaining intended mixed-annotation probe behavior.
- [ ] Check invalid profiles, configuration flags, sanitizer diagnostics, failure exit codes, and ordinary CTest.

### Task 3: Workflows and publication

- [x] Add independent native matrix jobs and independently runnable web/Python suites, with conservative path filters.
- [x] Keep logs in Actions console even when steps fail; document local reproduction and CI scope.
- [ ] Run actionlint, zizmor, appropriate tests and independent code review.
- [ ] Commit, push to fork, open template-based English PR to official/main, and inspect remote jobs.
