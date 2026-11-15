#!/usr/bin/env bash
# ============================================================================
#  check.sh —— 边缘人脸考勤终端 现场自检脚本
#
#  用法：
#      cd ~/face_punchin
#      bash check.sh            # 只读检查，不编译、不启动摄像头
#      bash check.sh --run      # 额外试跑 6 秒（需要图形界面 DISPLAY）
#
#  脚本只做“读”操作：不改代码、不改数据库、不安装任何东西。
#  如果执行时报 $'\r': command not found，说明文件被转成了 CRLF：
#      sed -i 's/\r$//' check.sh
# ============================================================================
set -u

cd "$(dirname "$0")" 2>/dev/null || cd .

line() { printf '\n===== %s =====\n' "$1"; }

line "1. 系统与工具链"
date
uname -srm
[ -r /etc/os-release ] && . /etc/os-release && echo "发行版: ${PRETTY_NAME:-unknown}"
echo "g++  : $(g++ --version 2>/dev/null | head -1 || echo '未安装')"
echo "cmake: $(cmake --version 2>/dev/null | head -1 || echo '未安装')"
echo "gcc 对 C++17 的支持: $(echo 'int main(){return 0;}' | g++ -std=c++17 -x c++ -fsyntax-only - 2>/dev/null && echo OK || echo FAIL)"

line "2. 依赖库"
if pkg-config --modversion opencv4 >/dev/null 2>&1; then
    echo "OpenCV: $(pkg-config --modversion opencv4)"
else
    echo "OpenCV: pkg-config 找不到 opencv4.pc（可能在 /usr/local/lib/pkgconfig）"
fi
[ -f /usr/local/include/mqtt/async_client.h ] \
    && echo "Paho C++ 头文件: 存在" || echo "Paho C++ 头文件: 缺失 /usr/local/include/mqtt/async_client.h"
ls -1 /usr/local/lib/libpaho-mqtt* 2>/dev/null || echo "Paho 库: /usr/local/lib 下没有"
if [ -f /usr/local/include/mqtt/event.h ]; then
    if grep -q 'std::variant' /usr/local/include/mqtt/event.h 2>/dev/null; then
        echo "Paho 版本: 2.x（需要 C++17）"
    else
        echo "Paho 版本: 1.3.x（C++14 即可）"
    fi
fi
[ -f /usr/include/sqlite3.h ] && echo "sqlite3.h: 存在" || echo "sqlite3.h: 缺失"

line "3. 工程与编译产物"
pwd
ls -1 CMakeLists.txt main.cpp DBManager.cpp DBManager.h 2>/dev/null
grep -n 'CMAKE_CXX_STANDARD' CMakeLists.txt 2>/dev/null | head -3
echo "-- 构建目录 --"
ls -ld build build2 2>/dev/null
echo "-- 可执行文件 --"
ls -l --time-style=long-iso build*/face_attendance 2>/dev/null || echo "没找到 build*/face_attendance"
echo "-- 二进制里的关键字符串（判断是不是新程序）--"
for b in build*/face_attendance; do
    [ -f "$b" ] || continue
    echo "[$b]"
    strings "$b" 2>/dev/null | grep -E 'Face Attendance Terminal|NO FACE MATCHED|faces:%d|terminal/register|attendance/checkin' | sort -u
done

line "4. 正在运行的进程"
ps -ef | grep -E 'face_attendance|/fr( |$)' | grep -v grep || echo "没有相关进程在跑"
for p in $(pgrep -f face_attendance 2>/dev/null); do
    echo "PID $p -> $(readlink -f /proc/$p/exe 2>/dev/null)"
done

line "5. 动态库链接情况"
for b in build*/face_attendance; do
    [ -f "$b" ] || continue
    echo "[$b]"
    ldd "$b" 2>/dev/null | grep -E 'paho|opencv|sqlite|not found' || true
done

line "6. 数据库内容"
shopt -s nullglob
found=0
for db in attendance.db build/attendance.db build2/attendance.db; do
    [ -f "$db" ] || continue
    found=1
    echo "-- $db ($(stat -c '%s' "$db" 2>/dev/null) 字节, 修改于 $(stat -c '%y' "$db" 2>/dev/null | cut -c1-19))"
    if command -v sqlite3 >/dev/null 2>&1; then
        sqlite3 "$db" "select 'Person 表人数 = '||count(*) from Person;" 2>/dev/null
        sqlite3 -separator ' | ' "$db" "select id,name,length(feature) from Person limit 5;" 2>/dev/null
        sqlite3 "$db" "select 'Attendance 表条数 = '||count(*) from Attendance;" 2>/dev/null
        sqlite3 -separator ' | ' "$db" "select name,check_time,printf('%.3f',score) from Attendance order by id desc limit 5;" 2>/dev/null
    else
        echo "（没有 sqlite3 命令行工具，跳过；可 sudo apt install sqlite3）"
    fi
done
[ "$found" -eq 0 ] && echo "当前目录及 build*/ 下没有 attendance.db（程序还没成功打开过数据库？）"

line "7. 摄像头与显示环境"
ls -l /dev/video* 2>/dev/null || echo "没有 /dev/video* 设备"
if id -nG 2>/dev/null | tr ' ' '\n' | grep -qx video; then
    echo "当前用户在 video 组: 是"
else
    echo "当前用户在 video 组: 否（打不开摄像头时可 sudo usermod -aG video \$USER）"
fi
echo "DISPLAY=${DISPLAY:-<未设置>}  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-<未设置>}"

line "8. 模型与字库"
ls -l models/ 2>/dev/null || echo "没有 models/ 目录"
[ -f resources/HZK16 ] && ls -lh resources/HZK16 || echo "没有 resources/HZK16（可选，只影响画面上的中文姓名）"

if [ "${1:-}" = "--run" ]; then
    line "9. 试跑 6 秒（输出前 40 行）"
    bin=$(ls -1 build*/face_attendance 2>/dev/null | head -1)
    if [ -z "$bin" ]; then
        echo "没有可执行文件，先编译：cmake -B build2 -S . && cmake --build build2 -j\$(nproc)"
    elif [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
        echo "没有图形环境（DISPLAY 为空），跳过试跑"
    else
        echo "启动: $bin"
        timeout 6 "$bin" 2>&1 | head -40
        echo "（已超时结束，返回码 $?）"
    fi
fi

line "完成"
echo "把上面整段输出贴给我即可（尤其是 3、4、6 三节）。"
