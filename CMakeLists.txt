# ============================================================================
#  边缘人脸考勤终端 —— CMake 构建脚本
#
#  依赖：
#    - OpenCV 4.5.4（core / imgproc / highgui / videoio / dnn / objdetect）
#    - SQLite3
#    - Eclipse Paho MQTT C++（必须带 WebSocket 支持，见文件末尾说明）
#    - pthread；iconv（glibc 自带，用于把 UTF-8 姓名转成 GBK 画点阵汉字）
#
#  构建：
#    mkdir -p build && cd build
#    cmake .. && make -j4
#
#  Paho 必须以「带 WebSocket」的方式编译安装，否则 ws:// 连接会失败：
#    git clone https://github.com/eclipse/paho.mqtt.c.git
#    cd paho.mqtt.c && cmake -Bbuild -DPAHO_WITH_WEBSOCKETS=TRUE -DPAHO_BUILD_SHARED=TRUE
#    sudo cmake --build build --target install
#    git clone https://github.com/eclipse/paho.mqtt.cpp.git
#    cd paho.mqtt.cpp && cmake -Bbuild -DPAHO_BUILD_SHARED=TRUE \
#         -DPAHO_MQTT_C_PATH=/usr/local
#    sudo cmake --build build --target install && sudo ldconfig
# ============================================================================

cmake_minimum_required(VERSION 3.10)
project(face_attendance CXX)

# Paho MQTT C++ 2.x（git master 分支）内部用了 std::variant / std::string_view /
# std::holds_alternative，必须是 C++17；1.3.x 用 C++14 也能编。
# 统一按 C++17 编译，两代 Paho 都能过（g++ >= 7 均支持）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# 1) OpenCV：YuNet 检测与 SFace 识别都在 objdetect 模块里（4.5.4 起）
# ---------------------------------------------------------------------------
find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui videoio dnn objdetect)
message(STATUS "OpenCV 版本 : ${OpenCV_VERSION}")
message(STATUS "OpenCV 路径 : ${OpenCV_DIR}")

# ---------------------------------------------------------------------------
# 2) SQLite3：CMake 3.14+ 自带 FindSQLite3，会提供 SQLite3::SQLite3 导入目标
# ---------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
  set(SQLITE3_TARGET SQLite3::SQLite3)
else()
  set(SQLITE3_TARGET ${SQLite3_LIBRARIES})
endif()
message(STATUS "SQLite3     : ${SQLite3_LIBRARIES}")

# ---------------------------------------------------------------------------
# 3) Eclipse Paho MQTT C++（源码安装，默认前缀 /usr/local）
#    - C++ 库：paho-mqttpp3
#    - C   库：异步客户端要链接 paho-mqtt3as（WebSocket/SSL 版）；
#             若系统里只有同步版 paho-mqtt3cs 则退而求其次（需自行确认可用）
# ---------------------------------------------------------------------------
set(PAHO_PREFIX "/usr/local" CACHE PATH "Paho MQTT 的安装前缀")

set(PAHO_SEARCH_DIRS
    ${PAHO_PREFIX}/lib ${PAHO_PREFIX}/lib64
    /usr/local/lib /usr/local/lib64
    /usr/lib /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/lib/aarch64-linux-gnu
    /usr/lib/arm-linux-gnueabihf)

find_path(PAHO_MQTT_CXX_INCLUDE_DIR
  NAMES mqtt/async_client.h
  HINTS ${PAHO_PREFIX}/include /usr/local/include /usr/include)

find_library(PAHO_MQTT_CXX_LIB
  NAMES paho-mqttpp3
  HINTS ${PAHO_SEARCH_DIRS})

find_library(PAHO_MQTT_C_LIB
  NAMES paho-mqtt3as          # 异步 + SSL/WebSocket
  HINTS ${PAHO_SEARCH_DIRS})

if(NOT PAHO_MQTT_C_LIB)
  find_library(PAHO_MQTT_C_LIB
    NAMES paho-mqtt3cs        # 同步 + SSL/WebSocket（回退方案）
    HINTS ${PAHO_SEARCH_DIRS})
  if(PAHO_MQTT_C_LIB)
    message(WARNING "未找到 paho-mqtt3as，改用 paho-mqtt3cs，请确认它可以正常做异步连接")
  endif()
endif()

if(NOT PAHO_MQTT_CXX_INCLUDE_DIR OR NOT PAHO_MQTT_CXX_LIB OR NOT PAHO_MQTT_C_LIB)
  message(FATAL_ERROR
    "找不到 Paho MQTT C++ 库（或缺少 WebSocket 版本）。\n"
    "请确认已按下面的方式源码安装：\n"
    "  paho.mqtt.c : cmake -Bbuild -DPAHO_WITH_WEBSOCKETS=TRUE -DPAHO_BUILD_SHARED=TRUE\n"
    "  paho.mqtt.cpp: cmake -Bbuild -DPAHO_BUILD_SHARED=TRUE -DPAHO_MQTT_C_PATH=/usr/local\n"
    "如安装在自定义目录，请加 -DPAHO_PREFIX=/your/prefix")
endif()
message(STATUS "Paho C++    : ${PAHO_MQTT_CXX_LIB}")
message(STATUS "Paho C      : ${PAHO_MQTT_C_LIB}")

# ---------------------------------------------------------------------------
# 4) 线程 / iconv
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Iconv QUIET)

# ---------------------------------------------------------------------------
# 5) 可执行文件
# ---------------------------------------------------------------------------
add_executable(face_attendance
  main.cpp
  DBManager.cpp)

target_include_directories(face_attendance PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${OpenCV_INCLUDE_DIRS}
  ${PAHO_MQTT_CXX_INCLUDE_DIR}
  ${SQLite3_INCLUDE_DIRS})

target_link_libraries(face_attendance PRIVATE
  ${OpenCV_LIBS}
  ${SQLITE3_TARGET}
  ${PAHO_MQTT_CXX_LIB}
  ${PAHO_MQTT_C_LIB}
  Threads::Threads)

# Paho 的 ws/wss 传输依赖 OpenSSL；动态库通常已自动带上，静态库则需要显式链接
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_link_libraries(face_attendance PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# iconv：用于 UTF-8 -> GBK，把中文姓名用 HZK16 点阵字库画到 OpenCV 窗口上
if(Iconv_FOUND)
  target_compile_definitions(face_attendance PRIVATE HAVE_ICONV=1)
  target_link_libraries(face_attendance PRIVATE Iconv::Iconv)
  message(STATUS "iconv       : 可用（支持窗口显示中文姓名）")
else()
  target_compile_definitions(face_attendance PRIVATE HAVE_ICONV=0)
  message(WARNING "iconv 不可用：中文姓名只能显示在控制台")
endif()

# ---------------------------------------------------------------------------
# 6) 运行时提示（模型和字库都是运行时按相对路径查找的，不参与编译）
# ---------------------------------------------------------------------------
message(STATUS "运行提示：把 yunet.onnx / face_recognizer_fast.onnx 放到 models/ 目录；")
message(STATUS "          中文显示需把 16x16 点阵字库放到 resources/HZK16（可选）。")
