@echo off
rem Golden Balloon Windows test bundle. Double-click to run every test listed
rem in tests\tests.txt; the results are written to windows-test-results.txt
rem beside this file. The batch part only starts Windows PowerShell, which
rem reads the rest of this file (after the marker line) and runs it. Keep the
rem whole file ASCII: cmd.exe reads it in the console code page.
setlocal EnableExtensions DisableDelayedExpansion
title Golden Balloon Windows tests
set "GB_RUNNER=%~f0"
set "GB_BUNDLE=%~dp0"
set "GB_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%GB_POWERSHELL%" set "GB_POWERSHELL=powershell.exe"
"%GB_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$t = [IO.File]::ReadAllText($env:GB_RUNNER); $i = $t.IndexOf([string][char]10 + '#POWER' + 'SHELL', [StringComparison]::Ordinal); if ($i -lt 0) { exit 98 }; Invoke-Expression $t.Substring($i + 1)"
set "GB_EXIT=%ERRORLEVEL%"
if "%GB_EXIT%"=="98" echo The test runner could not read RUN-TESTS.cmd itself.
echo.
pause
endlocal & exit /b %GB_EXIT%

#POWERSHELL
# Everything below is Windows PowerShell 5.1 and runs once per double-click.
# Each test runs from a copy of its executable in its own scratch folder, with
# TEMP and TMP pointing inside it, so nothing is written next to this file
# except the two result files. Every test runs from a folder whose path is
# plain ASCII; the tests marked "unicode" in tests\tests.txt run again from a
# folder whose path has spaces, Latin-1 and CJK letters (see
# tools/windows_test_bundle/select_tests.py for which tests and why).

$ErrorActionPreference = 'Continue'
$bundle = $env:GB_BUNDLE
$testsDir = [IO.Path]::Combine($bundle, 'tests')
$dataDir = [IO.Path]::Combine($testsDir, 'data')
$manifestPath = [IO.Path]::Combine($testsDir, 'tests.txt')
$utf8 = New-Object Text.UTF8Encoding($false)

if (-not [IO.File]::Exists($manifestPath)) {
    Write-Host 'The tests folder is missing next to RUN-TESTS.cmd.' -ForegroundColor Red
    Write-Host 'Unzip the whole folder first (right-click the zip, Extract All),'
    Write-Host 'then double-click RUN-TESTS.cmd inside the extracted folder.'
    exit 2
}

# A crashing test must not leave a "stopped working" dialog waiting for a
# click; child processes inherit this error mode.
try {
    Add-Type -Namespace GoldenBalloon -Name NativeMethods -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetErrorMode(uint mode);
'@
    [void][GoldenBalloon.NativeMethods]::SetErrorMode(0x8003)
} catch {
}

function Get-Text([string]$path) {
    try { return [IO.File]::ReadAllText($path, $utf8).Trim() } catch { return '' }
}

function Read-Registry([string]$key, [string]$name) {
    try { return [string](Get-ItemProperty -LiteralPath $key -Name $name -ErrorAction Stop).$name } catch { return '?' }
}

function Test-Ascii([string]$text) {
    foreach ($c in $text.ToCharArray()) { if ([int]$c -gt 127) { return $false } }
    return $true
}

# The Windows command-line rule (CommandLineToArgvW and the C runtime): quote
# the argument, double each backslash run that precedes a quote, escape quotes.
function ConvertTo-Argument([string]$text) {
    if ($text.Length -gt 0 -and $text -notmatch '[\s"]') { return $text }
    $out = New-Object Text.StringBuilder
    [void]$out.Append([char]34)
    $slashes = 0
    foreach ($c in $text.ToCharArray()) {
        if ($c -eq [char]92) { $slashes++; continue }
        if ($c -eq [char]34) {
            [void]$out.Append([char]92, 2 * $slashes + 1)
        } elseif ($slashes -gt 0) {
            [void]$out.Append([char]92, $slashes)
        }
        [void]$out.Append($c)
        $slashes = 0
    }
    if ($slashes -gt 0) { [void]$out.Append([char]92, 2 * $slashes) }
    [void]$out.Append([char]34)
    return $out.ToString()
}

