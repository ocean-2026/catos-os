# demo.ps1 -- 逐条演示 sndinfo 原型的所有模式与退出码
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'sndinfo.ps1')

function Show-Run {
    param([string]$Label, [string[]]$Argv, [string]$Sim = 'ok')
    $bar = '=' * 72
    Write-Host $bar -ForegroundColor DarkCyan
    Write-Host "  sndinfo $($Argv -join ' ')" -ForegroundColor Cyan
    if ($Sim -ne 'ok') { Write-Host "  [故障注入: $Sim]" -ForegroundColor DarkYellow }
    Write-Host $bar -ForegroundColor DarkCyan
    $r = Invoke-SndInfo -Argv $Argv -SimState $Sim
    if ($r.Output.Count -gt 0) { Write-Output ($r.Output -join "`n") }
    $color = 'Red'; if ($r.ExitCode -eq 0) { $color = 'Green' }
    Write-Host "  -> exit $($r.ExitCode)" -ForegroundColor $color
    Write-Host ''
}

Show-Run -Label '帮助'       -Argv @('-h')
Show-Run -Label '版本'       -Argv @('--version')
Show-Run -Label '默认列表'   -Argv @()
Show-Run -Label '长参数列表' -Argv @('--list')
Show-Run -Label 'PCM 状态'   -Argv @('-p')
Show-Run -Label '混音控件'   -Argv @('--ctrl')
Show-Run -Label '短列表'     -Argv @('-s')
Show-Run -Label 'JSON'       -Argv @('-j')
Show-Run -Label '指定设备'   -Argv @('-d','0','-p')
Show-Run -Label '合并短参'   -Argv @('-lp')
Show-Run -Label 'watch'      -Argv @('-w','-p')
Show-Run -Label 'quiet'      -Argv @('-q')
Show-Run -Label '测试音'     -Argv @('-t')

Write-Host '######## 错误 / 退出码 ########' -ForegroundColor Yellow
Show-Run -Label '参数错误'   -Argv @('--bogus')
Show-Run -Label '缺参数'     -Argv @('-d')
Show-Run -Label '卡不存在'   -Argv @('-d','9')
Show-Run -Label '无音频设备' -Argv @('-l') -Sim 'nodrv'
Show-Run -Label '驱动失败'   -Argv @('-l') -Sim 'fault'
