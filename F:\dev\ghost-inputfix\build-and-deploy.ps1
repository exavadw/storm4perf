$ErrorActionPreference = "Stop"
$log = "F:\dev\ghost-inputfix\build-deploy.log"
"=== build-and-deploy start $(Get-Date -Format o) ===" | Tee-Object $log

# 1. Fix storm4perf.ini: move inputfix=1 into [fixes] section
$iniPath = "F:\dev\ghost-inputfix\game\storm4perf.ini"
$lines = [System.Collections.Generic.List[string]]::new()
foreach ($l in @(Get-Content $iniPath)) { $lines.Add([string]$l) }
# Remove any inputfix= lines outside [fixes]
$cleaned = [System.Collections.Generic.List[string]]::new()
$inFixes = $false
$inserted = $false
foreach ($l in $lines) {
    if ($l.Trim() -ieq '[fixes]') { $inFixes = $true }
    elseif ($l.Trim().StartsWith('[') -and $l.Trim().EndsWith(']')) {
        if ($inFixes -and -not $inserted) {
            $cleaned.Add('; Fast-input fix: stop PreCtrl from clearing the command buffer every frame so jutsu/chakra dash inputs')
            $cleaned.Add('; are not lost when entered within a few frames of chakra charge.')
            $cleaned.Add('inputfix=1')
            $inserted = $true
        }
        $inFixes = $false
    }
    if ($l -match '^\s*inputfix\s*=') { continue }  # skip existing inputfix lines
    $cleaned.Add($l)
}
if (-not $inserted) {
    # [fixes] was last section or missing; append
    $cleaned.Add('inputfix=1')
}
Set-Content $iniPath $cleaned -Encoding ASCII
"ini fixed: inputfix=1 now in [fixes]" | Tee-Object $log -Append

# 2. Build
$repoDir = "F:\dev\ghost-inputfix\repo"
Push-Location $repoDir
$env:VCVARS = "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
& cmd /c "build.cmd" 2>&1 | Tee-Object $log -Append
$buildOk = $LASTEXITCODE -eq 0
Pop-Location

if (-not $buildOk) {
    "BUILD FAILED (exit $LASTEXITCODE)" | Tee-Object $log -Append
    exit 1
}

# 3. Deploy built DLL to game folder
$builtDll = Join-Path $repoDir "xinput9_1_0.dll"
if (-not (Test-Path $builtDll)) {
    "ERROR: built DLL not found at $builtDll" | Tee-Object $log -Append
    exit 1
}
Copy-Item $builtDll "F:\dev\ghost-inputfix\game\xinput9_1_0.dll" -Force
$newHash = (Get-FileHash "F:\dev\ghost-inputfix\game\xinput9_1_0.dll").Hash.Substring(0,16)
"Deployed: xinput9_1_0.dll hash=$newHash" | Tee-Object $log -Append

# Also copy the updated ini from repo if it differs
$repoIni = Join-Path $repoDir "storm4perf.ini"
if (Test-Path $repoIni) {
    # Merge: keep game ini but ensure inputfix=1 is present in [fixes]
    "Repo ini exists; game ini already patched above" | Tee-Object $log -Append
}

"=== build-and-deploy end $(Get-Date -Format o) ===" | Tee-Object $log -Append