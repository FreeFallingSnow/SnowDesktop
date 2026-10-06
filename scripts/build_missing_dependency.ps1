param([string]$TestName,[string]$Reason='Python 3.8+ unavailable')
Write-Output ("ENVIRONMENT-BLOCKED: "+$TestName+"; "+$Reason+". Required regression was not executed; no dependency is installed automatically.")
exit 78
