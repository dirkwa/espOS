# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""
MkDocs hook: render Markdown files from the repository root as site pages.

CHANGELOG.md lives at the root, where GitHub, Keep-a-Changelog tooling and
scripts/release.sh expect it, and the site wants it at /changelog/. A copy
under docs/ would be two files to keep in step; a symlink or a snippet
include would carry the file's links over as written, and CHANGELOG.md's
`docs/releasing.md`, resolved from docs/changelog.md, names
docs/docs/releasing.md, which `mkdocs build --strict` rejects. So the page
in docs/ is a stub and this hook substitutes the root file's text at build
time, rewriting its relative links: one that points into docs/ becomes a
link to that page, anything else becomes a link to the file on GitHub.

Registered in mkdocs.yml (`hooks:`); PAGES maps a page in docs/ to the root
file it shows. Standard library only.
"""

import posixpath
import re
from pathlib import Path

PAGES = {
    "changelog.md": "CHANGELOG.md",
}

# `](target)` of an inline link; reference-style links are not used at the root.
_LINK = re.compile(r"\]\(([^)\s]+)\)")


def _rewrite(text: str, root_file: str, page_uri: str, repo_url: str) -> str:
    def repl(m: re.Match) -> str:
        target = m.group(1)
        if target.startswith(("http://", "https://", "mailto:", "#")):
            return m.group(0)
        path, _, fragment = target.partition("#")
        fragment = f"#{fragment}" if fragment else ""
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(root_file), path))
        if resolved.startswith("docs/") and resolved.endswith(".md"):
            page_dir = posixpath.dirname(page_uri) or "."
            return f"]({posixpath.relpath(resolved[len('docs/'):], page_dir)}{fragment})"
        return f"]({repo_url.rstrip('/')}/blob/main/{resolved}{fragment})"

    return _LINK.sub(repl, text)


def on_page_markdown(markdown: str, page, config, files) -> str:
    root_file = PAGES.get(page.file.src_uri)
    if root_file is None:
        return markdown
    root = Path(config["config_file_path"]).resolve().parent / root_file
    return _rewrite(root.read_text(encoding="utf-8"), root_file, page.file.src_uri, config["repo_url"])
