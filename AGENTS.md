# Notes for contributors and coding agents

## Project
stepwright is a header-only C++20 library for writing asynchronous operations as
explicit state machines: each step is a member function that returns the next
transition (`done`, `fail`, `then`, `after`, `forward`, `park`, `defer`).
See `docs/design.md` (engine, threading, cancellation) and `docs/guide.md`.

## Build and test
```bash
make test          # Release build + ctest
make debug         # Debug build into build/debug
make asan          # ASan + UBSan
make tsan          # ThreadSanitizer
make benchmarks    # Release benchmarks with repetitions
make format        # clang-format in place
```
On macOS, CMake may need `export PATH="/opt/homebrew/bin:$PATH"`.

## Independent-implementation rules (mandatory)
- Implement from the specifications and public documentation only. Do not read,
  copy, paraphrase or consult any proprietary or third-party codebase whose
  license does not permit it, and do not reuse code from earlier private
  prototypes of this project.
- Public prior art (papers, standard proposals, open-source projects with
  compatible licenses) may inform designs; record it in `docs/provenance.md`.
- Every change that adds a component appends an entry to `docs/provenance.md`.

## Engineering rules
- Never start a child task, fire a completion, or invoke user callbacks while
  holding an internal lock.
- After invoking a completion, never touch the completing object again.
- No heap allocation on the per-step or per-wait hot paths.
- Class layout must not depend on `NDEBUG`; debug-only checks use
  `STEPWRIGHT_DEBUG` and keep members unconditionally.
- Every bug fix lands with a regression test.
