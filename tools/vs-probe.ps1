<#
.SYNOPSIS
    Kamil - Visual Studio DTE probe (VS2022 / VS2026, Open Folder + CMake).

.DESCRIPTION
    Lists running Visual Studio instances from the Running Object Table and reports what
    Kamil needs to know before driving them over COM:
      - version, opened folder, build state, active configuration
      - CMake / build / debug related commands and whether they are available
      - output window panes
      - workspace settings under <folder>\.vs that may hold the active CMake preset
    With -TryBuild it sends Build.BuildAll to the first matching instance, polls the build
    state and prints the tail of the Output > Build pane.

    Run it from a NON-elevated PowerShell while VS is open (an elevated VS is only visible
    from an elevated PowerShell, and vice versa). Results are also written to a text file.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\vs-probe.ps1
    powershell -ExecutionPolicy Bypass -File .\vs-probe.ps1 -TryBuild -Folder D:\src\SensorUI
#>
param(
    [switch]$TryBuild,
    [string]$Folder = "",
    [int]$TimeoutSec = 600,
    [string]$OutFile = (Join-Path $PSScriptRoot ("vs-probe-{0:yyyyMMdd-HHmmss}.txt" -f (Get-Date)))
)

$ErrorActionPreference = 'Continue'
$log = New-Object System.Collections.Generic.List[string]
function Out([string]$s = "") { Write-Host $s; $log.Add($s) }

if (-not ('KamilRot' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

public static class KamilRot {
    [DllImport("ole32.dll")] static extern int GetRunningObjectTable(int reserved, out IRunningObjectTable rot);
    [DllImport("ole32.dll")] static extern int CreateBindCtx(int reserved, out IBindCtx ctx);

    public static List<KeyValuePair<string, object>> Find(string prefix) {
        var result = new List<KeyValuePair<string, object>>();
        IRunningObjectTable rot;
        if (GetRunningObjectTable(0, out rot) != 0) return result;
        IEnumMoniker en;
        rot.EnumRunning(out en);
        var mon = new IMoniker[1];
        while (en.Next(1, mon, IntPtr.Zero) == 0) {
            IBindCtx ctx;
            CreateBindCtx(0, out ctx);
            string name;
            try { mon[0].GetDisplayName(ctx, null, out name); } catch { continue; }
            if (!name.StartsWith(prefix)) continue;
            object obj;
            try { rot.GetObject(mon[0], out obj); } catch { continue; }
            result.Add(new KeyValuePair<string, object>(name, obj));
        }
        return result;
    }
}
"@
}

# VS rejects COM calls while busy (RPC_E_CALL_REJECTED / RPC_E_SERVERCALL_RETRYLATER); retry briefly.
function Try-Com([scriptblock]$block, $default = $null) {
    for ($i = 0; $i -lt 20; $i++) {
        try { return & $block }
        catch {
            $hr = $_.Exception.HResult
            if ($hr -eq -2147418111 -or $hr -eq -2147417846) { Start-Sleep -Milliseconds 250; continue }
            return $default
        }
    }
    return $default
}

$buildStates = @{ 1 = 'NotStarted'; 2 = 'InProgress'; 3 = 'Done' }

Out "Kamil vs-probe  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Out "PowerShell $($PSVersionTable.PSVersion)  |  Windows $([Environment]::OSVersion.Version)"
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Out "Elevated: $elevated"
Out ""

# ---- Installed Visual Studio versions ---------------------------------------------------------
Out "=== vswhere ==="
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $installs = & $vswhere -all -prerelease -format json -utf8 | ConvertFrom-Json
    foreach ($i in $installs) {
        Out ("  {0}  v{1}" -f $i.displayName, $i.installationVersion)
        Out ("    path: {0}" -f $i.installationPath)
        $cmake = Join-Path $i.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $cmake) { Out ("    cmake: {0}" -f ((& $cmake --version | Select-Object -First 1))) }
    }
} else {
    Out "  vswhere.exe not found"
}
Out ""

# ---- Running instances ------------------------------------------------------------------------
$instances = [KamilRot]::Find('!VisualStudio.DTE.')
Out "=== Running instances: $($instances.Count) ==="
if ($instances.Count -eq 0) {
    Out "  No VS instance visible. Open your project in VS (File > Open > Folder) and re-run."
    Out "  If VS runs as administrator, run this script from an elevated PowerShell too."
}

$commandPattern = 'CMake|Configur|Preset|Cache|StartupItem|Target|^Build\.|^Debug\.Start|^Debug\.StopDebugging|^File\.OpenFolder|^Project\.'

