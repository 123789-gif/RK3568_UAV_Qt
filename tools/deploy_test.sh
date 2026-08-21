#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/../rk_dual_screen_video.pro" ]]; then
    PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
elif [[ -f "$SCRIPT_DIR/rk_dual_screen_video.pro" ]]; then
    PROJECT_ROOT="$SCRIPT_DIR"
else
    echo "ERROR: deploy_test.sh 必须放在工程 tools/ 目录中运行。" >&2
    exit 1
fi

BUILD_ROOT="${BUILD_ROOT:-$PROJECT_ROOT/out/arm64-release}"
BINARY="${BINARY:-$BUILD_ROOT/rk_dual_screen_video}"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/dist}"
BOARD_TARGET="${BOARD_TARGET:-root@192.168.1.100}"
REMOTE_APPS_ROOT="${REMOTE_APPS_ROOT:-/userdata/Apps}"
EXPECTED_MARKER="${EXPECTED_MARKER:-}"
KEEP_REMOTE_RELEASES="${KEEP_REMOTE_RELEASES:-3}"
START_AFTER_DEPLOY=1
SSH_OPTIONS=(
    -o BatchMode=yes
    -o ConnectTimeout=10
    -o ServerAliveInterval=15
    -o ServerAliveCountMax=3
)

usage() {
    cat <<'EOF'
用法：
  ./tools/deploy_test.sh [选项]

选项：
  --board <user@ip>   板端 SSH 地址，默认 root@192.168.1.100
  --binary <path>     ARM64 可执行文件，默认 out/arm64-release/rk_dual_screen_video
  --keep-releases <N> 板端保留最新 N 个测试版本，默认 3
  --no-start          只上传、校验并解压，不停止旧程序、不启动新程序
  -h, --help          显示帮助

可选环境变量：
  EXPECTED_MARKER     要求 ELF 中必须存在的 UTF-16 字符串，例如 BUILD-CHECK-05
  BUILD_ROOT          Qt 构建目录
  OUTPUT_DIR          本地发布目录
  REMOTE_APPS_ROOT    板端应用根目录
  KEEP_REMOTE_RELEASES
                      板端保留的最新测试版本数量，默认 3

标准用法：
  Qt Creator 完成“重新构建”后执行：
    ./tools/deploy_test.sh

首次标题验证：
    EXPECTED_MARKER=BUILD-CHECK-05 ./tools/deploy_test.sh
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --board)
        [[ $# -ge 2 ]] || {
            echo "ERROR: --board 缺少参数。" >&2
            exit 2
        }
        BOARD_TARGET="$2"
        shift 2
        ;;
    --binary)
        [[ $# -ge 2 ]] || {
            echo "ERROR: --binary 缺少参数。" >&2
            exit 2
        }
        BINARY="$2"
        shift 2
        ;;
    --keep-releases)
        [[ $# -ge 2 ]] || {
            echo "ERROR: --keep-releases 缺少参数。" >&2
            exit 2
        }
        KEEP_REMOTE_RELEASES="$2"
        shift 2
        ;;
    --no-start)
        START_AFTER_DEPLOY=0
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "ERROR: 未知参数：$1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

for command_name in file gzip scp sha256sum ssh strings; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "ERROR: 缺少命令：$command_name" >&2
        exit 1
    }
done

