# Erelang conformance suite

Run from repo root with Debug runner:

```powershell
.\tests\run.ps1
```

Or manually:

```powershell
$exe = "build\bin\Debug\erelang.exe"
Get-ChildItem tests -Recurse -Filter *.elan | ForEach-Object {
  & $exe $_.FullName
  Write-Host "$($_.FullName) => $LASTEXITCODE"
}
```

## Conventions

- Positive `*.elan`: must print a stable line and exit `0`.
- Negative `*_neg.elan`: must exit non-zero. Optional first-line marker `// expect: TC123` or `// expect: Expected )` — runner checks stderr contains that substring.
- Skip `cycle_b.elan` as a standalone entry (included by `cycle_a.elan` only).

## Runner

`tests/run.ps1` walks every `*.elan` under `tests/`, treats `*_neg.elan` as pass-on-nonzero, prints pass/fail counts.