function Copy-Folder([string]$from, [string]$to) {
    [void][IO.Directory]::CreateDirectory($to)
    foreach ($file in [IO.Directory]::GetFiles($from)) {
        [IO.File]::Copy($file, [IO.Path]::Combine($to, [IO.Path]::GetFileName($file)), $true)
    }
    foreach ($folder in [IO.Directory]::GetDirectories($from)) {
        Copy-Folder $folder ([IO.Path]::Combine($to, [IO.Path]::GetFileName($folder)))
    }
}

function Read-Manifest {
    $tests = New-Object Collections.Generic.List[object]
    foreach ($line in [IO.File]::ReadAllLines($manifestPath, $utf8)) {
        if ($line.Length -eq 0 -or $line.StartsWith('#')) { continue }
        $f = $line.Split([char]9)
        if ($f.Count -ne 8) { throw "bad manifest line: $line" }
        $tests.Add([pscustomobject]@{
            Name = $f[0]; Exe = $f[1]; Timeout = [int]$f[2]
            Expect = $(if ($f[3] -eq '-') { '' } else { $f[3] })
            Env = $(if ($f[4] -eq '-') { @() } else { $f[4].Split('|') })
            Args = $(if ($f[5] -eq '-') { @() } else { $f[5].Split('|') })
            Unicode = ($f[6] -eq 'unicode')
            Folders = $(if ($f[7] -eq '-') { @() } else { $f[7].Split('|') })
        })
    }
    return $tests
}

# Runs one test from a fresh folder below $root and returns what happened.
function Invoke-Test($test, [string]$root, [int]$index) {
    # Short names keep a test's own nested paths as far from MAX_PATH as they
    # are in a build tree.
    $work = [IO.Path]::Combine($root, ('{0:D3}' -f $index))
    $temp = [IO.Path]::Combine($work, 'temp')
    $result = [pscustomobject]@{ Passed = $false; Summary = ''; Seconds = 0.0; Output = ''; Folder = $work }
    try {
        [void][IO.Directory]::CreateDirectory($temp)
        foreach ($folder in $test.Folders) {
            [void][IO.Directory]::CreateDirectory([IO.Path]::Combine($work, $folder))
        }
        $exe = [IO.Path]::Combine($work, $test.Exe)
        [IO.File]::Copy([IO.Path]::Combine($testsDir, $test.Exe), $exe, $true)
        $arguments = @()
        foreach ($a in $test.Args) {
            if ($a.Contains('data\') -and -not [IO.Directory]::Exists([IO.Path]::Combine($work, 'data'))) {
                Copy-Folder $dataDir ([IO.Path]::Combine($work, 'data'))
            }
            $arguments += (ConvertTo-Argument $a.Replace('{WORK}', $work))
        }
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = $arguments -join ' '
        $psi.WorkingDirectory = $work
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = $utf8
        $psi.StandardErrorEncoding = $utf8
        $psi.EnvironmentVariables['TEMP'] = $temp
        $psi.EnvironmentVariables['TMP'] = $temp
        foreach ($e in $test.Env) {
            $pair = $e.Split([char[]]'=', 2)
            $psi.EnvironmentVariables[$pair[0]] = $pair[1].Replace('{WORK}', $work)
        }
    } catch {
        $result.Summary = 'could not prepare: ' + $_.Exception.Message
        return $result
    }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        $process = [Diagnostics.Process]::Start($psi)
    } catch {
        $result.Summary = 'could not start: ' + $_.Exception.Message
        return $result
    }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit($test.Timeout * 1000)
    if (-not $finished) {
        & ([IO.Path]::Combine($env:SystemRoot, 'System32\taskkill.exe')) /T /F /PID $process.Id 2>&1 | Out-Null
        [void]$process.WaitForExit(15000)
    }
    [void]$stdout.Wait(15000)
    [void]$stderr.Wait(15000)
    $clock.Stop()
    $result.Seconds = $clock.Elapsed.TotalSeconds
    $out = $(if ($stdout.IsCompleted) { $stdout.Result } else { '(standard output not captured)' })
    $err = $(if ($stderr.IsCompleted) { $stderr.Result } else { '(standard error not captured)' })
    $result.Output = $out
    if ($err.Length -gt 0) { $result.Output += "`r`n--- standard error ---`r`n" + $err }
    if (-not $finished) {
        $result.Summary = "timed out after $($test.Timeout) s"
        return $result
    }
    $code = $process.ExitCode
    if ($code -ne 0) {
        $result.Summary = $(if ($code -lt 0 -or $code -gt 255) { 'exit 0x{0:X8}' -f $code } else { "exit $code" })
        return $result
    }
    # CTest's PASS_REGULAR_EXPRESSION is case-sensitive, hence -cmatch. Unlike
    # CTest, which then ignores the exit code, this runner also requires exit
    # 0 (in this bundle that only concerns app_lifecycle).
    if ($test.Expect.Length -gt 0 -and -not ($out + $err -cmatch $test.Expect)) {
        $result.Summary = 'exit 0 but the output lacks: ' + $test.Expect
        return $result
    }
    $result.Passed = $true
    return $result
}

