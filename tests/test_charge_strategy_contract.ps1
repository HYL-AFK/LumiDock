$ErrorActionPreference = 'Stop'
$root = "$PSScriptRoot\.."
$charge = Get-Content -Raw -LiteralPath "$root\main\MID\system\charge_strategy.c"
$mcp = Get-Content -Raw -LiteralPath "$root\main\MID\system\mcp_service.c"

# 安全门限是硬约束，不是保守取整：0°C 以下充电会在负极析锂，是真的伤电池。
@(
    @{ Pattern = '#define CHARGE_TEMP_MIN_C 0'; Message = 'Charging at or below 0 C must stay blocked (lithium plating).' },
    @{ Pattern = '#define CHARGE_TEMP_MAX_C 45'; Message = 'Charging above 45 C must stay blocked.' },
    @{ Pattern = '#define CHARGE_RESERVE_SOC 15U'; Message = 'A reserve floor must stay, so the plan never counts the pack down to zero.' },
    @{ Pattern = '#define CHARGE_SAVING_MIN_CT'; Message = 'Deferring to a cheaper window must clear a minimum saving threshold.' }
) | ForEach-Object { if ($charge -notmatch $_.Pattern) { throw $_.Message } }

# 安全判定必须排在"够不够用"之前：温度本来就不安全的电池，再缺电也不能建议充。
if ($charge -notmatch '(?s)CHARGE_TEMP_MIN_C[\s\S]{0,3000}?const float need_wh') {
    throw 'The safety gate must run before the range calculation.'
}
# 已经掉到警戒线以下就别再等便宜电了，省几毛钱不值得让用户第二天推车。
if ($charge -notmatch '(?s)below_floor[\s\S]{0,600}?CHARGE_PLAN_CHARGE_NOW') {
    throw 'At or below the configured floor the plan must charge now instead of waiting for a cheaper hour.'
}
if ($charge -notmatch 'usage_factor' -or $charge -notmatch 'tomorrow_precip_prob') {
    throw 'Cold and rain must inflate tomorrow consumption, or winter days get under-planned.'
}
@(
    @{ Pattern = 'bms_service_get_info'; Message = 'The plan must read real battery data.' },
    @{ Pattern = 'weather_snapshot_get'; Message = 'The plan must read the forecast.' },
    @{ Pattern = 'tariff_service_find_cheapest'; Message = 'The plan must look for the cheapest window.' },
    @{ Pattern = 'time\(NULL\)'; Message = 'A window that already started must not be offered as a future one.' },
    @{ Pattern = '电池数据不可用'; Message = 'Without battery data the strategy must refuse to invent a plan.' }
) | ForEach-Object { if ($charge -notmatch $_.Pattern) { throw $_.Message } }

# 工具登记与分发
if ($mcp -notmatch '#define MCP_TOOL_CHARGE_PLAN "self\.charge\.get_plan"') {
    throw 'The charge tool name is missing.'
}
if ($mcp -notmatch 'static const char \*handle_charge_get_plan\(long id\)') {
    throw 'The charge tool handler is missing.'
}
if ($mcp -notmatch 'strcmp\(tool, MCP_TOOL_CHARGE_PLAN\)') {
    throw 'The charge tool is not dispatched.'
}
# 具体数字（电量、时长、省多少钱）模型看不到，让它自己组织语言必然说错。
if ($mcp -notmatch 'verbatim') {
    throw 'The tool description must tell the model to read the reason sentence as-is.'
}
# 读不到电池是用户能理解、也能自己处理的状态，不是工具故障：报 error 会让云端回一个
# 难过表情加道歉，UI 上看就像设备坏了（实测日志里就是这么走的）。
if ($mcp -notmatch '现在读不到电池数据') {
    throw 'A missing battery must answer with a speakable sentence, not a tool error.'
}
# 反向断言：电池缺失这条路径不许再回到 error 分支。真正读不到电池时返回什么，
# 决定用户听到的是"电池没接上"还是"抱歉我出错了"。
if ($mcp -match '(?s)charge_strategy_build_plan\(s_charge_plan\)\)[\s\S]{0,200}?reply_error') {
    throw 'The missing-battery path must not return a tool error.'
}

Write-Output 'Charge strategy contract passed.'
