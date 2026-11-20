# sndinfo.ps1 -- CatOS `sndinfo` 命令的 PowerShell 原型
# 目的：在移植进内核之前，先验证 POSIX 风格长短参数解析、各模式输出、
#       JSON/简短格式以及退出码的实际效果。
#
# 运行方式（本文件既可 dot-source 也可直接 -File 执行）：
#   . .\sndinfo.ps1
#   $r = Invoke-SndInfo -Argv @('-p','-d','0'); $r.ExitCode; $r.Output
#   powershell -NoProfile -ExecutionPolicy Bypass -File sndinfo.ps1 -l -j

Set-StrictMode -Version 2.0

# ---------------------------------------------------------------------------
# 通用 POSIX 选项解析器（可被其他命令复用）
#   Spec: @{ 'h' = @{ long='help'; arg=$false }; 'd' = @{ long='dev'; arg=$true }; ... }
#   支持：-h  -lp（短选项合并）  -d0 / -d 0  --dev 0 / --dev=0  --de（长选项唯一前缀）
#         -- 结束选项解析；未知选项 -> error
# ---------------------------------------------------------------------------
function Parse-PosixOptions {
    param(
        [string[]]$Argv,
        [hashtable]$Spec
    )

    $result = @{ opts = @{}; positional = @(); error = $null }
    $longMap = @{}
    foreach ($k in $Spec.Keys) { $longMap[$Spec[$k].long] = $k }

    $endOptions = $false
    for ($i = 0; $i -lt $Argv.Count; $i++) {
        $a = $Argv[$i]
        if ($endOptions) { $result.positional += $a; continue }
        if ($a -eq '--') { $endOptions = $true; continue }

        if ($a.StartsWith('--') -and $a.Length -gt 2) {
            $body = $a.Substring(2)
            $name = $body; $value = $null
            $eq = $body.IndexOf('=')
            if ($eq -ge 0) {
                $name = $body.Substring(0, $eq)
                $value = $body.Substring($eq + 1)
            }
            # 唯一前缀匹配（GNU 风格）
            $matches = @($longMap.Keys | Where-Object { $_ -like "$name*" })
            if ($matches.Count -eq 0) {
                $result.error = "未识别的长选项: --$name"; return $result
            }
            if ($matches.Count -gt 1 -and -not $longMap.ContainsKey($name)) {
                $result.error = "长选项 --$name 有歧义: $($matches -join ', ')"; return $result
            }
            $canonical = if ($longMap.ContainsKey($name)) { $longMap[$name] } else { $longMap[$matches[0]] }
            if ($Spec[$canonical].arg) {
                if ($null -eq $value) {
                    $i++
                    if ($i -ge $Argv.Count) { $result.error = "--$name 缺少参数"; return $result }
                    $value = $Argv[$i]
                }
                $result.opts[$canonical] = $value
            } else {
                if ($null -ne $value) { $result.error = "--$name 不接受参数"; return $result }
                $result.opts[$canonical] = $true
            }
            continue
        }

        if ($a.StartsWith('-') -and $a.Length -gt 1 -and $a -ne '-') {
            $chars = $a.Substring(1).ToCharArray()
            for ($j = 0; $j -lt $chars.Length; $j++) {
                $c = [string]$chars[$j]
                if (-not $Spec.ContainsKey($c)) {
                    $result.error = "未识别的短选项: -$c"; return $result
                }
                if ($Spec[$c].arg) {
                    $tail = ''
                    if (($j + 1) -lt $chars.Length) {
                        $tail = ($chars[($j + 1)..($chars.Length - 1)] -join '')
                    }
                    if ($tail.Length -gt 0) {
                        $result.opts[$c] = $tail
                    } else {
                        $i++
                        if ($i -ge $Argv.Count) { $result.error = "-$c 缺少参数"; return $result }
                        $result.opts[$c] = $Argv[$i]
                    }
                    break
                }
                $result.opts[$c] = $true
            }
            continue
        }

        $result.positional += $a
    }
    return $result
}

# ---------------------------------------------------------------------------
# 模拟的音频硬件数据库（原型使用；内核版来自 snd_info() / es1371_info()）
# ---------------------------------------------------------------------------
function Get-FakeCards {
    return @(
        [pscustomobject]@{
            id = 0
            name = 'HDA Generic Audio'
            driver = 'snd-hda'
            drvver = 'CatOS Audio Subsys v0.3'
            pci = '00:1f.3'
            pcm = @(
                [pscustomobject]@{ id='pcm0p'; dir='playback'; ch_min=1; ch_max=2;
                    rates='8k~192k'; fmts=@('S16LE','S24LE','F32LE');
                    state='RUNNING'; rate=44100; ch=2; fmt='S16LE';
                    buf=4096; period=1024; avail=1240; underrun=0; overrun=0 }
                [pscustomobject]@{ id='pcm0c'; dir='capture'; ch_min=1; ch_max=2;
                    rates='8k~48k'; fmts=@('S16LE');
                    state='STOPPED'; rate=0; ch=0; fmt='-';
                    buf=2048; period=512; avail=0; underrun=0; overrun=0 }
            )
            ctrls = @(
                [pscustomobject]@{ name='Master Playback'; kind='volume'; val=72; unit='%';  muted=$false }
                [pscustomobject]@{ name='Master Capture';  kind='volume'; val=45; unit='%';  muted=$false }
                [pscustomobject]@{ name='Speaker';         kind='switch'; val=1;  unit='';   muted=$false }
                [pscustomobject]@{ name='Mic Boost';       kind='gain';   val=12; unit='dB'; muted=$false }
            )
        }
    )
}

