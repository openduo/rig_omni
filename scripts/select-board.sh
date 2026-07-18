#!/usr/bin/env bash
# 切换 RIG-Omni 板型(Puppy / Hover)
#
# 用法:
#   scripts/select-board.sh hover    # 切到 Hover
#   scripts/select-board.sh puppy    # 切到 Puppy
#   scripts/select-board.sh          # 显示当前板型
#
# 原理:
#   Kconfig 的 choice 不能用 -DCONFIG_XXX=y 传给 cmake(会被忽略 + 警告)
#   必须直接改 sdkconfig 文件,再 reconfigure
#   本脚本封装这个正确流程,避免踩坑
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDKCONFIG="${PROJECT_ROOT}/sdkconfig"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; NC=$'\033[0m'
err() { echo "${RED}[✗]${NC} $*" >&2; }

# 显示当前板型
show_current() {
    if [[ ! -f "${SDKCONFIG}" ]]; then
        echo "未配置(先跑 idf.py set-target esp32s3)"
        return
    fi
    if grep -q "^CONFIG_BOARD_TYPE_HOVER=y" "${SDKCONFIG}"; then
        echo "hover"
    elif grep -q "^CONFIG_BOARD_TYPE_PUPPY=y" "${SDKCONFIG}"; then
        echo "puppy"
    else
        echo "未知"
    fi
}

# 切换板型
switch_to() {
    local target="$1"
    local target_cfg other_cfg
    case "${target}" in
        hover)  target_cfg="CONFIG_BOARD_TYPE_HOVER=y";  other_cfg="CONFIG_BOARD_TYPE_PUPPY" ;;
        puppy)  target_cfg="CONFIG_BOARD_TYPE_PUPPY=y";  other_cfg="CONFIG_BOARD_TYPE_HOVER"  ;;
        *) err "未知板型: ${target}(只支持 hover / puppy)"; exit 1 ;;
    esac

    if [[ ! -f "${SDKCONFIG}" ]]; then
        err "sdkconfig 不存在,请先运行:idf.py set-target esp32s3"
        exit 1
    fi

    local current
    current=$(show_current)
    if [[ "${current}" == "${target}" ]]; then
        echo "${GREEN}[✓]${NC} 已经是 ${target},无需切换"
        return
    fi

    echo "${GREEN}[i]${NC} 切换板型: ${current} → ${target}"

    # 先关掉另一个(Kconfig choice 格式:"# CONFIG_XXX is not set")
    sed -i.bak "s/^${other_cfg}=y/# ${other_cfg} is not set/" "${SDKCONFIG}"
    # 再开目标(可能是 "# CONFIG_XXX is not set" 或不存在)
    if grep -q "^# ${target_cfg%=*} is not set" "${SDKCONFIG}"; then
        sed -i.bak "s/^# ${target_cfg%=*} is not set/${target_cfg}/" "${SDKCONFIG}"
    elif grep -q "^${target_cfg%=*}=" "${SDKCONFIG}"; then
        sed -i.bak "s|^${target_cfg%=*}=.*|${target_cfg}|" "${SDKCONFIG}"
    else
        echo "${target_cfg}" >> "${SDKCONFIG}"
    fi
    rm -f "${SDKCONFIG}.bak"

    echo "${GREEN}[✓]${NC} sdkconfig 已更新,当前板型: $(show_current)"
    echo "${YELLOW}[!]${NC} 下一步:idf.py reconfigure && idf.py build"
}

# 主逻辑
case "${1:-}" in
    hover|puppy) switch_to "$1" ;;
    "")          echo "当前板型: $(show_current)"
                 echo "用法: $0 <hover|puppy>" ;;
    *)           err "未知参数: $1"; echo "用法: $0 <hover|puppy>"; exit 1 ;;
esac
