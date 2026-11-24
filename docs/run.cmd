@echo off
rem Serve the docs (default) or build them: run [serve|build]
setlocal
cd /d "%~dp0"
python -c "import material, mkdocs, minify_plugin" 2>nul || python -m pip install -r requirements.txt || exit /b 1
if /i "%1"=="build" (
    python -m mkdocs build --clean
) else (
    python -m mkdocs serve --open
)
