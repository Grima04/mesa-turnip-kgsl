#!/bin/sh

python3 ./convert-sphinx.py &&
git add "docs/**.rst" && git rm "docs/**.html" &&
git commit -am "docs: convert articles to reructuredtext

This uses the previously added scripts to convert the documentation to
reStructuredText, which is both easier to read offline, and can be used
to generate modern HTML for online documentation.

No modification to the generated results have been done.

Acked-by: Eric Engestrom <eric@engestrom.ch>"