if [[ ! "$KEEP_REMOTE_RELEASES" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: KEEP_REMOTE_RELEASES/--keep-releases 必须是大于 0 的整数。" >&2
    exit 2
fi

if ! ssh "${SSH_OPTIONS[@]}" "$BOARD_TARGET" true; then
    echo "ERROR: 无法通过 SSH 密钥非交互登录：$BOARD_TARGET" >&2
    echo "请先执行 ssh-copy-id，将虚拟机公钥安装到板端；不要把密码写入本脚本。" >&2
    exit 1
fi

[[ -x "$BINARY" ]] || {
    echo "ERROR: 找不到 Qt 构建产物：$BINARY" >&2
    echo "请先在 Qt Creator 中重新构建，或通过 --binary 指定实际 ELF。" >&2
    exit 1
}

FILE_INFO="$(file "$BINARY")"
if ! grep -Fq "ARM aarch64" <<<"$FILE_INFO"; then
    echo "ERROR: 构建产物不是 ARM aarch64：" >&2
    echo "$FILE_INFO" >&2
    exit 1
fi

NEWER_SOURCE="$(find "$PROJECT_ROOT/src" \
    -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.ui' \) \
    -newer "$BINARY" \
    -print \
    -quit)"

if [[ -n "$NEWER_SOURCE" || "$PROJECT_ROOT/rk_dual_screen_video.pro" -nt "$BINARY" ]]; then
    echo "ERROR: 存在比 ELF 更新的源码，拒绝部署旧构建产物。" >&2
    if [[ -n "$NEWER_SOURCE" ]]; then
        echo "较新的源码：$NEWER_SOURCE" >&2
    fi
    if [[ "$PROJECT_ROOT/rk_dual_screen_video.pro" -nt "$BINARY" ]]; then
        echo "较新的工程文件：$PROJECT_ROOT/rk_dual_screen_video.pro" >&2
    fi
    echo "请在 Qt Creator 中重新构建后再执行本脚本。" >&2
    exit 1
fi

if [[ -n "$EXPECTED_MARKER" ]]; then
    MARKER_MATCH="$(strings -el "$BINARY" |
        grep -F -- "$EXPECTED_MARKER" || true)"
    if [[ -z "$MARKER_MATCH" ]]; then
        echo "ERROR: ELF 中未找到标记：$EXPECTED_MARKER" >&2
        echo "当前 ELF 很可能不是刚才修改源码后生成的版本。" >&2
        exit 1
    fi
fi

echo "[1/5] ARM64 ELF 检查通过"
echo "      $BINARY"

mkdir -p "$OUTPUT_DIR"
"$PROJECT_ROOT/deploy/make_bundle.sh" "$BINARY" "$OUTPUT_DIR"

PACKAGE="$(ls -1t "$OUTPUT_DIR"/RK3568_PTZ_Qt_*.tar.gz 2>/dev/null |
    head -n 1)"
[[ -n "$PACKAGE" && -f "$PACKAGE" ]] || {
    echo "ERROR: 未找到新发布包。" >&2
    exit 1
}
[[ -f "$PACKAGE.sha256" ]] || {
    echo "ERROR: 缺少发布包 SHA256：$PACKAGE.sha256" >&2
    exit 1
}

gzip -t "$PACKAGE"
(
    cd "$OUTPUT_DIR/RK3568_PTZ_Qt"
    sha256sum -c MANIFEST.sha256 >/dev/null
)

echo "[2/5] 发布包生成并完成本地校验"
echo "      $PACKAGE"

PACKAGE_NAME="$(basename "$PACKAGE")"
scp "${SSH_OPTIONS[@]}" \
    "$PACKAGE" "$PACKAGE.sha256" "$BOARD_TARGET:/tmp/"

echo "[3/5] 发布包已上传到 $BOARD_TARGET:/tmp/"

REMOTE_PACKAGE="/tmp/$PACKAGE_NAME"
ssh "${SSH_OPTIONS[@]}" "$BOARD_TARGET" \
    "bash -s -- '$REMOTE_PACKAGE' '$REMOTE_APPS_ROOT' '$START_AFTER_DEPLOY' '$KEEP_REMOTE_RELEASES'" <<'REMOTE_SCRIPT'
set -euo pipefail

package="$1"
apps_root="${2%/}"
start_after_deploy="$3"
keep_remote_releases="$4"
package_name="$(basename "$package")"

