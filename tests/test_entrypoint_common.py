"""Guard the slim-printing gate's required runtime against an unstage regression.

The shared slim-printing gate (projectbluefin/fsdk-containers#341) requires
``usr/libexec/printer-app/entrypoint-common.sh`` to be present in the image
before it will produce an OCI artifact. That file is installed by the
``printing/entrypoint-common.bst`` element from the shared fsdk-containers
junction. If a consumer drops that element from its runtime stack the gate
fails on the next build and no image is published, exactly what happened to
this app in #214.

This check runs the gate's precondition without building the image: it walks
the BuildStream dependency graph from ``oci/hplip-printer-app.bst`` and fails
unless the ``entrypoint-common.bst`` element is reachable. It fails with the
current build inputs (the dependency absent) and passes once the element is
staged again, so an accidental removal is caught on the next ``just validate``
rather than in a failed publication.

It does not follow the fsdk-containers junction into the external repo; the
junction source is version-pinned and its own element is its own contract.
This repo's responsibility is only that it stages the element.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
ELEMENTS = ROOT / "elements"
OCI_ELEMENT = ELEMENTS / "oci" / "hplip-printer-app.bst"

# The path the slim-printing gate requires and the fsdk-containers element
# that installs it. Kept together so a gate change and this guard move as one.
REQUIRED_PATH = "usr/libexec/printer-app/entrypoint-common.sh"
STAGED_ELEMENT = "printing/entrypoint-common.bst"


def _dep_refs(element):
    """Yield element references from an element's depends and build-depends.

    BuildStream folds both ``depends`` and ``build-depends`` into the artifact
    graph that ``bst show --deps all`` reports, so a stack's ``depends`` (which
    is how the shared elements are listed) must be walked as well.
    """
    for key in ("build-depends", "depends"):
        value = element.get(key)
        if not isinstance(value, list):
            continue
        for item in value:
            if isinstance(item, str):
                yield item
            elif isinstance(item, dict):
                filename = item.get("filename")
                if isinstance(filename, str):
                    yield filename


def _classify(ref):
    """Classify a dependency reference.

    Returns ``("local", "printer-app/runtime.bst")`` for an element in this
    repo, ``("junction", "printing/entrypoint-common.bst")`` for a shared
    fsdk-containers element, or ``None`` for a YAML include or variable that is
    not an element.
    """
    if ref.startswith("filename:"):
        return "local", ref.split(":", 1)[1].strip()
    if ref.startswith("fsdk-containers.bst:"):
        return "junction", ref.split(":", 1)[1]
    # A bare string is a local element relative to elements/ (BuildStream
    # resolves it from the project root), e.g. "printer-app/runtime-stack.bst".
    if ref.endswith(".bst"):
        return "local", ref
    return None


def reachable_elements(oci_element=OCI_ELEMENT):
    """Every fsdk-containers element reachable from ``oci_element``.

    Local ``.bst`` elements are followed through the dependency graph;
    fsdk-containers junction references are recorded by their
    fsdk-containers-relative name and not followed into the external repo.
    ``filename:`` references resolve against the ``elements/`` root, as
    BuildStream does.
    """
    elements_root = oci_element.parent.parent
    junctions = set()
    visited = set()
    stack = [oci_element]
    while stack:
        path = stack.pop()
        try:
            element = yaml.safe_load(path.read_text())
        except (OSError, yaml.YAMLError):
            continue
        if not isinstance(element, dict):
            continue
        for ref in _dep_refs(element):
            classified = _classify(ref)
            if classified is None:
                continue
            kind, target = classified
            if kind == "local":
                if not target.endswith(".bst"):
                    continue
                resolved = elements_root / target
                key = str(resolved.resolve())
                if key in visited:
                    continue
                visited.add(key)
                stack.append(resolved)
            else:  # junction
                junctions.add(target)
    return junctions


def _write_element(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class EntrypointCommonStagingTests(unittest.TestCase):
    def test_entrypoint_common_is_staged(self):
        junctions = reachable_elements()
        self.assertIn(
            STAGED_ELEMENT,
            junctions,
            (
                f"slim-printing gate requires {REQUIRED_PATH}, but the "
                f"{STAGED_ELEMENT} element is not in the OCI element's "
                "dependency graph. Stage it in elements/printer-app/"
                "runtime-stack.bst (as ghostscript-printer-app does) so the "
                "gate passes and the image publishes."
            ),
        )

    def test_graph_walker_passes_when_staged(self):
        # A fixture that lists the element must pass, so the guard is really
        # testing reachability and not always green.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "elements"
            _write_element(root / "oci" / "app.bst",
                           "kind: script\nbuild-depends:\n"
                           "  - filename: printer-app/runtime.bst\n"
                           "  - fsdk-containers.bst:include/slim-printing.yml\n"
                           "  - app-forbidden-paths: |\n"
                           "      usr/bin/cupsd\n")
            _write_element(root / "printer-app" / "runtime.bst",
                           "kind: compose\nbuild-depends:\n"
                           "  - printer-app/core-stack.bst\n")
            _write_element(root / "printer-app" / "core-stack.bst",
                           "kind: stack\ndepends:\n"
                           "  - fsdk-containers.bst:printing/base.bst\n"
                           "  - fsdk-containers.bst:printing/entrypoint-common.bst\n")
            self.assertIn(STAGED_ELEMENT, reachable_elements(root / "oci" / "app.bst"))

    def test_graph_walker_fails_when_unstaged(self):
        # A fixture that omits the element must fail, mirroring the #214 state.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "elements"
            _write_element(root / "oci" / "app.bst",
                           "kind: script\nbuild-depends:\n"
                           "  - fsdk-containers.bst:printing/base.bst\n")
            self.assertNotIn(STAGED_ELEMENT, reachable_elements(root / "oci" / "app.bst"))


if __name__ == "__main__":
    unittest.main()
