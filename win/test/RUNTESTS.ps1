#
# RUNTEST.ps1 -- temp script to executing all tests
#
# NOTE: this is not the ported version of the bash script,
# this is just a quick and dirty way to run all tests 
# with one command.  porting RUNTESTS is still a backlog
# item!!
#

Get-ChildItem './*/TEST*.ps1' | % {
    Push-Location $_.Directory
    & $_.FullName  
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Error "RUNTESTS FAILED at $test_script"
        Pop-Location
        Exit $LASTEXITCODE
    }
    Pop-Location
}
