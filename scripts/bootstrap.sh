#!/usr/bin/env bash
# rig_omni 开发环境引导脚本
#
# 用途:检查并引导安装 ESP-IDF v5.5.2 + direnv,让 clone 下来即可构建
#
# 用法:
#   scripts/bootstrap.sh           # 检查 + 引导
#   scripts/bootstrap.sh --check   # 只检查,不引导
#
# 设计原则:
#   - 不污染系统:所有东西装在 ~/esp 和 ~/.espressif,不碰 /opt 或系统 Python
#   - 可重入:已装的部分会跳过
#   - 失败可恢复:每步都有清晰错误提示
set -euo pipefail

# ===== 配置 =====
IDF_VERSION="v5.5.2"
IDF_TARGET="esp32s3"
IDF_DIR="${HOME}/esp/esp-idf-${IDF_VERSION}"
IDF_TOOLS_PATH="${HOME}/.espressif"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ===== 颜色 =====
RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
BLUE=$'\033[0;34m'; NC=$'\033[0m'
info()  { echo "${BLUE}[i]${NC} $*"; }
ok()    { echo "${GREEN}[✓]${NC} $*"; }
warn()  { echo "${YELLOW}[!]${NC} $*"; }
err()   { echo "${RED}[✗]${NC} $*" >&2; }
die()   { err "$*"; exit 1; }

CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

# ===== 1. 系统级依赖(必须 Homebrew,无法工程化) =====
check_system_deps() {
    info "检查系统级依赖(需 Homebrew 提供)..."
    local missing=()
    for tool in git cmake ninja; do
        if command -v "$tool" >/dev/null 2>&1; then
            ok "$tool: $(command -v "$tool")"
        else
            missing+=("$tool")
        fi
    done
    # Python:IDF v5.5.2 需要 3.8-3.12,系统默认 3.14 太新
    if [[ -x /opt/homebrew/bin/python3.12 ]]; then
        ok "python3.12: $(/opt/homebrew/bin/python3.12 --version)"
    elif [[ -x /opt/homebrew/bin/python3.11 ]]; then
        ok "python3.11: $(/opt/homebrew/bin/python3.11 --version)"
    else
        missing+=("python@3.12 (brew)")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "缺失系统依赖: ${missing[*]}"
        if [[ $CHECK_ONLY -eq 0 ]]; then
            info "运行以下命令安装:"
            echo "    brew install git cmake ninja dfu-util python@3.12 direnv"
            return 1
        fi
    fi
}

# ===== 2. direnv(进目录自动激活) =====
check_direnv() {
    info "检查 direnv..."
    if command -v direnv >/dev/null 2>&1; then
        ok "direnv: $(direnv --version)"
        # 检查 shell hook
        if ! grep -q 'direnv hook' ~/.zshrc 2>/dev/null; then
            warn "direnv 已装但未挂 zsh hook"
            if [[ $CHECK_ONLY -eq 0 ]]; then
                info '请在 ~/.zshrc 末尾添加:  eval "$(direnv hook zsh)"'
            fi
        fi
    else
        warn "direnv 未安装"
        if [[ $CHECK_ONLY -eq 0 ]]; then
            info "  brew install direnv"
            info "  并在 ~/.zshrc 加:  eval \"\$(direnv hook zsh)\""
        fi
    fi
}

# ===== 3. ESP-IDF v5.5.2 =====
check_idf() {
    info "检查 ESP-IDF ${IDF_VERSION}..."
    if [[ -f "${IDF_DIR}/export.sh" ]]; then
        local ver
        ver=$(cd "${IDF_DIR}" && git describe --tags 2>/dev/null || echo "unknown")
        ok "ESP-IDF: ${IDF_DIR} (${ver})"
        # 检查工具链是否装了(用 ls 判断,glob 在 [[ ]] 里不展开)
        if ls "${IDF_TOOLS_PATH}/python_env/idf5.5_py"*"_env" >/dev/null 2>&1; then
            ok "Python venv: 已创建"
            # venv 在,但要检查工程 Python 依赖
            if [[ $CHECK_ONLY -eq 0 ]]; then
                install_project_python_deps
            fi
        else
            warn "IDF 源码在,但工具链/venv 未安装"
            if [[ $CHECK_ONLY -eq 0 ]]; then
                install_idf_tools
            fi
        fi
    else
        warn "ESP-IDF ${IDF_VERSION} 未安装"
        if [[ $CHECK_ONLY -eq 0 ]]; then
            install_idf
        fi
    fi
}