function Get-HelpLines {
    return @(
        '用法: sndinfo [选项]'
        ''
        '选项:'
        '  -h, --help            显示本帮助'
        '  -v, --version         打印 sndinfo 版本与音频驱动版本'
        '  -l, --list            列出所有声卡与 PCM 设备（默认动作）'
        '  -p, --pcm             查看 PCM 流实时状态：缓冲区、周期、运行状态、采样参数'
        '  -c, --ctrl            列出混音控制项（音量、静音、增益、通道开关）'
        '  -d N, --dev N         只查询指定声卡编号 N'
        '  -w, --watch           持续刷新，实时监控缓冲区、underrun/overrun'
        '  -j, --json            JSON 输出，供 osh 脚本 / GUI 面板调用'
        '  -s, --short           简短列表模式，适合管道过滤'
        '  -q, --quiet           只设置退出码，不输出文本'
        '  -t, --tone            查询后播放一次测试音（映射 snd_tone()）'
        ''
        '退出码:'
        '  0  成功，查到音频硬件'
        '  1  参数错误'
        '  2  音频子系统未初始化 / 无声卡设备'
        '  3  指定 card id 不存在'
        '  4  驱动查询接口调用失败'
        ''
        '示例:'
        '  sndinfo                 # 等价于 sndinfo -l'
        '  sndinfo -p -d 0 -w      # 实时盯住 0 号卡的 PCM'
        '  sndinfo -j -s           # 简短 JSON'
    )
}

function Render-ListMode {
    param($Card)
    $o = @()
    $o += "Card $($Card.id): $($Card.name)"
    $o += "Driver: $($Card.driver) (CatOS Audio Subsys v0.3)"
    $o += "PCI: $($Card.pci)"
    $o += 'PCM Devices:'
    foreach ($p in $Card.pcm) {
        $dir = if ($p.dir -eq 'playback') { 'Playback' } else { 'Capture ' }
        $ch = if ($p.ch_min -eq $p.ch_max) { "$($p.ch_min)" } else { "$($p.ch_min)/$($p.ch_max)" }
        $o += ("{0}: {1}, channels {2}, rates {3}, fmt {4}" -f $p.id, $dir, $ch, $p.rates, ($p.fmts -join ' '))
    }
    $o += 'Mixer controls:'
    foreach ($c in $Card.ctrls) { $o += "  $($c.name)" }
    return $o
}

function Render-PcmMode {
    param($Card)
    $o = @()
    foreach ($p in $Card.pcm) {
        $o += "PCM $($p.id) ($($p.dir)):"
        $o += "State: $($p.state)"
        if ($p.state -eq 'RUNNING') {
            $o += "Sample rate: $($p.rate) Hz, $($p.ch)ch $($p.fmt)"
            $o += "Buffer size: $($p.buf) frames, period: $($p.period) frames"
            $o += "Avail: $($p.avail) frames"
            $o += "Underrun count: $($p.underrun)"
        }
        if ($p.overrun -gt 0) { $o += "Overrun count: $($p.overrun)" }
        $o += ''
    }
    return $o
}

function Render-CtrlMode {
    param($Card)
    $o = @("Card $($Card.id) mixer controls:")
    foreach ($c in $Card.ctrls) {
        if ($c.kind -eq 'volume') {
            $tag = if ($c.muted) { 'muted' } else { 'unmuted' }
            $o += ("{0}: {1}{2} [{3}]" -f $c.name, $c.val, $c.unit, $tag)
        } elseif ($c.kind -eq 'gain') {
            $o += ("{0}: +{1}{2}" -f $c.name, $c.val, $c.unit)
        } else {
            $o += ("{0}: {1}" -f $c.name, $(if ($c.val) { 'on' } else { 'off' }))
        }
    }
    return $o
}

function Render-ShortMode {
    param($Card)
    $parts = @()
    foreach ($p in $Card.pcm) {
        $d = if ($p.dir -eq 'playback') { 'play' } else { 'capture' }
        $parts += "$($p.id):$d"
    }
    $shortName = $Card.name -replace ' Audio$',''
    return "$($Card.id) $shortName $($parts -join ' ')"
}

function Render-JsonMode {
    param($Cards)
    $cardsOut = @()
    foreach ($c in $Cards) {
        $pcmOut = @()
        foreach ($p in $c.pcm) {
            $pcmOut += [ordered]@{ id=$p.id; dir=$p.dir; ch_min=$p.ch_min; ch_max=$p.ch_max }
        }
        $cardsOut += [ordered]@{
            card_id = $c.id
            name = $c.name
            driver = $c.driver
            pcm = @($pcmOut)
        }
    }
    $doc = [ordered]@{ cards = @($cardsOut) }
    return ($doc | ConvertTo-Json -Depth 8 -Compress:$false).Split("`n")
}

