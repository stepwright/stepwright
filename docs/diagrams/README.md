# Diagrams

The diagrams in the README, the overview and the guide come in two kinds.

**Interactive architecture diagrams** are built with
[Archify](https://github.com/tt-a1i/archify) from the JSON sources in this
directory. Each source pins a commit of this repository, and every component,
state or participant links to the lines of code it describes. The published
images in [`docs/assets/diagrams/`](../assets/diagrams/) are the viewer's
dual-theme SVG exports, so they follow GitHub's light or dark theme.

| Source | Image | Shows |
|---|---|---|
| [`runtime.architecture.json`](runtime.architecture.json) | [`runtime.svg`](../assets/diagrams/runtime.svg) | How an operation runs: your code, the machine, the driver loop, children, executors, timers and cancellation |
| [`machine-lifecycle.lifecycle.json`](machine-lifecycle.lifecycle.json) | [`machine-lifecycle.svg`](../assets/diagrams/machine-lifecycle.svg) | The states of a machine and the transitions between them |

**Smaller diagrams** in the README and the overview are Mermaid blocks written
directly in the Markdown. GitHub renders them; edit them as text. They share one
colour palette: `client` (blue), `server` (green), `storage` (amber), `network`
(violet), `policy` (rose), `accent` (navy), `success` and `danger`.

## Regenerating an Archify diagram

The sources cite repository lines at a pinned commit. After changing the code a
diagram describes, update its `meta.repository.revision` and source line ranges,
then check and render it against a clean clone of that commit:

```bash
git clone https://github.com/stepwright/stepwright.git /tmp/stepwright-pinned
git -C /tmp/stepwright-pinned checkout <revision from the JSON>
node <archify>/bin/archify.mjs finalize architecture docs/diagrams/runtime.architecture.json \
  .archify/diagrams/runtime.html --repo-root /tmp/stepwright-pinned --quality showcase
```

Use the matching diagram type (`architecture` or `lifecycle`). `finalize`
validates the source and its evidence, renders the HTML and checks it in a real
browser. Open the HTML, choose **Export → SVG · Auto**,
and save the result over the image in `docs/assets/diagrams/`. The `.archify/`
working directory is not tracked.