install_idf() {
    info "开始安装 ESP-IDF ${IDF_VERSION} 到 ${IDF_DIR}..."
    mkdir -p "${HOME}/esp"
    if [[ ! -d "${IDF_DIR}" ]]; then
        info "clone esp-idf(含 submodule,可能需几分钟)..."
        git clone -b "${IDF_VERSION}" --recursive https://github.com/espressif/esp-idf.git "${IDF_DIR}"
        ok "clone 完成"
    fi
    install_idf_tools
}

install_idf_tools() {
    info "安装 ESP-IDF 工具链(esp32s3)+ Python venv..."
    info "这会下载约 1-2GB,请耐心等待"
    export IDF_TOOLS_PATH
    bash "${IDF_DIR}/install.sh" "${IDF_TARGET}"
    # install.sh 可能漏装 gdb(esp32s3 的 ULP 协处理器需要 riscv gdb)
    if ! bash "${IDF_DIR}/export.sh" >/dev/null 2>&1; then
        info "补装缺失的 gdb 工具..."
        "${IDF_TOOLS_PATH}/python_env"/idf*_py*/bin/python "${IDF_DIR}/tools/idf_tools.py" \
            install riscv32-esp-elf-gdb xtensa-esp-elf-gdb 2>/dev/null || true
    fi
    ok "工具链安装完成"
    install_project_python_deps
}

# ===== 3.5 工程内 Python 依赖(构建脚本需要,装进 IDF venv)=====
install_project_python_deps() {
    info "检查工程 Python 依赖(requirements.txt)..."
    local venv_python
    venv_python=$(ls "${IDF_TOOLS_PATH}/python_env"/idf*_py*/bin/python 2>/dev/null | head -1)
    if [[ -z "${venv_python}" ]]; then
        warn "找不到 IDF venv python,跳过依赖安装"
        return
    fi
    if [[ -f "${PROJECT_ROOT}/requirements.txt" ]]; then
        # 只在缺包时装,避免每次都跑 pip
        if ! "${venv_python}" -c "import numpy" >/dev/null 2>&1; then
            info "安装工程 Python 依赖到 IDF venv..."
            "${venv_python}" -m pip install -r "${PROJECT_ROOT}/requirements.txt" >/dev/null 2>&1 \
                && ok "Python 依赖已安装" \
                || warn "Python 依赖安装失败,可能需要手动: ${venv_python} -m pip install -r requirements.txt"
        else
            ok "Python 依赖已就绪"
        fi
    fi
}

# ===== 4. 工程内 .envrc 是否已 allow =====
check_direnv_allow() {
    if [[ -f "${PROJECT_ROOT}/.envrc" ]] && command -v direnv >/dev/null 2>&1; then
        if ! direnv status 2>/dev/null | grep -q "${PROJECT_ROOT}.*allowed"; then
            warn ".envrc 尚未 direnv allow"
            if [[ $CHECK_ONLY -eq 0 ]]; then
                info "进入工程目录后运行:  direnv allow"
            fi
        else
            ok ".envrc 已 allow"
        fi
    fi
}

# ===== 主流程 =====
main() {
    echo "${BLUE}════════════════════════════════════════════${NC}"
    echo "${BLUE} rig_omni 开发环境引导${NC}"
    echo "${BLUE}════════════════════════════════════════════${NC}"
    echo ""
    check_system_deps || true
    echo ""
    check_direnv || true
    echo ""
    check_idf
    echo ""
    check_direnv_allow || true
    echo ""
    echo "${GREEN}════════════════════════════════════════════${NC}"
    if [[ $CHECK_ONLY -eq 1 ]]; then
        echo "${GREEN} 检查完成${NC}"
    else
        echo "${GREEN} 引导完成。下一步:${NC}"
        echo "${GREEN}   1. cd ${PROJECT_ROOT}${NC}"
        echo "${GREEN}   2. direnv allow                # 激活 ESP-IDF 环境(自动)${NC}"
        echo "${GREEN}   3. idf.py set-target esp32s3   # 生成 sdkconfig${NC}"
        echo "${GREEN}   4. scripts/select-board.sh hover  # 选板型(默认 puppy)${NC}"
        echo "${GREEN}   5. idf.py build${NC}"
    fi
    echo "${GREEN}════════════════════════════════════════════${NC}"
}

main "$@"
