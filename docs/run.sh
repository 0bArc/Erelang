#!/usr/bin/env sh
# Serve the docs (default) or build them: ./run.sh [serve|build]
cd "$(dirname "$0")" || exit 1
python3 -c "import material, mkdocs, minify_plugin" 2>/dev/null || python3 -m pip install -r requirements.txt || exit 1
if [ "$1" = "build" ]; then
    python3 -m mkdocs build --clean
else
    python3 -m mkdocs serve --open
fi
