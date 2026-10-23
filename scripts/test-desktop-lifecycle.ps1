param(
  [Parameter(Mandatory = $true)][string]$Binary,
  [string]$ReportPath
)

$ErrorActionPreference = 'Stop'
$binaryPath = (Resolve-Path -LiteralPath $Binary).ProviderPath
if (-not [IO.File]::Exists($binaryPath) -or [IO.Path]::GetFileName($binaryPath) -ieq 'GlintBridge.exe') {
  throw 'Binary must name the desktop Glint.exe build.'
}

if (-not ('Glint.DesktopLifecycleSmoke' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Glint {
    public static class DesktopLifecycleSmoke {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr FindWindow(string name, string title);
        [DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll")]
        static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr OpenMutex(uint access, bool inherit, string name);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        public static void AssertNoDesktop(string sid) {
            var mutex = OpenMutex(0x00100000, false, "Local\\Glint.Desktop." + sid);
            if (mutex != IntPtr.Zero) {
                CloseHandle(mutex);
                throw new InvalidOperationException("A Glint desktop is already running; exit it before this smoke test.");
            }
            int error = Marshal.GetLastWin32Error();
            if (error != 2)
                throw new InvalidOperationException("Cannot safely check the Glint desktop mutex (Windows error " + error + ").");
            if (FindWindow("Glint.SearchWindow", null) != IntPtr.Zero)
                throw new InvalidOperationException("An existing Glint search window was found; this test will not close it.");
        }

        public static bool DesktopReady(int expectedPid) {
            var window = FindWindow("Glint.SearchWindow", null);
            uint pid;
            return window != IntPtr.Zero && GetWindowThreadProcessId(window, out pid) != 0 && pid == (uint)expectedPid;
        }

        public static void QuitDesktop(int expectedPid) {
            var window = FindWindow("Glint.SearchWindow", null);
            uint pid;
            if (window == IntPtr.Zero || GetWindowThreadProcessId(window, out pid) == 0 || pid != (uint)expectedPid)
                throw new InvalidOperationException("The desktop window does not belong to this test process.");
            // The same WM_COMMAND handled by the tray's Exit Glint entry.
            if (!PostMessage(window, 0x0111, new IntPtr(1003), IntPtr.Zero))
                throw new InvalidOperationException("Could not request the test desktop's tray exit.");
        }

        static async Task<byte[]> ReadExact(Stream stream, int size) {
            var buffer = new byte[size];
            int total = 0;
            while (total < size) {
                int count = await stream.ReadAsync(buffer, total, size - total).ConfigureAwait(false);
                if (count == 0) throw new EndOfStreamException("Native stdout closed before its status frame completed.");
                total += count;
            }
            return buffer;
        }

        static async Task<string> ReadFrameAsync(Stream stream) {
            var header = await ReadExact(stream, 4).ConfigureAwait(false);
            uint size = (uint)header[0] | ((uint)header[1] << 8) | ((uint)header[2] << 16) | ((uint)header[3] << 24);
            if (size == 0 || size > 1024 * 1024) throw new InvalidDataException("Invalid native status frame length.");
            return new UTF8Encoding(false, true).GetString(await ReadExact(stream, (int)size).ConfigureAwait(false));
        }

        public static string ReadFrame(Stream stream, int timeoutMs) {
            var task = ReadFrameAsync(stream);
            if (!task.Wait(timeoutMs)) throw new TimeoutException("Native status timed out.");
            return task.GetAwaiter().GetResult();
        }

        public static void WriteFrame(Stream stream, string message) {
            var payload = new UTF8Encoding(false, true).GetBytes(message);
            var header = BitConverter.GetBytes((uint)payload.Length);
            if (!BitConverter.IsLittleEndian) Array.Reverse(header);
            stream.Write(header, 0, header.Length);
            stream.Write(payload, 0, payload.Length);
            stream.Flush();
        }
    }
}
'@
}

$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
[Glint.DesktopLifecycleSmoke]::AssertNoDesktop($sid)

$desktopProcess = $null
$nativeProcess = $null
$cycles = @()

function Read-DesktopStatus([string]$ExpectedState) {
  $deadline = [Diagnostics.Stopwatch]::StartNew()
  while ($deadline.ElapsedMilliseconds -lt 8000) {
    $remaining = [Math]::Max(1, 8000 - [int]$deadline.ElapsedMilliseconds)
    $status = [Glint.DesktopLifecycleSmoke]::ReadFrame($nativeProcess.StandardOutput.BaseStream, $remaining) | ConvertFrom-Json
    if ($status.type -eq 'desktop_status' -and $status.version -eq 1 -and $status.state -eq $ExpectedState) {
      return $status
    }
  }
  throw "Native bridge did not report '$ExpectedState' within 8 seconds."
}

try {
  $start = [Diagnostics.ProcessStartInfo]::new($binaryPath, '--native-host')
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
  $start.RedirectStandardInput = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $nativeProcess = [Diagnostics.Process]::Start($start)
  $bridgePid = $nativeProcess.Id
  $null = Read-DesktopStatus 'waiting'
  $generation = 0

  for ($cycle = 1; $cycle -le 3; $cycle++) {
    [Glint.DesktopLifecycleSmoke]::AssertNoDesktop($sid)
    $desktopProcess = Start-Process -FilePath $binaryPath -WindowStyle Hidden -PassThru
    $ready = [Diagnostics.Stopwatch]::StartNew()
    while (-not [Glint.DesktopLifecycleSmoke]::DesktopReady($desktopProcess.Id)) {
      $desktopProcess.Refresh()
      if ($desktopProcess.HasExited) { throw 'The test desktop exited before its search window was ready.' }
      if ($ready.ElapsedMilliseconds -ge 5000) { throw 'The test desktop did not create its search window within 5 seconds.' }
      Start-Sleep -Milliseconds 50
    }

    $status = Read-DesktopStatus 'connected'
    if (-not ($status.generation -is [long] -or $status.generation -is [int]) -or $status.generation -le $generation) {
      throw 'The connected desktop generation must be a strictly increasing integer.'
    }
    $generation = $status.generation
    # Exercise the real desktop reply and native relay, including later
    # reconnects. This never changes the user's saved language preference.
    $hello = @{ type = 'hello'; version = 1; desktopGeneration = $generation;
      sessionId = "lifecycle-$cycle"; profileId = 'lifecycle-smoke'; browser = 'chrome' } | ConvertTo-Json -Compress
    [Glint.DesktopLifecycleSmoke]::WriteFrame($nativeProcess.StandardInput.BaseStream, $hello)
    $language = [Glint.DesktopLifecycleSmoke]::ReadFrame($nativeProcess.StandardOutput.BaseStream, 5000) | ConvertFrom-Json
    if ($language.type -ne 'language_settings' -or $language.version -ne 1 -or
        $language.preference -notin @('system', 'en', 'zh-CN') -or $language.language -notin @('en', 'zh-CN') -or
        ($language.preference -ne 'system' -and $language.preference -ne $language.language)) {
      throw 'The desktop did not send valid language settings after the browser hello.'
    }
    $desktopPid = $desktopProcess.Id
    $exitClock = [Diagnostics.Stopwatch]::StartNew()
    [Glint.DesktopLifecycleSmoke]::QuitDesktop($desktopPid)
    if (-not $desktopProcess.WaitForExit(5000)) { throw 'Tray exit did not terminate the test desktop within 5 seconds.' }
    $exitClock.Stop()
    if ($desktopProcess.ExitCode -ne 0) { throw "The test desktop reported a failed exit: $($desktopProcess.ExitCode)." }
    $desktopProcess.Dispose()
    $desktopProcess = $null

    $null = Read-DesktopStatus 'waiting'
    $nativeProcess.Refresh()
    if ($nativeProcess.HasExited -or $nativeProcess.Id -ne $bridgePid) {
      throw 'The native bridge exited or changed PID while the desktop was stopped.'
    }
    [Glint.DesktopLifecycleSmoke]::AssertNoDesktop($sid)
    $cycles += [pscustomobject]@{
      Cycle = $cycle; DesktopPID = $desktopPid; BridgePID = $bridgePid
      Generation = $generation; TrayExitMilliseconds = $exitClock.Elapsed.TotalMilliseconds
      WaitingAfterExit = $true
      LanguagePreference = $language.preference; Language = $language.language
    }
  }

  $nativeProcess.Refresh()
  $cpuBefore = $nativeProcess.TotalProcessorTime.TotalMilliseconds
  $sample = [Diagnostics.Stopwatch]::StartNew()
  Start-Sleep -Seconds 5
  $sample.Stop()
  $nativeProcess.Refresh()
  if ($nativeProcess.HasExited) { throw 'The bridge did not remain waiting during the offline CPU sample.' }
  $offlineCpu = $nativeProcess.TotalProcessorTime.TotalMilliseconds - $cpuBefore
  [Glint.DesktopLifecycleSmoke]::AssertNoDesktop($sid)

  $nativeProcess.StandardInput.Close()
  if (-not $nativeProcess.WaitForExit(3000)) { throw 'Browser stdin EOF did not stop the offline bridge within 3 seconds.' }
  if ($nativeProcess.ExitCode -ne 0) { throw "The bridge reported a failed exit after browser EOF: $($nativeProcess.ExitCode)." }
  if ($nativeProcess.StandardError.ReadToEnd().Length -ne 0) { throw 'The native bridge wrote unexpected stderr output.' }

  $report = [pscustomobject]@{
    Binary = $binaryPath; SHA256 = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    Cycles = $cycles; SameBridgeAcrossCycles = $true
    OfflineSampleSeconds = $sample.Elapsed.TotalSeconds; OfflineCpuMilliseconds = $offlineCpu
    BrowserEofExit = $true; DesktopLeftStopped = $true
  }
  if ($ReportPath) {
    $reportFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ReportPath)
    $directory = Split-Path -Parent $reportFile
    if ([string]::IsNullOrEmpty($directory)) { $directory = (Get-Location).ProviderPath }
    if (-not [IO.Directory]::Exists($directory)) { $null = New-Item -ItemType Directory -Path $directory -Force }
    [IO.File]::WriteAllText($reportFile, ($report | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
  }
  Write-Output ("Lifecycle smoke passed: 3 tray exits, one bridge PID, automatic desktop reconnect with language sync, browser EOF exit; offline CPU {0:N3} ms / {1:N3} s." -f $offlineCpu, $sample.Elapsed.TotalSeconds)
  if ($ReportPath) { Write-Output "Report: $reportFile" }
} finally {
  # Only touch processes created by this invocation; never adopt an existing app.
  if ($desktopProcess) {
    try {
      if (-not $desktopProcess.HasExited) {
        try { [Glint.DesktopLifecycleSmoke]::QuitDesktop($desktopProcess.Id) } catch {}
        if (-not $desktopProcess.WaitForExit(3000)) {
          $desktopProcess.Kill()
          $null = $desktopProcess.WaitForExit(2000)
        }
      }
    } finally { $desktopProcess.Dispose() }
  }
  if ($nativeProcess) {
    try {
      if (-not $nativeProcess.HasExited) {
        try { $nativeProcess.StandardInput.Close() } catch {}
        if (-not $nativeProcess.WaitForExit(3000)) {
          $nativeProcess.Kill()
          $null = $nativeProcess.WaitForExit(2000)
        }
      }
    } finally { $nativeProcess.Dispose() }
  }
}
