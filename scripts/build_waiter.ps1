[CmdletBinding()]
param([ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$Participant,[ValidatePattern('^[a-f0-9]{32}$')][string]$Batch,[long]$Revision)
# Detached shell launch does not inherit a caller's captured pipes. Logs are
# redirected inside the worker, keeping ready nonblocking even in API tools.
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$prefix=Join-Path $root ('.build/collaboration/'+$Batch+'.'+$Participant+'.r'+$Revision+'.'+[Guid]::NewGuid().ToString('N'))
& (Join-Path $PSScriptRoot 'build_manager.ps1') finish $Participant -Batch $Batch -Revision $Revision -AutoCheck 1> ($prefix+'.out') 2> ($prefix+'.err')
