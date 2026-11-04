# elevated: clean reinstall of the octnic driver + port 1, then rename NICs
$w = $args[0]
Start-Transcript "$env:ProgramData\octnic\reinstall.log" -Force | Out-Null
Get-PnpDevice -Class Net | Where-Object HardwareID -match 'OCTNIC_PORT1' | ForEach-Object { pnputil /remove-device $_.InstanceId }
pnputil /enum-drivers | Select-String 'Published Name:\s+(oem\d+\.inf)' -Context 0,1 | ForEach-Object {
  $n = $_.Matches[0].Groups[1].Value; if (($_.Context.PostContext -join ' ') -match 'octnic') { pnputil /delete-driver $n /uninstall } }
Start-Sleep 4
pnputil /add-driver "$w\dist\octnic.inf" /install
& "$w\dist\octnic-addport.ps1" -Inf "$w\dist\octnic.inf"
Start-Sleep 10
foreach ($pair in @(@('*10GbE Port 0*','oct0'),@('*10GbE Port 1*','oct1'))) {
  $a = Get-NetAdapter -IncludeHidden | Where-Object { $_.InterfaceDescription -like $pair[0] -and $_.Name -ne $pair[1] -and $_.Status -ne 'Not Present' } | Select -First 1
  if ($a) { Rename-NetAdapter -Name $a.Name -NewName $pair[1] }
}
Get-NetAdapter | Format-Table Name,InterfaceDescription,Status
Stop-Transcript | Out-Null
