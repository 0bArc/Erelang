$ErrorActionPreference = 'Stop'
$root = 'D:\Development\Game\Physics'
$exclude = '\\(build|\.git|node_modules|\.vs|out|bin|obj)\\'
$files = Get-ChildItem -Path $root -Recurse -File | Where-Object { $_.FullName -notmatch $exclude }

$replacements = @(
  @('@ERELANG','@ERELANG'),
  @('@erelang','@erelang'),
  @('@erelang','@erelang'),
  @('ERELANG','ERELANG'),
  @('Erelang','Erelang'),
  @('erelang','erelang'),
  @('ERELANG','ERELANG'),
  @('erelang','erelang'),
  @('erelang','erelang'),
  @('ERELANG','ERELANG'),
  @('Erelang','Erelang'),
  @('erelang','erelang'),
  @('ERELANG_','ERELANG_'),
  @('erelang_','erelang_')
)

$changed = 0
foreach ($f in $files) {
  $path = $f.FullName
  try { $text = [System.IO.File]::ReadAllText($path) } catch { continue }
  $orig = $text

  foreach ($pair in $replacements) {
    $text = $text.Replace($pair[0], $pair[1])
  }

  $text = [regex]::Replace($text, '\bOBS\b', 'ERELANG')
  $text = [regex]::Replace($text, '\bobs\b', 'erelang')

  if ($text -ne $orig) {
    [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
    $changed++
  }
}

Write-Host ("Changed files: $changed")