if [[ ! "$keep_remote_releases" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: 板端版本保留数量无效：$keep_remote_releases" >&2
    exit 2
fi

[[ -f "$package" ]] || {
    echo "ERROR: 板端找不到发布包：$package" >&2
    exit 1
}
[[ -f "${package}.sha256" ]] || {
    echo "ERROR: 板端缺少发布包 SHA256：${package}.sha256" >&2
    exit 1
}

cd "$(dirname "$package")"
sha256sum -c "$(basename "${package}.sha256")"
gzip -t "$package"

version="${package_name#RK3568_PTZ_Qt_}"
version="${version%.tar.gz}"
new_root="$apps_root/RK3568_PTZ_Qt_test_$version"

if [[ -e "$new_root" ]]; then
    echo "ERROR: 测试目录已存在，拒绝混合覆盖：$new_root" >&2
    exit 1
fi

mkdir -p "$new_root"
tar -xzf "$package" -C "$new_root" --strip-components=1
sync

(
    cd "$new_root"
    sha256sum -c MANIFEST.sha256
)

echo "[4/5] 板端发布目录校验通过"
echo "      $new_root"

if [[ "$start_after_deploy" != "1" ]]; then
    rm -f -- "$package" "${package}.sha256"
    echo "[5/5] 已按 --no-start 完成部署，未切换运行程序。"
    echo "手动启动：$new_root/scripts/start.sh --foreground"
    exit 0
fi

current_state="$apps_root/RK3568_PTZ_Qt_test_current.path"
official_root="$apps_root/RK3568_PTZ_Qt"

find_app_processes() {
    local proc_exe=""
    local candidate_pid=""
    local candidate_exe=""

    for proc_exe in /proc/[0-9]*/exe; do
        [[ -e "$proc_exe" ]] || continue
        candidate_pid="${proc_exe#/proc/}"
        candidate_pid="${candidate_pid%/exe}"
        candidate_exe="$(readlink -f "$proc_exe" 2>/dev/null || true)"
        if [[ "$candidate_exe" == */bin/rk_dual_screen_video ]]; then
            printf '%s|%s\n' "$candidate_pid" "$candidate_exe"
        fi
    done
}

find_pid_for_exe() {
    local expected_exe="$1"
    local record=""
    local candidate_pid=""
    local candidate_exe=""

    while IFS= read -r record; do
        [[ -n "$record" ]] || continue
        candidate_pid="${record%%|*}"
        candidate_exe="${record#*|}"
        if [[ "$candidate_exe" == "$expected_exe" ]]; then
            printf '%s\n' "$candidate_pid"
            return 0
        fi
    done < <(find_app_processes)
    return 1
}

is_test_root() {
    local candidate_root="$1"
    local candidate_parent=""
    local candidate_name=""

    candidate_parent="$(dirname "$candidate_root")"
    candidate_name="$(basename "$candidate_root")"
    [[ "$candidate_parent" == "$apps_root" &&
       "$candidate_name" == RK3568_PTZ_Qt_test_20??????T??????Z ]]
}

mapfile -t active_records < <(find_app_processes)
if ((${#active_records[@]} > 1)); then
    echo "ERROR: 检测到多个视频程序，部署前必须人工清理，避免误判回滚目标：" >&2
    printf '  %s\n' "${active_records[@]}" >&2
    exit 1
fi

rollback_root=""
if ((${#active_records[@]} == 1)); then
    active_exe="${active_records[0]#*|}"
    rollback_root="$(dirname "$(dirname "$active_exe")")"
    if [[ "$rollback_root" != "$official_root" ]] && ! is_test_root "$rollback_root"; then
        echo "ERROR: 当前进程不属于受管正式/测试目录，拒绝自动停止：$active_exe" >&2
        exit 1
    fi
fi

switch_started=0
switch_committed=0

rollback_on_exit() {
    local status=$?
    local rollback_exe=""
    local rollback_pid=""
    local state_tmp=""
    local remaining_records=()

    trap - EXIT
    if ((status == 0 || switch_started == 0 || switch_committed == 1)); then
        exit "$status"
    fi

    set +e
    echo "WARNING: 新版本切换失败，开始自动回滚。" >&2
    if [[ -x "$new_root/scripts/stop.sh" ]]; then
        "$new_root/scripts/stop.sh" </dev/null >&2
    fi

    mapfile -t remaining_records < <(find_app_processes)
    if ((${#remaining_records[@]} > 0)); then
        echo "ERROR: 仍有视频进程，无法安全恢复旧版本：" >&2
        printf '  %s\n' "${remaining_records[@]}" >&2
        exit "$status"
    fi

    if [[ -n "$rollback_root" && -x "$rollback_root/scripts/start.sh" ]]; then
        echo "      恢复：$rollback_root" >&2
        "$rollback_root/scripts/start.sh" </dev/null >&2
        rollback_exe="$rollback_root/bin/rk_dual_screen_video"
        for ((attempt = 1; attempt <= 10; attempt++)); do
            rollback_pid="$(find_pid_for_exe "$rollback_exe" || true)"
            [[ -n "$rollback_pid" ]] && break
            sleep 1
        done

        if [[ -n "$rollback_pid" ]]; then
            if is_test_root "$rollback_root"; then
                state_tmp="${current_state}.tmp.$$"
                printf '%s\n' "$rollback_root" > "$state_tmp"
                mv -f "$state_tmp" "$current_state"
            elif [[ "$rollback_root" == "$official_root" ]]; then
                rm -f "$current_state"
            fi
            echo "      回滚成功，PID：$rollback_pid" >&2
        else
            echo "ERROR: 旧版本启动命令已执行，但未检测到目标 ELF。" >&2
        fi
    else
        echo "      切换前没有运行中的受管版本，无可恢复进程。" >&2
    fi

    exit "$status"
}

trap rollback_on_exit EXIT
switch_started=1

if [[ -n "$rollback_root" ]]; then
    [[ -x "$rollback_root/scripts/stop.sh" ]] || {
        echo "ERROR: 旧版本缺少停止脚本：$rollback_root/scripts/stop.sh" >&2
        exit 1
    }
    "$rollback_root/scripts/stop.sh" </dev/null
fi

mapfile -t active_records < <(find_app_processes)
if ((${#active_records[@]} > 0)); then
    echo "ERROR: 仍有 rk_dual_screen_video 进程，拒绝并行占用 DRM/PCIe：" >&2
    printf '  %s\n' "${active_records[@]}" >&2
    exit 1
fi

# 非交互SSH会把本脚本的here-doc作为stdin。显式切到/dev/null，
# 避免Qt后台进程继承SSH输入通道，导致部署已经完成但ssh一直不退出。
if ! "$new_root/scripts/start.sh" </dev/null; then
    echo "ERROR: 新测试版本启动失败。" >&2
    exit 1
fi

expected_exe="$new_root/bin/rk_dual_screen_video"
running_pid=""
for ((attempt = 1; attempt <= 10; attempt++)); do
    running_pid="$(find_pid_for_exe "$expected_exe" || true)"
    [[ -n "$running_pid" ]] && break
    sleep 1
done

if [[ -z "$running_pid" ]]; then
    echo "ERROR: 启动脚本已返回，但未检测到新版本进程：$expected_exe" >&2
    if [[ -f "$new_root/logs/rk_dual_screen_video.log" ]]; then
        echo "----- 最近运行日志 -----" >&2
        tail -n 40 "$new_root/logs/rk_dual_screen_video.log" >&2 || true
    fi
    exit 1
fi

state_tmp="${current_state}.tmp.$$"
printf '%s\n' "$new_root" > "$state_tmp"
mv -f "$state_tmp" "$current_state"
switch_committed=1

echo "[5/5] 新测试版本已在后台启动"
echo "      目录：$new_root"
echo "      PID：$running_pid"
echo "      日志：$new_root/logs/rk_dual_screen_video.log"
echo "      停止：$new_root/scripts/stop.sh"
echo "      前台重启：$new_root/scripts/stop.sh && $new_root/scripts/start.sh --foreground"

mapfile -t release_dirs < <(
    find "$apps_root" \
        -mindepth 1 \
        -maxdepth 1 \
        -type d \
        -name 'RK3568_PTZ_Qt_test_20*T*Z' \
        -print |
        sort -r
)

release_index=0
for release_dir in "${release_dirs[@]}"; do
    release_index=$((release_index + 1))

    if ((release_index <= keep_remote_releases)); then
        echo "      保留：$release_dir"
        continue
    fi

    release_real="$(readlink -f "$release_dir" 2>/dev/null || true)"
    [[ -n "$release_real" ]] || continue

    if [[ "$release_real" == "$new_root" ]]; then
        echo "      保留当前版本：$release_real"
        continue
    fi

    release_active=0
    if find_pid_for_exe "$release_real/bin/rk_dual_screen_video" >/dev/null; then
        release_active=1
    fi

    if ((release_active == 1)); then
        echo "      保留运行中版本：$release_real"
        continue
    fi

    case "$release_real" in
    "$apps_root"/RK3568_PTZ_Qt_test_20??????T??????Z)
        if rm -rf --one-file-system -- "$release_real"; then
            echo "      已清理：$release_real"
        else
            echo "WARNING: 旧测试版本清理失败：$release_real" >&2
        fi
        ;;
    *)
        echo "WARNING: 路径不符合版本目录规则，跳过清理：$release_real" >&2
        ;;
    esac
done

rm -f -- "$package" "${package}.sha256"
sync

echo "      自动清理完成：保留最新 $keep_remote_releases 个测试版本，并保护当前/运行版本"
REMOTE_SCRIPT

echo
echo "部署完成。"
echo "板端正式目录没有被覆盖；本次运行的是新的版本化测试目录。"