foreach ($kv in $instances) {
    $dte = $kv.Value
    Out ""
    Out ("--- {0}" -f $kv.Key)
    Out ("  Version      : {0}" -f (Try-Com { $dte.Version }))
    Out ("  devenv       : {0}" -f (Try-Com { $dte.FullName }))
    $opened = Try-Com { $dte.Solution.FullName } ''
    Out ("  Solution     : {0}" -f $opened)
    $state = Try-Com { $dte.Solution.SolutionBuild.BuildState }
    Out ("  BuildState   : {0} ({1})" -f $state, $buildStates[[int]$state])
    Out ("  LastBuildInfo: {0}" -f (Try-Com { $dte.Solution.SolutionBuild.LastBuildInfo }))
    Out ("  ActiveConfig : {0}" -f (Try-Com { $dte.Solution.SolutionBuild.ActiveConfiguration.Name } '(none)'))
    Out ("  StartupProjs : {0}" -f ((Try-Com { $dte.Solution.SolutionBuild.StartupProjects } @()) -join ', '))
    Out ("  Debugger mode: {0}" -f (Try-Com { $dte.Debugger.CurrentMode }))

    Out "  Output panes :"
    $panes = Try-Com { @($dte.ToolWindows.OutputWindow.OutputWindowPanes) } @()
    foreach ($p in $panes) { Out ("    - {0}" -f (Try-Com { $p.Name })) }

    Out "  Commands (matching CMake/build/debug):"
    $cmds = Try-Com { @($dte.Commands) } @()
    $hits = foreach ($c in $cmds) {
        $n = Try-Com { $c.Name } ''
        if ($n -and $n -match $commandPattern) {
            [pscustomobject]@{
                Name      = $n
                Available = (Try-Com { $c.IsAvailable } $false)
                Keys      = ((Try-Com { @($c.Bindings) } @()) -join ' ; ')
            }
        }
    }
    foreach ($h in ($hits | Sort-Object Name)) {
        $mark = if ($h.Available) { 'x' } else { ' ' }
        $keys = if ($h.Keys) { "   ($($h.Keys))" } else { '' }
        Out ("    [{0}] {1}{2}" -f $mark, $h.Name, $keys)
    }

    # Workspace settings that may hold the selected configure preset (Open Folder mode).
    $root = if ($opened -and (Test-Path $opened -PathType Container)) { $opened }
            elseif ($opened) { Split-Path $opened -Parent } else { '' }
    if ($root -and (Test-Path (Join-Path $root '.vs'))) {
        Out ("  .vs workspace files under {0}:" -f $root)
        Get-ChildItem (Join-Path $root '.vs') -Recurse -Include *.json -ErrorAction SilentlyContinue |
            Where-Object { $_.Length -lt 200KB } |
            ForEach-Object {
                Out ("    {0}" -f $_.FullName.Substring($root.Length + 1))
                $text = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue
                if ($text -and $text -match 'Preset|CurrentProjectSetting|args') {
                    foreach ($line in ($text -split "`r?`n" | Where-Object { $_ -match 'Preset|CurrentProjectSetting|"args"|projectTarget' } | Select-Object -First 15)) {
                        Out ("      {0}" -f $line.Trim())
                    }
                }
            }
    }
}

# ---- Optional build trigger -------------------------------------------------------------------
if ($TryBuild -and $instances.Count -gt 0) {
    Out ""
    Out "=== TryBuild ==="
    $target = $null
    foreach ($kv in $instances) {
        $opened = Try-Com { $kv.Value.Solution.FullName } ''
        if (-not $Folder -or ($opened -and $opened.TrimEnd('\') -ieq $Folder.TrimEnd('\'))) { $target = $kv; break }
    }
    if (-not $target) {
        Out "  No instance has '$Folder' open."
    } else {
        $dte = $target.Value
        Out ("  Instance: {0}  ({1})" -f $target.Key, (Try-Com { $dte.Solution.FullName }))
        $start = Get-Date
        try {
            $dte.ExecuteCommand('Build.BuildAll', '')
            Out "  Build.BuildAll sent."
        } catch {
            Out ("  Build.BuildAll failed: {0}" -f $_.Exception.Message)
        }
        $last = $null
        while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
            Start-Sleep -Milliseconds 500
            $s = Try-Com { $dte.Solution.SolutionBuild.BuildState }
            if ($s -ne $last) { Out ("  {0,6:N1}s  BuildState={1} ({2})" -f ((Get-Date) - $start).TotalSeconds, $s, $buildStates[[int]$s]); $last = $s }
            if ($s -eq 3 -and ((Get-Date) - $start).TotalSeconds -gt 2) { break }
        }
        Out ("  LastBuildInfo (failed projects): {0}" -f (Try-Com { $dte.Solution.SolutionBuild.LastBuildInfo }))
        $panes = Try-Com { @($dte.ToolWindows.OutputWindow.OutputWindowPanes) } @()
        foreach ($p in $panes) {
            $name = Try-Com { $p.Name } ''
            if ($name -match 'Build|Derle|CMake') {
                $doc = Try-Com { $p.TextDocument }
                $text = Try-Com { $doc.StartPoint.CreateEditPoint().GetText($doc.EndPoint) } ''
                $lines = $text -split "`r?`n"
                Out ("  --- Output pane '{0}' (last 40 of {1} lines) ---" -f $name, $lines.Count)
                foreach ($l in ($lines | Select-Object -Last 40)) { Out ("    {0}" -f $l) }
            }
        }
    }
}

$log | Set-Content -Path $OutFile -Encoding UTF8
Write-Host ""
Write-Host "Saved: $OutFile"