$tests = Read-Manifest
$resultsPath = [IO.Path]::Combine($bundle, 'windows-test-results.txt')
$outputPath = [IO.Path]::Combine($bundle, 'windows-test-output.txt')
try {
    [IO.File]::WriteAllText($resultsPath, '', $utf8)
} catch {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $resultsPath = [IO.Path]::Combine($desktop, 'windows-test-results.txt')
    $outputPath = [IO.Path]::Combine($desktop, 'windows-test-output.txt')
    Write-Host "This folder is read-only; the results go to $desktop instead." -ForegroundColor Yellow
}

$stamp = Get-Date -Format 'HHmmss'
$tempRoot = [IO.Path]::GetTempPath()
# "Golden Balloon <U-umlaut>n<i-diaeresis> <two CJK letters> <time>": Latin-1
# and CJK letters together, which no single ANSI code page can spell.
$unicodeName = 'Golden Balloon ' + [char]0x00DC + 'n' + [char]0x00EF + ' ' + [char]0x540D + [char]0x524D + ' ' + $stamp
$runRoot = [IO.Path]::Combine($tempRoot, $unicodeName)
# The ASCII folder: under %TEMP% when its path is ASCII, else under the first
# ASCII one of the public profile and the system drive's root.
$asciiRoot = ''
foreach ($candidate in @($tempRoot, $env:PUBLIC, ($env:SystemDrive + '\'))) {
    if ($candidate -and (Test-Ascii $candidate) -and [IO.Directory]::Exists($candidate)) {
        $asciiRoot = [IO.Path]::Combine($candidate, 'GoldenBalloonTests ' + $stamp)
        break
    }
}

$header = New-Object Collections.Generic.List[string]
$header.Add('Golden Balloon Windows test results')
$header.Add('Bundle:        ' + (Get-Text ([IO.Path]::Combine($testsDir, 'BUILD.txt'))))
$header.Add('Run at:        ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))
$header.Add('ver:           ' + ((& cmd.exe /d /c ver) -join ' ').Trim())
$nt = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$header.Add('Windows:       ' + (Read-Registry $nt 'ProductName') + ', ' + (Read-Registry $nt 'DisplayVersion') + ', build ' + (Read-Registry $nt 'CurrentBuild') + '.' + (Read-Registry $nt 'UBR'))
$header.Add('Architecture:  ' + $env:PROCESSOR_ARCHITECTURE + ', PowerShell ' + $PSVersionTable.PSVersion)
$cp = 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage'
$header.Add('Code pages:    ANSI ' + (Read-Registry $cp 'ACP') + ', OEM ' + (Read-Registry $cp 'OEMCP'))
$header.Add('Bundle folder: ' + $bundle)
if (-not $asciiRoot) { $asciiRoot = $runRoot }
$header.Add('ASCII folder:  ' + $(if ($asciiRoot -ne $runRoot) { $asciiRoot } else { '(none; every run uses the non-ASCII folder)' }))
$header.Add('Non-ASCII:     ' + $runRoot)
$header.Add('')
[IO.File]::WriteAllText($resultsPath, ($header -join "`r`n") + "`r`n", $utf8)
[IO.File]::WriteAllText($outputPath, ($header -join "`r`n") + "`r`n", $utf8)
$header | ForEach-Object { Write-Host $_ }

$failures = New-Object Collections.Generic.List[string]
$passed = 0
$index = 0
foreach ($test in $tests) {
    $index++
    Write-Host -NoNewline ('[{0}/{1}] {2} ... ' -f $index, $tests.Count, $test.Name)
    $runs = @()
    $runs += ,@($(if ($asciiRoot -ne $runRoot) { 'ASCII folder' } else { 'non-ASCII folder' }), (Invoke-Test $test $asciiRoot $index))
    if ($test.Unicode -and $asciiRoot -ne $runRoot) {
        $runs += ,@('non-ASCII folder', (Invoke-Test $test $runRoot $index))
    }
    $ok = $true
    $parts = @()
    foreach ($pair in $runs) {
        $run = $pair[1]
        if (-not $run.Passed) { $ok = $false }
        $state = $(if ($run.Passed) { '' } else { ', ' + $run.Summary })
        $parts += $(if ($runs.Count -gt 1) { $pair[0] + ': ' } else { '' }) + ('{0:N1} s' -f $run.Seconds) + $state
        if (-not $run.Passed) {
            $failures.Add('===== ' + $test.Name + ', ' + $pair[0] + ' (' + $run.Summary + ') in ' + $run.Folder)
            $failures.Add($run.Output.TrimEnd())
            $failures.Add('')
        }
        [IO.File]::AppendAllText($outputPath, "===== $($test.Name), $($pair[0]) ($(if ($run.Passed) { 'PASS' } else { $run.Summary }))`r`n" + $run.Output.TrimEnd() + "`r`n`r`n", $utf8)
    }
    $line = $(if ($ok) { 'PASS  ' } else { 'FAIL  ' }) + $test.Name + '  (' + ($parts -join '; ') + ')'
    if ($ok) {
        $passed++
        Write-Host 'PASS' -ForegroundColor Green
    } else {
        Write-Host 'FAIL' -ForegroundColor Red
    }
    $detail = New-Object Collections.Generic.List[string]
    foreach ($skip in ($runs[0][1].Output -split "`r?`n")) {
        if ($skip -cmatch '\bSKIP\b') { $detail.Add('      ' + $skip.Trim()) }
    }
    [IO.File]::AppendAllText($resultsPath, (@($line) + $detail -join "`r`n") + "`r`n", $utf8)
}

$cleanup = @()
foreach ($root in @($runRoot, $asciiRoot)) {
    if ($root -and [IO.Directory]::Exists($root)) {
        try { [IO.Directory]::Delete($root, $true) } catch { $cleanup += "Could not remove $root ($($_.Exception.Message))" }
    }
}

$overall = $(if ($passed -eq $tests.Count) { 'OVERALL: PASS' } else { 'OVERALL: FAIL' }) + " ($passed of $($tests.Count) tests passed)"
$tail = New-Object Collections.Generic.List[string]
$tail.Add('')
if ($failures.Count -gt 0) {
    $tail.Add('Full output of each failing test:')
    $tail.Add('')
    $failures | ForEach-Object { $tail.Add($_) }
}
$cleanup | ForEach-Object { $tail.Add($_) }
$tail.Add($overall)
[IO.File]::AppendAllText($resultsPath, ($tail -join "`r`n") + "`r`n", $utf8)
[IO.File]::AppendAllText($outputPath, $overall + "`r`n", $utf8)

Write-Host ''
$cleanup | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
Write-Host $overall -ForegroundColor $(if ($passed -eq $tests.Count) { 'Green' } else { 'Red' })
Write-Host ('Results: ' + $resultsPath)
exit $(if ($passed -eq $tests.Count) { 0 } else { 1 })
