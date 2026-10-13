[CmdletBinding()]
param(
    [string]$PipeName = 'desktop',
    [Parameter(Mandatory=$true)][string]$Request,
    [int]$TimeoutMilliseconds = 20000
)
$ErrorActionPreference = 'Stop'
if ($PipeName -notmatch '^[a-zA-Z0-9_-]{1,80}$') { throw 'Invalid pipe name.' }
# Parse locally to catch malformed input, then transmit one compact UTF-8 line.
$payload = $Request | ConvertFrom-Json | ConvertTo-Json -Depth 30 -Compress
if ([Text.Encoding]::UTF8.GetByteCount($payload) -gt 65536) { throw 'Request exceeds 64 KiB.' }
$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', "stale-$PipeName", [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
try {
    $pipe.Connect(3000)
    $writer = [IO.StreamWriter]::new($pipe, [Text.UTF8Encoding]::new($false), 4096, $true)
    $reader = [IO.StreamReader]::new($pipe, [Text.UTF8Encoding]::new($false), $false, 4096, $true)
    $writer.AutoFlush = $true
    $writer.WriteLine($payload)
    $read = $reader.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) { throw 'Agent response timed out. Query state before retrying an action.' }
    if (-not $read.Result) { throw 'App closed the connection without a response.' }
    $writer.WriteLine('ACK')
    $read.Result
} finally {
    if ($writer) { $writer.Dispose() }
    if ($reader) { $reader.Dispose() }
    $pipe.Dispose()
}
