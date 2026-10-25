# Security policy

## Supported versions

Security fixes target the latest published 0.1.x release. Before the first
release, reports against the development branch are welcome. Older minor series
are not maintained; upgrading may require source changes while the major version
is zero. Check [CHANGELOG.md](CHANGELOG.md) for fixes.

## Report privately

**Do not open a public issue or pull request containing an exploitable defect.**
Use GitHub's [Report a vulnerability](https://github.com/stepwright/stepwright/security/advisories/new)
form (repository → Security → Advisories). Include:

- A minimal reproducer, affected version/commit, OS/architecture, compiler and flags.
- The lifetime/threading preconditions your program satisfies, observed behavior,
  sanitizer output and the possible impact (for example use-after-free or data race).
- Whether you have a proposed fix or disclosure timeline. Do not include secrets
  or unrelated private application data.

Maintainers will acknowledge and investigate privately, coordinate a fix and
credit, and publish an advisory when a fix or mitigation is ready. This is a
volunteer project with no guaranteed response SLA. If private reporting is not
available, ask the maintainer for a private channel via their GitHub profile
without disclosing the defect publicly.

Precondition violations (destroying active tasks, duplicate starts, stale wakers,
mixed tracking layouts) are unsupported use, but reports of unclear contracts or
missing diagnostics are still useful. Never dismiss a potential library defect
solely because it is intermittent.
