# 边缘人脸考勤终端（Linux / OpenCV / SQLite3 / Paho MQTT over WebSocket）

纯 C++ 实现、不依赖 Qt：摄像头画面 + `cv::putText` 提示 + 控制台命令；通过
MQTT（WebSocket）接收 Qt 页面下发的人员信息，本地做人脸比对并上报打卡。

```
face_punchin/
├── CMakeLists.txt        # 构建脚本（OpenCV + SQLite3 + Paho，WebSocket 说明）
├── DBManager.h/.cpp      # 本地特征库 / 考勤库（SQLite3，线程安全）
├── main.cpp              # 主程序：检测、注册、比对、上报、画面
├── models/               # yunet.onnx / face_recognizer_fast.onnx
├── resources/HZK16       # 可选：16x16 点阵字库，用于在画面上显示中文姓名
└── legacy/               # 原来的 demo 备份（不参与编译）
```

## 1. 数据流

```
Qt 页面 --MQTT terminal/register--> base64 解码 512B -> Mat(1,128,CV_32F) -> SQLite Person
摄像头帧 -> YuNet 画绿框 -> 取最大脸 -> SFace 128 维特征 -> 与 Person 逐个 FR_COSINE(≥0.363)
        -> 命中: 写 Attendance + 发布 attendance/checkin + 画面绿字 "CHECK IN SUCCESS: 姓名"
        -> 未命中: 画面红字 "NO FACE MATCHED"
```

## 2. 编译

先确认 Paho 的 C 库是**带 WebSocket** 编译的，否则 `ws://` 一定连不上：

```bash
git clone https://github.com/eclipse-paho/paho.mqtt.c.git
cd paho.mqtt.c
cmake -Bbuild -DPAHO_WITH_WEBSOCKETS=TRUE -DPAHO_BUILD_SHARED=TRUE
sudo cmake --build build --target install && sudo ldconfig

git clone https://github.com/eclipse-paho/paho.mqtt.cpp.git
cd paho.mqtt.cpp
cmake -Bbuild -DPAHO_BUILD_SHARED=TRUE -DPAHO_MQTT_C_PATH=/usr/local
sudo cmake --build build --target install && sudo ldconfig
```

> **Paho C++ 1.3.x 与 2.x 的差别**：2.x（master）头文件用了 `std::variant`、
> `std::string_view`，**必须 C++17**（本工程默认就是 C++17，两代都能编）。
> 如果想留在 C++14，安装时切到 1.3.2：
> `git checkout v1.3.2 && cmake -Bbuild ...`。
> 报 `'variant' in namespace 'std' does not name a template type` 就是版本/标准不匹配。

再编译本工程（`build/` 是之前 demo 的旧缓存，先删掉）：

```bash
cd ~/face_punchin
rm -rf build && mkdir build && cd build
cmake .. && make -j4
```

## 3. 运行

```bash
./face_attendance                    # 默认 /dev/video0，models/ 目录
./face_attendance 1 /opt/models      # 指定摄像头编号 + 模型目录
```

窗口内按 **ESC**（或 `q`）退出；控制台回车后输入命令。

## 4. 环境变量（可选，覆盖默认配置）

| 变量 | 默认值 | 说明 |
|---|---|---|
| `FACE_MQTT_URI` | `ws://8.152.219.98:9001/mqtt` | MQTT 地址（WebSocket） |
| `FACE_MQTT_USER` / `FACE_MQTT_PASS` | 空 | 需要鉴权时填 |
| `FACE_DB` | `attendance.db` | SQLite 文件路径 |
| `FACE_CAMERA` | `0` | 摄像头编号 |
| `FACE_YUNET` | 自动探测 | YuNet 模型路径 |
| `FACE_RECOGNIZER` | 自动探测 | SFace 模型路径 |
| `FACE_THRESHOLD` | `0.363` | 余弦相似度阈值 |
| `FACE_DET_SCORE` | `0.9` | YuNet 检测置信度（光照差/侧脸可降到 0.75） |
| `FACE_COOLDOWN` | `30` | 同一人重复打卡冷却（秒） |
| `FACE_MATCH_INTERVAL` | `400` | 比对间隔（毫秒） |
| `FACE_FONT` | `resources/HZK16` | 中文字库路径 |
| `FACE_CLIENT_ID` | 自动生成 | MQTT clientId（多台终端必须不同） |

## 5. 控制台命令

```
help / h     帮助
list / ls    列出已注册人员
att          最近 10 条打卡
reload       重新加载特征库
q / quit     退出
```

## 6. MQTT 协议

订阅 `terminal/register`（录入 / 更新人员）：

```json
{"name":"张三","feature":"<base64：128 个 float、小端、共 512 字节>"}
```

订阅 `terminal/delete`（删除人员，来自大屏的「删除员工」按钮）：

```json
{"name":"张三"}
```

删除时：先把 `Attendance.person_id` 置空、再删 `Person` 行（同一事务）。
必须这么做——`Attendance.person_id` 有外键引用 `Person(id)`，而程序启用了
`PRAGMA foreign_keys=ON`，直接 `DELETE` 会报 `FOREIGN KEY constraint failed`。
打卡历史会保留（姓名还在 `Attendance.name` 里）。

发布 `attendance/checkin`（QoS 1）：

```json
{"name":"张三","time":"2026-11-14 08:30:00"}
```

## 7. 数据库

```sql
Person     (id, name UNIQUE, feature BLOB(512B), created_at, updated_at)
Attendance (id, person_id, name, check_time, epoch, score)
```

同名校验：重复下发同一姓名只更新特征（先 UPDATE，`changes()==0` 再 INSERT）。

## 8. 常见问题

* **连不上 broker**：先确认 Paho C 库带 WebSocket（`-DPAHO_WITH_WEBSOCKETS=TRUE`）。
  程序会打印 paho 的原始错误；若含 "WebSocket" 字样即此问题。
* **画面上中文是问号**：`cv::putText` 只支持 ASCII。把 `HZK16`（16×16 点阵，
  约 282KB）放到 `resources/`，程序用 iconv 转 GBK 后自行点阵绘制；没有字库时
  中文姓名只在控制台显示。
* **总是 NO FACE MATCHED**：底部状态栏有 `best:x.xxx` 最高分。若长期在 0.2~0.3，
  说明注册端特征与本端预处理不一致（注册端也应使用 OpenCV `FaceRecognizerSF`
  的 `alignCrop`+`feature`）。
* **同一个人反复打卡**：调大 `FACE_COOLDOWN`。
* **VT/serial 与摄像头并存**：控制台轮询用 `select()` 非阻塞读 stdin，不影响视频循环。

## 9. 已验证项

* `g++ -std=c++14 -Wall -Wextra -fsyntax-only` 通过（用桩头文件，Linux/非 Linux、
  有/无 iconv 四种组合均无告警）。
* Base64（含 URL-safe、换行、非法字符）、JSON（中文、转义、`\u`、往返）等工具函数
  的单元测试全部通过。
* 建表 / UPDATE / INSERT / LIMIT / 唯一约束等 SQL 在真实 SQLite 上验证通过。
* 未在本机交叉编译真实目标（本机无 OpenCV / Paho / 板子），需在开发板上 build 一次。