# ---------------------------------------------------------------------------
# 主入口
#   -Argv       : 命令行参数
#   -SimState   : 原型专用故障注入（ok / nodrv / fault），用于演示退出码 2/4
# 返回: @{ ExitCode = int; Output = string[] }
# ---------------------------------------------------------------------------
function Invoke-SndInfo {
    param(
        [string[]]$Argv = @(),
        [string]$SimState = 'ok'
    )

    $spec = @{
        'h' = @{ long='help';    arg=$false }
        'v' = @{ long='version'; arg=$false }
        'l' = @{ long='list';    arg=$false }
        'p' = @{ long='pcm';     arg=$false }
        'c' = @{ long='ctrl';    arg=$false }
        'd' = @{ long='dev';     arg=$true  }
        'w' = @{ long='watch';   arg=$false }
        'j' = @{ long='json';    arg=$false }
        's' = @{ long='short';   arg=$false }
        'q' = @{ long='quiet';   arg=$false }
        't' = @{ long='tone';    arg=$false }
    }
    $out = @()
    $p = Parse-PosixOptions -Argv $Argv -Spec $spec
    if ($p.error) {
        return @{ ExitCode = 1; Output = @("sndinfo: $($p.error)", 'sndinfo: 使用 -h 查看帮助') }
    }
    $o = $p.opts

    if ($o.ContainsKey('h')) {
        return @{ ExitCode = 0; Output = (Get-HelpLines) }
    }
    if ($o.ContainsKey('v')) {
        return @{ ExitCode = 0; Output = @('sndinfo 0.3.0 (catos-userland)',
                                            'audio driver: snd-hda / CatOS Audio Subsys v0.3') }
    }

    # 故障注入：模拟音频子系统未初始化 / 驱动查询失败
    if ($SimState -eq 'nodrv') {
        return @{ ExitCode = 2; Output = @('sndinfo: 音频子系统未初始化 / 无声卡设备') }
    }
    if ($SimState -eq 'fault') {
        return @{ ExitCode = 4; Output = @('sndinfo: 驱动查询接口调用失败') }
    }

    $cards = @(Get-FakeCards)
    if ($cards.Count -eq 0) {
        return @{ ExitCode = 2; Output = @('sndinfo: 音频子系统未初始化 / 无声卡设备') }
    }

    if ($o.ContainsKey('d')) {
        $raw = [string]$o['d']
        $id = -1
        if (-not [int]::TryParse($raw, [ref]$id)) {
            return @{ ExitCode = 1; Output = @("sndinfo: --dev 需要数字编号，收到 '$raw'") }
        }
        $cards = @($cards | Where-Object { $_.id -eq $id })
        if ($cards.Count -eq 0) {
            return @{ ExitCode = 3; Output = @("sndinfo: 声卡 $id 不存在") }
        }
    }

    # 默认动作为 -l
    $modeList = $o.ContainsKey('l') -or $o.ContainsKey('p') -or $o.ContainsKey('c') -or
                $o.ContainsKey('j') -or $o.ContainsKey('s') -or $o.ContainsKey('w')
    if (-not $modeList) { $o['l'] = $true }

    # -q 只保留退出码
    if ($o.ContainsKey('q')) {
        return @{ ExitCode = 0; Output = @() }
    }

    $watchRounds = if ($o.ContainsKey('w')) { 3 } else { 1 }
    for ($r = 0; $r -lt $watchRounds; $r++) {
        if ($watchRounds -gt 1) {
            if ($r -gt 0) { $out += '' }
            $out += "[watch] 刷新 $($r+1)/$watchRounds  (原型演示：真实实现持续刷新至 Ctrl+C)"
        }
        foreach ($c in $cards) {
            if ($o.ContainsKey('j'))        { $out += (Render-JsonMode -Cards @($c)) }
            elseif ($o.ContainsKey('s'))    { $out += (Render-ShortMode -Card $c) }
            else {
                if ($o.ContainsKey('l'))    { $out += (Render-ListMode -Card $c) }
                if ($o.ContainsKey('p'))    { if ($out.Count) { $out += '' }; $out += (Render-PcmMode -Card $c) }
                if ($o.ContainsKey('c'))    { if ($out.Count) { $out += '' }; $out += (Render-CtrlMode -Card $c) }
            }
        }
    }

    if ($o.ContainsKey('t')) {
        try { [Console]::Beep(880, 120) } catch { }
        $out += '[tone] 测试音 880Hz/120ms 已播放'
    }

    return @{ ExitCode = 0; Output = $out }
}

# 以脚本方式执行时：输出文本并按退出码退出
if ($MyInvocation.InvocationName -ne '.') {
    $r = Invoke-SndInfo -Argv $args
    foreach ($line in $r.Output) { Write-Output $line }
    exit $r.ExitCode
}
