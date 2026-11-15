// ============================================================================
//  main.cpp —— 边缘人脸考勤终端（Edge Face Attendance Terminal）
//
//  运行环境 : Linux 开发板 / C++14 / OpenCV 4.5.4 / SQLite3
//             Eclipse Paho MQTT C++（WebSocket 传输、异步客户端）
//
//  功能：
//    1. 循环读摄像头 —— YuNet 检测人脸并画绿框，窗口左上角提示 ESC 退出。
//    2. 订阅 terminal/register —— 接收 Qt 页面下发的
//         {"name":"张三","feature":"<base64 的 512 字节 float 特征>"}
//       Base64 解码 -> cv::Mat(1,128,CV_32F) -> 写入 SQLite 的 Person 表。
//    3. 实时取画面中「最大的人脸」提取特征，遍历 Person 表用 FR_COSINE 比对：
//         相似度 >= 0.363 命中 -> 写 Attendance 表 + 发布 attendance/checkin
//                                  + 画面绿字 "CHECK IN SUCCESS: 姓名"
//         否则            -> 画面红字 "NO FACE MATCHED"
//
//  线程模型：
//    主线程      : 摄像头采集 / 检测 / 特征比对 / 画面绘制 / 控制台命令轮询
//    Paho 线程   : MQTT 收消息（注册）与连接状态回调
//    两者共享 DBManager（内部加锁）和一个 atomic<bool> 脏标记（触发特征库热加载）
// ============================================================================

#include <opencv2/opencv.hpp>
#include <mqtt/async_client.h>

#include "DBManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#  include <sys/select.h>     // select()：非阻塞读控制台
#  include <unistd.h>
#endif

#if defined(HAVE_ICONV) && HAVE_ICONV
#  include <iconv.h>          // UTF-8 -> GBK，配合 HZK16 点阵字库画中文
#endif

// ============================================================================
//  一、全局配置与运行状态
// ============================================================================

// 终端配置：都可以通过环境变量覆盖，方便在板子上用 systemd 部署
struct Config
{
    std::string mqttUri       = "ws://8.152.219.98:9001/mqtt";  // ★ WebSocket 协议
    std::string mqttUser;                                        // 可选：用户名
    std::string mqttPass;                                        // 可选：密码
    std::string clientId;                                        // 自动生成
    std::string topicRegister = "terminal/register";             // 订阅：人员注册
    std::string topicDelete   = "terminal/delete";               // 订阅：人员删除
    std::string topicCheckIn  = "attendance/checkin";            // 发布：打卡记录

    std::string dbPath        = "attendance.db";                 // 本地数据库
    std::string yunetPath;                                       // YuNet 模型
    std::string recogPath;                                       // SFace 模型
    std::string fontPath      = "resources/HZK16";               // 中文字库(可选)

    int   cameraIndex         = 0;      // /dev/video0
    float threshold           = 0.363f; // SFace 官方推荐的余弦阈值
    float detScore            = 0.9f;   // YuNet 检测置信度阈值（光照差时可调到 0.75）
    int   cooldownSec         = 30;     // 同一人 N 秒内不重复上报（防刷屏）
    int   matchIntervalMs     = 400;    // 特征比对的最小间隔，省 CPU
};

static std::atomic<bool>          g_running{true};     // 主循环开关
static volatile std::sig_atomic_t g_signalCaught = 0;  // Ctrl+C / kill 标志
static std::mutex                 g_logMtx;            // 控制台输出互斥

static void onSignal(int) { g_signalCaught = 1; }

// ============================================================================
//  二、通用小工具
// ============================================================================

static bool fileExists(const std::string& path)
{
    if (path.empty())
        return false;
    std::ifstream ifs(path.c_str(), std::ios::binary);
    return ifs.good();
}

static std::string envOr(const char* key, const std::string& def)
{
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

static int envIntOr(const char* key, int def)
{
    const char* v = std::getenv(key);
    if (!v || !*v)
        return def;
    return std::atoi(v);
}

static std::string formatDouble(double v, int precision = 3)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(precision);
    oss << v;
    return oss.str();
}

// 单调递增毫秒数：只用于计算时间差（不受系统改时间影响）
static int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 本地时间字符串："2026-11-14 08:30:00"
static std::string nowTimeString()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

// 线程安全日志：[14:03:22][MQTT] xxx
static void logLine(const std::string& tag, const std::string& msg)
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[16] = {0};
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    std::lock_guard<std::mutex> lk(g_logMtx);
    std::cout << "[" << ts << "][" << tag << "] " << msg << std::endl;
}

static bool isAscii(const std::string& s)
{
    for (unsigned char c : s)
        if (c >= 0x80)
            return false;
    return true;
}

// 客户端 ID 必须每个终端唯一，否则后上线的会把先上线的踢下线
static std::string makeClientId()
{
    unsigned long long r = 0;
#if defined(__linux__)
    r = static_cast<unsigned long long>(getpid());
#endif
    r = (r << 32) ^ static_cast<unsigned long long>(
            std::chrono::system_clock::now().time_since_epoch().count());
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), "face-terminal-%012llx", r & 0xFFFFFFFFFFFFULL);
    return std::string(buf);
}

// ============================================================================
//  三、Base64 解码（Qt 页面下发的人脸特征）
//      输入 512 字节 -> base64 约 684 个字符；这里忽略换行/空格，
//      同时兼容 URL-safe 的 '-' '_' 变体。
// ============================================================================

static bool base64Decode(const std::string& in, std::vector<unsigned char>& out)
{
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // 反查表：值 -1 表示非法字符
    static const std::array<signed char, 256> rev = []() {
        std::array<signed char, 256> t;
        t.fill(-1);
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(kTable[i])] = static_cast<signed char>(i);
        t[static_cast<unsigned char>('-')] = 62;   // URL-safe
        t[static_cast<unsigned char>('_')] = 63;   // URL-safe
        return t;
    }();

    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);

    unsigned int buf  = 0;   // 累积 24 bit
    int          bits = 0;   // 已累积多少 bit

    for (char ch : in)
    {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (uc == '=' || uc == '\r' || uc == '\n' || uc == ' ' || uc == '\t')
            continue;                       // 跳过填充与空白

        const int v = rev[uc];
        if (v < 0)
            return false;                   // 出现非法字符

        buf = (buf << 6) | static_cast<unsigned int>(v);
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

// ============================================================================
//  四、极简 JSON 处理
//      只依赖两个固定格式的消息，不引入第三方 JSON 库：
//        收: {"name":"张三", "feature":"<base64>"}
//        发: {"name":"张三", "time":"2026-11-14 08:30:00"}
// ============================================================================

static void appendUtf8(std::string& out, unsigned int cp)
{
    if (cp <= 0x7F)
        out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 取出 JSON 中某个字符串字段的值（支持 \uXXXX 与代理对，中文姓名没问题）
static bool jsonGetString(const std::string& json, const std::string& key, std::string& value)
{
    const std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos)
        return false;

    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos)
        return false;
    ++pos;

    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
        ++pos;

    if (pos >= json.size() || json[pos] != '"')
        return false;
    ++pos;

    std::string out;
    while (pos < json.size())
    {
        const char c = json[pos++];
        if (c == '"')
        {
            value.swap(out);
            return true;
        }
        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }
        if (pos >= json.size())
            break;

        const char e = json[pos++];
        switch (e)
        {
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/');  break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u':
        {
            if (pos + 4 > json.size())
                return false;
            unsigned int cp = 0;
            for (int i = 0; i < 4; ++i)
            {
                const int h = hexVal(json[pos + i]);
                if (h < 0)
                    return false;
                cp = (cp << 4) | static_cast<unsigned int>(h);
            }
            pos += 4;

            // UTF-16 代理对 -> 一个完整码点
            if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= json.size() &&
                json[pos] == '\\' && json[pos + 1] == 'u')
            {
                unsigned int lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; ++i)
                {
                    const int h = hexVal(json[pos + 2 + i]);
                    if (h < 0) { ok = false; break; }
                    lo = (lo << 4) | static_cast<unsigned int>(h);
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    pos += 6;
                }
            }
            appendUtf8(out, cp);
            break;
        }
        default:
            out.push_back(e);
            break;
        }
    }
    return false;
}

// 序列化为 JSON 字符串内容（UTF-8 原样透传，只转义引号/反斜杠/控制字符）
static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char b[8] = {0};
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            }
            else
            {
                out.push_back(ch);          // 中文 UTF-8 字节直接放行
            }
        }
    }
    return out;
}

// ============================================================================
//  五、中文点阵字库渲染
//
//  cv::putText 用的是 Hershey 矢量字体，只支持 ASCII，中文会变成一堆 '?'。
//  这里用最经典的做法：把 HZK16（16x16 GB2312 点阵字库）按 GBK 区位码查表，
//  逐点画矩形拼出汉字。只需 iconv（glibc 自带）把 UTF-8 姓名转成 GBK。
//  字库没有时自动降级：中文姓名只在控制台打印。
// ============================================================================

class CjkTextRenderer
{
public:
    // 加载 HZK16 点阵字库（每个汉字 32 字节，94x94 的区位表）
    bool load(const std::string& path)
    {
#if defined(HAVE_ICONV) && HAVE_ICONV
        std::ifstream ifs(path.c_str(), std::ios::binary);
        if (!ifs)
            return false;
        data_.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return !data_.empty();
#else
        (void)path;
        return false;
#endif
    }

    bool ready() const { return !data_.empty(); }

    // 在 img 的 org(左上角) 处绘制一行 UTF-8 文本；dotScale 为点阵放大倍数
    void draw(cv::Mat& img, const std::string& utf8, cv::Point org,
              int dotScale, const cv::Scalar& color) const
    {
#if defined(HAVE_ICONV) && HAVE_ICONV
        if (!ready() || utf8.empty())
            return;

        std::string gbk;
        if (!toGbk(utf8, gbk))
            return;

        const double asciiScale = 0.7 * dotScale;
        const int    asciiThick = std::max(1, dotScale / 2);

        int x = org.x;
        size_t i = 0;
        while (i < gbk.size())
        {
            const unsigned char c = static_cast<unsigned char>(gbk[i]);
            if (c < 0x80)
            {
                // 连续的 ASCII 一次性交给 putText，边缘更清晰
                size_t j = i;
                while (j < gbk.size() && static_cast<unsigned char>(gbk[j]) < 0x80)
                    ++j;
                const std::string run = gbk.substr(i, j - i);

                int baseline = 0;
                const cv::Size sz = cv::getTextSize(run, cv::FONT_HERSHEY_SIMPLEX,
                                                    asciiScale, asciiThick, &baseline);
                const int baseY = org.y + 16 * dotScale - static_cast<int>(2 * dotScale);
                cv::putText(img, run, cv::Point(x, baseY), cv::FONT_HERSHEY_SIMPLEX,
                            asciiScale, color, asciiThick, cv::LINE_AA);
                x += sz.width;
                i = j;
            }
            else if (i + 1 < gbk.size())
            {
                drawHanzi(img, c, static_cast<unsigned char>(gbk[i + 1]),
                          cv::Point(x, org.y), dotScale, color);
                x += 16 * dotScale;
                i += 2;
            }
            else
            {
                ++i;   // 落单的半个汉字，直接丢掉
            }
        }
#else
        (void)img; (void)utf8; (void)org; (void)dotScale; (void)color;
#endif
    }

private:
#if defined(HAVE_ICONV) && HAVE_ICONV
    // UTF-8 -> GBK（GB2312 超集，区位码与 HZK16 对应）
    static bool toGbk(const std::string& utf8, std::string& gbk)
    {
        iconv_t cd = iconv_open("GBK", "UTF-8");
        if (cd == reinterpret_cast<iconv_t>(-1))
            return false;

        std::string out(utf8.size() * 2 + 16, '\0');
        char*  inPtr   = const_cast<char*>(utf8.data());
        size_t inLeft  = utf8.size();
        char*  outPtr  = &out[0];
        size_t outLeft = out.size();

        const size_t rc = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
        iconv_close(cd);
        if (rc == static_cast<size_t>(-1))
            return false;

        out.resize(out.size() - outLeft);
        gbk.swap(out);
        return true;
    }

    // 画一个 16x16 的汉字：区位码 -> 32 字节点阵
    void drawHanzi(cv::Mat& img, unsigned char qu, unsigned char wei,
                   cv::Point org, int dotScale, const cv::Scalar& color) const
    {
        if (qu < 0xA1 || wei < 0xA1)
            return;

        const size_t index  = static_cast<size_t>(qu - 0xA1) * 94 + (wei - 0xA1);
        const size_t offset = index * 32;
        if (offset + 32 > data_.size())
            return;                              // 字库里没有这个字

        const unsigned char* p = reinterpret_cast<const unsigned char*>(data_.data()) + offset;
        for (int row = 0; row < 16; ++row)
        {
            const unsigned char hi = p[row * 2];
            const unsigned char lo = p[row * 2 + 1];
            for (int col = 0; col < 16; ++col)
            {
                const unsigned char byte = (col < 8) ? hi : lo;
                const int bit = 7 - (col % 8);
                if (byte & (1 << bit))
                {
                    cv::rectangle(img,
                                  cv::Rect(org.x + col * dotScale, org.y + row * dotScale,
                                           dotScale, dotScale),
                                  color, cv::FILLED);
                }
            }
        }
    }
#endif

    std::vector<char> data_;
};

// ============================================================================
//  六、MQTT 桥接层
//
//  * 连接 URI 使用 ws:// 开头即可让 Paho 走 WebSocket 传输
//    （前提：Paho 的 C 库编译时开了 PAHO_WITH_WEBSOCKETS=TRUE）。
//  * clean_session=true：每次重连都要重新 subscribe，所以订阅放在
//    connected 回调里，而不是连接前调用一次。
//  * 断网时打卡消息先入内存队列，连接恢复后自动补发。
// ============================================================================

class MqttBridge
{
public:
    MqttBridge(DBManager& db, std::atomic<bool>& personsDirty, const Config& cfg)
        : db_(db), personsDirty_(personsDirty), cfg_(cfg),
          client_(cfg.mqttUri, cfg.clientId)
    {
        // 心跳 20s；连接超时 8s；掉线后 1~30s 内自动重连
        connOpts_.set_clean_session(true);
        connOpts_.set_keep_alive_interval(20);
        connOpts_.set_connect_timeout(8);
        connOpts_.set_automatic_reconnect(1, 30);
        if (!cfg_.mqttUser.empty())
        {
            connOpts_.set_user_name(cfg_.mqttUser);
            connOpts_.set_password(cfg_.mqttPass);
        }

        // ---- 收到 terminal/register（注册）或 terminal/delete（删除）----
        // 注意：Paho C++ 1.x 只有「全局消息回调」，没有按主题注册的版本，
        //      所以这里在 onMqttMessage() 里再按 topic 分发。
        client_.set_message_callback(
            [this](const mqtt::const_message_ptr& msg) { this->onMqttMessage(msg); });

        // ---- 连接成功 ----
        client_.set_connected_handler([this](const std::string& cause) {
            connected_      = true;
            connecting_     = false;
            everConnected_  = true;
            logLine("MQTT", "连接成功: " + cfg_.mqttUri +
                            (cause.empty() ? "" : ("  (cause=" + cause + ")")));
            try
            {
                // clean_session=true，重连后订阅会丢失，必须重新订阅
                client_.subscribe(cfg_.topicRegister, 1);
                client_.subscribe(cfg_.topicDelete, 1);
                logLine("MQTT", "已订阅主题: " + cfg_.topicRegister + " , " + cfg_.topicDelete);
            }
            catch (const mqtt::exception& e)
            {
                logLine("ERR", std::string("订阅失败: ") + e.what());
            }
            flushPending();      // 把断网期间缓存的打卡补发出去
        });

        // ---- 连接断开 ----
        client_.set_connection_lost_handler([this](const std::string& cause) {
            connected_ = false;
            logLine("WARN", "MQTT 连接断开: " + (cause.empty() ? std::string("未知原因") : cause));
        });
    }

    ~MqttBridge() { stop(); }

    // 发起异步连接（失败不致命，主循环会定时重试）
    bool start()
    {
        if (connected_.load())
            return true;
        if (connecting_.exchange(true))
            return false;                 // 已有连接动作在进行

        try
        {
            mqtt::token_ptr tok = client_.connect(connOpts_);
            if (tok)
                tok->wait_for(std::chrono::seconds(6));
        }
        catch (const mqtt::exception& e)
        {
            connecting_ = false;
            logLine("WARN", std::string("MQTT 连接失败: ") + e.what());
            const std::string what = e.what();
            if (what.find("WebSocket") != std::string::npos ||
                what.find("websocket") != std::string::npos)
            {
                logLine("WARN", "提示：当前 Paho C 库可能没有编译 WebSocket 支持，"
                                "需要用 -DPAHO_WITH_WEBSOCKETS=TRUE 重新编译安装");
            }
            return false;
        }

        connecting_ = false;
        return connected_.load();
    }

    void stop()
    {
        if (stopped_.exchange(true))
            return;
        try
        {
            if (client_.is_connected())
            {
                mqtt::token_ptr tok = client_.disconnect();
                if (tok)
                    tok->wait_for(std::chrono::seconds(3));
            }
        }
        catch (const std::exception& e)
        {
            logLine("WARN", std::string("断开 MQTT 时出现异常: ") + e.what());
        }
        connected_ = false;
    }

    bool connected() const     { return connected_.load(); }
    bool everConnected() const { return everConnected_.load(); }

    // 上报一条打卡记录：{"name":"张三","time":"2026-11-14 08:30:00"}
    void publishCheckIn(const std::string& name, const std::string& timeStr)
    {
        const std::string payload =
            "{\"name\":\"" + jsonEscape(name) + "\",\"time\":\"" + jsonEscape(timeStr) + "\"}";

        std::lock_guard<std::recursive_mutex> lk(queueMtx_);
        if (pending_.size() >= 200)          // 极端情况下防止内存无限增长
            pending_.pop_front();
        pending_.push_back(payload);

        const bool sent = flushPendingLocked();
        logLine("MQTT", std::string(sent ? "打卡上报: " : "离线，打卡已缓存待补发: ") + payload);
    }

private:
    // 全局消息回调收所有主题，这里按 topic 分发
    void onMqttMessage(const mqtt::const_message_ptr& msg)
    {
        const std::string topic   = msg->get_topic();
        const std::string payload = msg->get_payload_str();

        logLine("MQTT", "收到 " + topic + " 消息，" +
                        std::to_string(payload.size()) + " 字节");

        if (topic == cfg_.topicRegister)
            handleRegisterMessage(payload);
        else if (topic == cfg_.topicDelete)
            handleDeleteMessage(payload);
        else
            logLine("MQTT", "忽略未处理的主题: " + topic);
    }

    // 处理注册消息：{"name":"张三", "feature":"<base64>"}
    void handleRegisterMessage(const std::string& payload)
    {
        std::string name, b64;
        if (!jsonGetString(payload, "name", name) || name.empty())
        {
            logLine("WARN", "注册消息缺少 name 字段，已忽略");
            return;
        }
        if (!jsonGetString(payload, "feature", b64) || b64.empty())
        {
            logLine("WARN", "注册消息缺少 feature 字段，已忽略");
            return;
        }

        // ---- Base64 -> 512 字节 -> 1x128 CV_32F ----
        std::vector<unsigned char> raw;
        if (!base64Decode(b64, raw))
        {
            logLine("WARN", "feature Base64 解码失败: " + name);
            return;
        }
        if (raw.size() != kFeatureBytes)
        {
            logLine("WARN", "特征长度应为 " + std::to_string(kFeatureBytes) +
                            " 字节，实际 " + std::to_string(raw.size()) +
                            " 字节（128 维 float），已忽略: " + name);
            return;
        }

        cv::Mat feature(1, kFeatureDim, CV_32F);
        std::memcpy(feature.data, raw.data(), kFeatureBytes);

        if (cv::norm(feature) < 1e-6)
        {
            logLine("WARN", "特征全为 0，可能是无效数据，已忽略: " + name);
            return;
        }

        // ---- 写入 SQLite ----
        if (db_.upsertPerson(name, feature))
        {
            personsDirty_ = true;            // 通知主线程热加载特征库
            logLine("DB", "人员注册成功: " + name +
                          "，本地库现有 " + std::to_string(db_.countPersons()) + " 人");
        }
        else
        {
            logLine("ERR", "人员注册失败: " + name + "，原因: " + db_.lastError());
        }
    }

    // 处理删除消息：{"name":"张三"}
    // 注意：Attendance 表用外键引用 Person(id)，直接 DELETE 会撞外键约束，
    //       所以 DBManager::deletePerson() 里先把历史打卡的 person_id 置空，
    //       再删人员（同一事务内完成），历史打卡记录本身保留。
    void handleDeleteMessage(const std::string& payload)
    {
        std::string name;
        if (!jsonGetString(payload, "name", name) || name.empty())
        {
            logLine("WARN", "删除消息缺少 name 字段，已忽略");
            return;
        }

        if (db_.deletePerson(name))
        {
            personsDirty_ = true;          // 主线程下一轮重新加载特征库
            logLine("DB", "人员已删除: " + name +
                          "，本地库现有 " + std::to_string(db_.countPersons()) + " 人");
        }
        else
        {
            logLine("WARN", "删除失败: " + name + "，原因: " + db_.lastError());
        }
    }

    void flushPending()
    {
        std::lock_guard<std::recursive_mutex> lk(queueMtx_);
        flushPendingLocked();
    }

    bool flushPendingLocked()
    {
        if (!connected_.load())
            return false;

        while (!pending_.empty())
        {
            try
            {
                // QoS 1：至少送达一次（考勤记录宁可重也不能丢）
                client_.publish(mqtt::make_message(cfg_.topicCheckIn, pending_.front(), 1, false));
                pending_.pop_front();
            }
            catch (const mqtt::exception& e)
            {
                logLine("WARN", std::string("MQTT 发布失败: ") + e.what());
                return false;
            }
        }
        return true;
    }

    DBManager&            db_;
    std::atomic<bool>&    personsDirty_;
    const Config&         cfg_;

    mqtt::async_client    client_;
    mqtt::connect_options connOpts_;

    std::atomic<bool>     connected_{false};
    std::atomic<bool>     connecting_{false};
    std::atomic<bool>     everConnected_{false};
    std::atomic<bool>     stopped_{false};

    std::recursive_mutex  queueMtx_;
    std::deque<std::string> pending_;        // 断网期间的打卡队列
};

// ============================================================================
//  七、画面绘制：检测框、提示条、状态栏
// ============================================================================

enum class ToastKind { None, Success, NoMatch, Already };

// 画面中央偏上的提示（绿色成功 / 红色未匹配 / 橙色已打过卡）
struct ToastState
{
    ToastKind   kind     = ToastKind::None;
    std::string title;                      // ASCII 主标题
    std::string name;                       // 姓名（可能含中文）
    cv::Scalar  color    = cv::Scalar(0, 255, 0);
    int64_t     expireMs = 0;
};

static void drawPanel(cv::Mat& img, const cv::Rect& rc, double alpha = 0.45)
{
    cv::Rect r = rc & cv::Rect(0, 0, img.cols, img.rows);
    if (r.width <= 0 || r.height <= 0)
        return;
    cv::Mat roi = img(r);
    cv::Mat dark(roi.size(), roi.type(), cv::Scalar(0, 0, 0));
    cv::addWeighted(roi, 1.0 - alpha, dark, alpha, 0.0, roi);
}

// 所有检测到的人脸画绿框，并标出 5 个关键点
static void drawDetections(cv::Mat& frame, const cv::Mat& faces)
{
    if (faces.empty() || faces.rows <= 0 || faces.cols < 15)
        return;

    for (int r = 0; r < faces.rows; ++r)
    {
        const float* p = faces.ptr<float>(r);

        int x = std::max(0, static_cast<int>(std::lround(p[0])));
        int y = std::max(0, static_cast<int>(std::lround(p[1])));
        int w = static_cast<int>(std::lround(p[2]));
        int h = static_cast<int>(std::lround(p[3]));

        if (x + w > frame.cols) w = frame.cols - x;
        if (y + h > frame.rows) h = frame.rows - y;
        if (w <= 0 || h <= 0)
            continue;

        cv::rectangle(frame, cv::Rect(x, y, w, h), cv::Scalar(0, 255, 0), 2);

        // 关键点顺序：右眼、左眼、鼻尖、右嘴角、左嘴角
        for (int i = 0; i < 5; ++i)
        {
            const cv::Point pt(static_cast<int>(std::lround(p[4 + i * 2])),
                               static_cast<int>(std::lround(p[5 + i * 2])));
            cv::circle(frame, pt, 2, cv::Scalar(0, 255, 255), cv::FILLED);
        }
    }
}

// 左上角常驻提示
static void drawHint(cv::Mat& frame)
{
    cv::putText(frame, "Press ESC to exit", cv::Point(10, 24),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
}

// 右上角常驻诊断：人脸数 / 最高相似度 / 上次结论。
// 这行与"打卡提示条"是两套独立绘制，只要它在闪，就说明跑的是新程序。
static void drawDebugCorner(cv::Mat& frame, int faceCount, double bestScore, bool matched)
{
    char buf[160] = {0};
    std::snprintf(buf, sizeof(buf), "faces:%d  best:%.3f  %s",
                  faceCount, bestScore, matched ? "MATCH" : "no-match");

    int baseline = 0;
    const double scale = 0.5;
    const cv::Size sz = cv::getTextSize(buf, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline);
    const int x = std::max(8, frame.cols - sz.width - 12);

    cv::putText(frame, buf, cv::Point(x, 22), cv::FONT_HERSHEY_SIMPLEX, scale,
                cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
}

// 打卡结果提示：ASCII 用 putText（绿/红大字），中文姓名用点阵字库
//
// 面板故意放在「画面中部偏上」而不是贴着左上角：
// 摄像头真实分辨率常常比屏幕大（1920x1080 之类），窗口会超出屏幕，
// 贴边绘制的文字容易被裁掉，看起来就像“什么都没显示”。
static void drawToast(cv::Mat& frame, const ToastState& t, const CjkTextRenderer& cjk)
{
    if (t.kind == ToastKind::None)
        return;

    const int panelW = std::min(std::max(0, frame.cols - 40), 640);
    const int panelH = 150;
    if (panelW <= 80 || frame.rows <= panelH)
        return;

    const int px = (frame.cols - panelW) / 2;               // 水平居中
    const int py = std::max(20, frame.rows / 8);            // 垂直偏上，避开人脸中心

    drawPanel(frame, cv::Rect(px, py, panelW, panelH), 0.60);

    // 主标题：居中绘制，绿色 "CHECK IN SUCCESS:" / 红色 "NO FACE MATCHED"
    const double titleScale = 1.2;
    const int    titleThick = 3;
    int baseline = 0;
    const cv::Size ts = cv::getTextSize(t.title, cv::FONT_HERSHEY_SIMPLEX,
                                        titleScale, titleThick, &baseline);
    const int tx = px + std::max(10, (panelW - ts.width) / 2);
    cv::putText(frame, t.title, cv::Point(tx, py + 48),
                cv::FONT_HERSHEY_SIMPLEX, titleScale, t.color, titleThick, cv::LINE_AA);

    if (t.name.empty())
        return;

    if (cjk.ready())
    {
        // 点阵字：先按 UTF-8 字符数算出宽度（每字 16*3 = 48 像素），再居中
        int chars = 0;
        for (char ch : t.name)
            if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
                ++chars;
        const int nameW = chars * 16 * 3;
        const int nx = px + std::max(10, (panelW - nameW) / 2);
        cjk.draw(frame, t.name, cv::Point(nx, py + 66), 3, t.color);
    }
    else if (isAscii(t.name))
    {
        int base2 = 0;
        const cv::Size ns = cv::getTextSize(t.name, cv::FONT_HERSHEY_SIMPLEX,
                                            1.0, 2, &base2);
        cv::putText(frame, t.name, cv::Point(px + std::max(10, (panelW - ns.width) / 2), py + 112),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, t.color, 2, cv::LINE_AA);
    }
    // 非 ASCII 且没有字库时：姓名只在控制台打印（启动时已经给过提示）
}

// 底部状态栏：人数 / 人脸数 / 帧率 / MQTT 状态 / 最高相似度 / 上次结论
static void drawStatusBar(cv::Mat& frame, int personCount, int faceCount, double fps,
                          bool mqttOnline, double bestScore, bool matched)
{
    const int barH = 26;
    if (frame.rows <= barH)
        return;

    drawPanel(frame, cv::Rect(0, frame.rows - barH, frame.cols, barH), 0.55);

    char buf[256] = {0};
    std::snprintf(buf, sizeof(buf),
                  "Persons:%d | faces:%d | FPS:%.1f | MQTT:%s | best:%.3f | %s",
                  personCount, faceCount, fps, mqttOnline ? "on" : "off", bestScore,
                  matched ? "MATCH" : "no-match");

    cv::putText(frame, buf, cv::Point(8, frame.rows - 8),
                cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(220, 220, 220), 1, cv::LINE_AA);
}

// ============================================================================
//  八、人脸检测结果解析 + 特征比对
// ============================================================================

struct Detection
{
    int      row  = -1;      // 在 faces 矩阵中的行号
    cv::Rect rect;
    float    score = 0.0f;   // YuNet 的检测置信度
};

// 取面积最大的人脸（考勤场景：站得最近的人）
static Detection pickLargestFace(const cv::Mat& faces, const cv::Size& frameSize)
{
    Detection best;
    if (faces.empty() || faces.rows <= 0 || faces.cols < 15)
        return best;

    double bestArea = 0.0;
    for (int r = 0; r < faces.rows; ++r)
    {
        const float* p = faces.ptr<float>(r);
        const float  w = p[2];
        const float  h = p[3];
        if (w <= 8.0f || h <= 8.0f)      // 过滤过于离谱的小框
            continue;

        const double area = static_cast<double>(w) * h;
        if (area <= bestArea)
            continue;

        int x = std::max(0, static_cast<int>(std::lround(p[0])));
        int y = std::max(0, static_cast<int>(std::lround(p[1])));
        int ww = static_cast<int>(std::lround(w));
        int hh = static_cast<int>(std::lround(h));
        if (x + ww > frameSize.width)  ww = frameSize.width - x;
        if (y + hh > frameSize.height) hh = frameSize.height - y;

        bestArea  = area;
        best.row  = r;
        best.rect = cv::Rect(x, y, std::max(0, ww), std::max(0, hh));
        best.score = p[14];
    }
    return best;
}

struct MatchResult
{
    bool        matched  = false;
    std::string name;
    double      score    = 0.0;     // 命中时的相似度
    long long   personId = 0;
    double      bestScore = 0.0;    // 全部比对中的最高分（无论是否命中，便于调阈值）
    std::string bestName;
};

// 从检测结果的第 row 张脸提取 128 维特征
//   alignCrop：用 5 个关键点做相似变换，把人脸摆正并缩放到 112x112
//   feature  ：内部 blobFromImage(scale=1, 112x112, swapRB=true) -> 输出 1x128 CV_32F
static bool extractFaceFeature(const cv::Mat& frame,
                               const cv::Mat& faces,
                               int row,
                               const cv::Ptr<cv::FaceRecognizerSF>& recognizer,
                               cv::Mat& outFeature)
{
    cv::Mat aligned;
    recognizer->alignCrop(frame, faces.row(row), aligned);
    if (aligned.empty())
        return false;

    recognizer->feature(aligned, outFeature);
    return !outFeature.empty();
}

// 提取最大人脸的特征，并遍历本地特征库做余弦比对
static MatchResult recognizeLargestFace(const cv::Mat& frame,
                                        const cv::Mat& faces,
                                        int row,
                                        const cv::Ptr<cv::FaceRecognizerSF>& recognizer,
                                        const std::vector<PersonRecord>& persons,
                                        float threshold)
{
    MatchResult res;

    cv::Mat feature;
    if (!extractFaceFeature(frame, faces, row, recognizer, feature))
        return res;

    for (const auto& p : persons)
    {
        if (p.feature.empty())
            continue;

        // ★ clone 是必须的：FaceRecognizerSF::match 会就地归一化输入特征，
        //   直接传缓存里的 Mat 会把底库数据改掉（虽然结果一样，但语义不清）。
        const double score = recognizer->match(feature.clone(), p.feature.clone(),
                                               cv::FaceRecognizerSF::FR_COSINE);

        if (score > res.bestScore)
        {
            res.bestScore = score;
            res.bestName  = p.name;
        }
        if (score >= threshold && score > res.score)
        {
            res.matched  = true;
            res.score    = score;
            res.name     = p.name;
            res.personId = p.id;
        }
    }
    return res;
}

// ============================================================================
//  九、控制台交互（非阻塞：用 select 轮询 stdin，不影响视频循环）
// ============================================================================

static void printHelp()
{
    std::lock_guard<std::mutex> lk(g_logMtx);
    std::cout << "  可用控制台命令：\n"
                 "    help / h         显示本帮助\n"
                 "    enroll <姓名>    注册画面中当前最大的人脸（没有 Qt 页面时自测用）\n"
                 "    list / ls        列出本地已注册人员\n"
                 "    att              查看最近 10 条打卡记录\n"
                 "    reload           从数据库重新加载人脸特征库\n"
                 "    q / quit         退出程序（也可以直接按 ESC）\n";
}

static void handleConsoleCommand(const std::string& raw,
                                 DBManager& db,
                                 std::atomic<bool>& personsDirty,
                                 std::atomic<bool>& running,
                                 std::string& pendingEnroll)
{
    std::istringstream iss(raw);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty())
        return;

    for (char& c : cmd)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (cmd == "q" || cmd == "quit" || cmd == "exit")
    {
        logLine("INFO", "控制台请求退出");
        running = false;
        return;
    }
    if (cmd == "help" || cmd == "h" || cmd == "?")
    {
        printHelp();
        return;
    }
    // enroll 张三：把「下一帧里最大的那张脸」注册成本地人员。
    // 没有 Qt 页面下发特征时，用它先把底库填起来，才能验证比对/打卡/上报。
    if (cmd == "enroll" || cmd == "reg" || cmd == "register")
    {
        std::string who;
        std::getline(iss, who);

        const size_t b = who.find_first_not_of(" \t");
        if (b == std::string::npos)
        {
            logLine("WARN", "用法: enroll 张三   （注册画面中当前最大的人脸）");
            return;
        }
        const size_t e = who.find_last_not_of(" \t");
        who = who.substr(b, e - b + 1);

        pendingEnroll = who;
        logLine("DB", "等待采集人脸特征，用于注册: " + who + "（请正对摄像头，保持 1 秒）");
        return;
    }
    if (cmd == "list" || cmd == "ls")
    {
        const std::vector<PersonRecord> ps = db.getAllPersons();
        std::lock_guard<std::mutex> lk(g_logMtx);
        std::cout << "  本地人员共 " << ps.size() << " 人：\n";
        for (const auto& p : ps)
            std::cout << "    #" << p.id << "  " << p.name
                      << "   注册时间 " << p.createdAt << "\n";
        return;
    }
    if (cmd == "att")
    {
        const std::vector<AttendanceRecord> rs = db.queryRecentAttendance(10);
        std::lock_guard<std::mutex> lk(g_logMtx);
        std::cout << "  最近 " << rs.size() << " 条打卡记录：\n";
        for (const auto& r : rs)
            std::cout << "    " << r.checkTime << "  " << r.name
                      << "  相似度 " << formatDouble(r.score) << "\n";
        return;
    }
    if (cmd == "reload")
    {
        personsDirty = true;
        logLine("DB", "已请求重新加载特征库");
        return;
    }
    logLine("WARN", "未知命令：" + cmd + "（输入 help 查看）");
}

static void pollConsole(DBManager& db,
                        std::atomic<bool>& personsDirty,
                        std::atomic<bool>& running,
                        std::string& pendingEnroll)
{
#if defined(__linux__)
    static std::string lineBuf;          // 跨帧累积的一行输入

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;                      // 只探测，不阻塞

    if (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv) <= 0)
        return;

    // ★ 必须用 unsigned char 接收：UTF-8 汉字的每个字节都 >= 0x80，
    //   而 char 在 x86/ARM 上默认有符号，用 char 比较 ">= 0x20" 会把中文
    //   当成控制字符丢掉（表现为：输入 "enroll 张三" 只有 "enroll " 到达程序）。
    unsigned char ch = 0;
    if (::read(STDIN_FILENO, &ch, 1) != 1)
        return;

    // 丢弃方向键/功能键的转义序列（ESC [ A 之类），免得把 '[' 'A' 塞进命令里
    static int escSkip = 0;
    if (escSkip > 0)
    {
        --escSkip;
        return;
    }
    if (ch == 0x1B)
    {
        escSkip = 2;
        return;
    }

    if (ch == '\n' || ch == '\r')
    {
        if (!lineBuf.empty())
        {
            handleConsoleCommand(lineBuf, db, personsDirty, running, pendingEnroll);
            lineBuf.clear();
        }
    }
    else if (ch == 0x7F || ch == 0x08)   // 退格
    {
        if (!lineBuf.empty())
            lineBuf.pop_back();
    }
    else if (ch >= 0x20)                // 可打印字符（含 UTF-8 中文的每个字节）
    {
        lineBuf.push_back(static_cast<char>(ch));
    }
#else
    (void)db; (void)personsDirty; (void)running; (void)pendingEnroll;
#endif
}

// ============================================================================
//  十、配置装载（命令行 > 环境变量 > 默认值）
// ============================================================================

// 依次在若干目录里找模型文件；写回 triedPaths 方便报错时提示
static std::string resolveModel(const char* envKey,
                                const std::string& modelDir,
                                const std::vector<std::string>& names,
                                std::string* triedPaths)
{
    const char* env = std::getenv(envKey);
    if (env && *env)
    {
        if (fileExists(env))
            return std::string(env);
        if (triedPaths)
            *triedPaths += "    " + std::string(env) + "  (环境变量指定，但文件不存在)\n";
    }

    std::vector<std::string> dirs;
    if (!modelDir.empty())
        dirs.push_back(modelDir);
    dirs.push_back("models");
    dirs.push_back("../models");
    dirs.push_back("../../models");
    dirs.push_back(".");

    for (const auto& d : dirs)
    {
        for (const auto& n : names)
        {
            const std::string p = (d == ".") ? n : (d + "/" + n);
            if (fileExists(p))
                return p;
            if (triedPaths)
                *triedPaths += "    " + p + "\n";
        }
    }
    return "";
}

static Config loadConfig(int argc, char** argv)
{
    Config cfg;

    // 命令行：./face_attendance [摄像头编号] [模型目录]
    cfg.cameraIndex = (argc > 1) ? std::atoi(argv[1]) : envIntOr("FACE_CAMERA", cfg.cameraIndex);
    const std::string modelDir = (argc > 2) ? std::string(argv[2])
                                            : envOr("FACE_MODEL_DIR", "models");

    cfg.mqttUri       = envOr("FACE_MQTT_URI", cfg.mqttUri);
    cfg.mqttUser      = envOr("FACE_MQTT_USER", "");
    cfg.mqttPass      = envOr("FACE_MQTT_PASS", "");
    cfg.dbPath        = envOr("FACE_DB", cfg.dbPath);
    cfg.fontPath      = envOr("FACE_FONT", cfg.fontPath);
    cfg.cooldownSec   = envIntOr("FACE_COOLDOWN", cfg.cooldownSec);
    cfg.matchIntervalMs = envIntOr("FACE_MATCH_INTERVAL", cfg.matchIntervalMs);
    cfg.threshold     = static_cast<float>(std::atof(envOr("FACE_THRESHOLD", "0.363").c_str()));
    cfg.detScore      = static_cast<float>(std::atof(envOr("FACE_DET_SCORE", "0.9").c_str()));
    cfg.clientId      = envOr("FACE_CLIENT_ID", makeClientId());

    // 模型名兼容两种写法：官方 yunet.onnx / 下载后常见的 yunet(1).onnx
    cfg.yunetPath = resolveModel("FACE_YUNET", modelDir,
                                 {"yunet.onnx", "yunet(1).onnx"}, nullptr);
    cfg.recogPath = resolveModel("FACE_RECOGNIZER", modelDir,
                                 {"face_recognizer_fast.onnx",
                                  "face_recognizer_fast(1).onnx"}, nullptr);
    return cfg;
}

// ============================================================================
//  十一、main
// ============================================================================

int main(int argc, char** argv)
{
    std::signal(SIGINT, onSignal);
#ifdef SIGTERM
    std::signal(SIGTERM, onSignal);
#endif

    const Config cfg = loadConfig(argc, argv);

    logLine("INFO", "======================================================");
    logLine("INFO", " 边缘人脸考勤终端  (ESC 退出 / 控制台输入 help 查看命令)");
    logLine("INFO", "======================================================");
    logLine("INFO", "MQTT      : " + cfg.mqttUri + "  clientId=" + cfg.clientId);
    logLine("INFO", "主题      : 订阅 " + cfg.topicRegister + " , " + cfg.topicDelete +
                    "  ->  发布 " + cfg.topicCheckIn);
    logLine("INFO", "摄像头    : " + std::to_string(cfg.cameraIndex));
    logLine("INFO", "数据库    : " + cfg.dbPath);
    logLine("INFO", "比对阈值  : " + formatDouble(cfg.threshold));

    // ------------------------------------------------------------------
    // ★ 第 1 步：模型文件必须存在
    // ------------------------------------------------------------------
    std::string tried;
    const std::string yunet = cfg.yunetPath.empty()
        ? resolveModel("FACE_YUNET", "models", {"yunet.onnx", "yunet(1).onnx"}, &tried)
        : cfg.yunetPath;
    const std::string recog = cfg.recogPath.empty()
        ? resolveModel("FACE_RECOGNIZER", "models",
                       {"face_recognizer_fast.onnx", "face_recognizer_fast(1).onnx"}, &tried)
        : cfg.recogPath;

    if (yunet.empty() || recog.empty())
    {
        logLine("ERR", "找不到模型文件，已尝试以下路径：\n" + tried);
        logLine("ERR", "请把 yunet.onnx 与 face_recognizer_fast.onnx 放到 models/ 目录，"
                       "或用环境变量 FACE_YUNET / FACE_RECOGNIZER 指定绝对路径");
        return 1;
    }
    logLine("INFO", "YuNet     : " + yunet);
    logLine("INFO", "SFace     : " + recog);

    // ------------------------------------------------------------------
    // ★ 第 2 步：打开数据库（自动建表）
    // ------------------------------------------------------------------
    DBManager db;
    if (!db.open(cfg.dbPath))
    {
        logLine("ERR", "打开数据库失败: " + db.lastError());
        return 1;
    }
    logLine("DB", "SQLite 就绪: " + cfg.dbPath +
                  "，现有人员 " + std::to_string(db.countPersons()) + " 人");

    // ------------------------------------------------------------------
    // ★ 第 3 步：加载两个 ONNX 模型
    //   FaceDetectorYN   : YuNet 人脸检测（输出 15 列：x,y,w,h + 5 关键点 + 置信度）
    //   FaceRecognizerSF : SFace 人脸识别（对齐 + 128 维特征 + 余弦比对）
    // ------------------------------------------------------------------
    cv::Ptr<cv::FaceDetectorYN>   detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    try
    {
        detector = cv::FaceDetectorYN::create(yunet, "", cv::Size(320, 320),
                                              cfg.detScore, // score_threshold：置信度阈值
                                              0.3f,   // nms_threshold
                                              5000,   // top_k
                                              0, 0);  // CPU
        recognizer = cv::FaceRecognizerSF::create(recog, "", 0, 0);
    }
    catch (const cv::Exception& e)
    {
        logLine("ERR", std::string("模型加载失败: ") + e.what());
        return 1;
    }
    if (detector.empty() || recognizer.empty())
    {
        logLine("ERR", "模型加载失败（返回空对象），请检查 ONNX 文件是否完整");
        return 1;
    }

    // ------------------------------------------------------------------
    // ★ 第 4 步：中文字库（可选，只影响画面上的中文姓名）
    // ------------------------------------------------------------------
    CjkTextRenderer cjk;
    if (cjk.load(cfg.fontPath))
        logLine("INFO", "中文字库已加载: " + cfg.fontPath);
    else
        logLine("WARN", "未找到中文字库 " + cfg.fontPath +
                        "：中文姓名仅在控制台显示（可把 HZK16 放到 resources/ 后重试）");

    // ------------------------------------------------------------------
    // ★ 第 5 步：打开摄像头
    // ------------------------------------------------------------------
    cv::VideoCapture cap(cfg.cameraIndex);
    if (!cap.isOpened())
    {
        logLine("ERR", "无法打开摄像头 " + std::to_string(cfg.cameraIndex) +
                       "（检查 /dev/videoN 是否存在、当前用户是否有权限）");
        return 1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    logLine("INFO", "摄像头已打开: " +
                    std::to_string(static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH))) + "x" +
                    std::to_string(static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT))));

    // ------------------------------------------------------------------
    // ★ 第 6 步：启动 MQTT（WebSocket）
    // ------------------------------------------------------------------
    std::atomic<bool> personsDirty{false};
    std::unique_ptr<MqttBridge> mqtt;
    try
    {
        mqtt.reset(new MqttBridge(db, personsDirty, cfg));
    }
    catch (const std::exception& e)
    {
        logLine("ERR", std::string("创建 MQTT 客户端失败: ") + e.what());
        return 1;
    }
    mqtt->start();      // 失败不退出，主循环里每 10 秒重试一次

    // ------------------------------------------------------------------
    // ★ 第 7 步：本地特征库（内存缓存，MQTT 注册新人员后热更新）
    // ------------------------------------------------------------------
    std::vector<PersonRecord> persons = db.getAllPersons();
    logLine("DB", "特征库载入完成，共 " + std::to_string(persons.size()) + " 人");

    // ------------------------------------------------------------------
    // ★ 第 8 步：创建窗口
    // ------------------------------------------------------------------
    const std::string kWindow = "Face Attendance Terminal";
    try
    {
        cv::namedWindow(kWindow, cv::WINDOW_AUTOSIZE);
    }
    catch (const cv::Exception& e)
    {
        logLine("ERR", std::string("创建 OpenCV 窗口失败（是否没有图形界面?）: ") + e.what());
        return 1;
    }

    printHelp();

    // ------------------------------------------------------------------
    // ★ 第 9 步：主循环
    // ------------------------------------------------------------------
    ToastState toast;
    double     fps           = 0.0;
    int        frameCount    = 0;
    int64_t    lastFpsMs     = nowMs();
    int64_t    lastMatchMs   = 0;
    int64_t    lastRetryMs   = 0;
    int64_t    lastFaceLogMs = 0;
    double     lastBestScore = 0.0;
    bool       lastMatched   = false;
    std::string pendingEnroll;      // 控制台 enroll 命令待采集的姓名（空 = 无）

    // 更新提示条
    auto showToast = [&toast](ToastKind kind, const std::string& title,
                              const std::string& name, const cv::Scalar& color, int holdMs) {
        toast.kind     = kind;
        toast.title    = title;
        toast.name     = name;
        toast.color    = color;
        toast.expireMs = nowMs() + holdMs;
    };

    // 命中/未命中的处理：写库 + 上报 + 画面提示
    auto handleMatch = [&](const MatchResult& mr) {
        const long long nowSec = static_cast<long long>(std::time(nullptr));

        if (!mr.matched)
        {
            showToast(ToastKind::NoMatch, "NO FACE MATCHED", "", cv::Scalar(0, 0, 255), 1500);
            return;
        }

        // 冷却时间内重复出现 -> 不重复上报，只提示
        const long long last = db.lastCheckInEpoch(mr.name);
        if (last > 0 && nowSec - last < cfg.cooldownSec)
        {
            showToast(ToastKind::Already, "ALREADY CHECKED IN:", mr.name,
                      cv::Scalar(0, 200, 255), 1500);
            return;
        }

        const std::string ts = nowTimeString();
        if (!db.addAttendance(mr.name, ts, nowSec, mr.score, mr.personId))
        {
            logLine("ERR", "写入考勤记录失败: " + db.lastError());
            return;
        }

        // 本地写库成功后再上报，保证「库里有的，服务器也会收到」
        mqtt->publishCheckIn(mr.name, ts);

        showToast(ToastKind::Success, "CHECK IN SUCCESS:", mr.name,
                  cv::Scalar(0, 255, 0), 3000);
        logLine("INFO", "打卡成功: " + mr.name +
                        "  相似度=" + formatDouble(mr.score) + "  时间=" + ts);
    };

    while (g_running.load() && !g_signalCaught)
    {
        // ---- 9.1 取一帧 ----
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
        {
            logLine("WARN", "摄像头返回空帧，100ms 后重试");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 分辨率保护：不少摄像头会忽略上面的 640x480 设置，直接吐 1920x1080，
        // 窗口就会超出屏幕，贴着边缘画的文字会被裁掉（看起来像“什么都没显示”）。
        // 这里统一把处理/显示分辨率限制在 1280 宽以内，检测、比对、绘制全部用同一张图。
        const int kMaxWidth = 1280;
        if (frame.cols > kMaxWidth)
        {
            const double s = static_cast<double>(kMaxWidth) / static_cast<double>(frame.cols);
            cv::resize(frame, frame, cv::Size(kMaxWidth,
                                              static_cast<int>(frame.rows * s + 0.5)));
        }

        // ---- 9.2 YuNet 检测 + 画绿框 ----
        detector->setInputSize(frame.size());
        cv::Mat faces;
        detector->detect(frame, faces);
        drawDetections(frame, faces);

        // ---- 9.3 特征库热更新（MQTT 线程注册了新人员）----
        if (personsDirty.exchange(false))
        {
            std::string err;
            std::vector<PersonRecord> fresh = db.getAllPersons(&err);
            if (err.empty())
            {
                persons.swap(fresh);
                logLine("DB", "特征库已刷新，共 " + std::to_string(persons.size()) + " 人");
            }
            else
            {
                logLine("ERR", "刷新特征库失败: " + err);
            }
        }

        // ---- 9.4 定时对最大人脸做比对（不必每帧都跑，省 CPU）----
        const int64_t tNow = nowMs();
        const int     detectedFaces = faces.empty() ? 0 : faces.rows;

        if (tNow - lastMatchMs >= cfg.matchIntervalMs)
        {
            lastMatchMs = tNow;

            const Detection det = pickLargestFace(faces, frame.size());

            // 控制台 enroll 命令：把当前最大的人脸注册进本地库（没有 Qt 页面时的自测入口）
            if (!pendingEnroll.empty() && det.row >= 0)
            {
                cv::Mat feat;
                if (extractFaceFeature(frame, faces, det.row, recognizer, feat))
                {
                    const std::string who = pendingEnroll;
                    pendingEnroll.clear();                 // 采集成功，清空等待状态

                    if (db.upsertPerson(who, feat))
                    {
                        persons = db.getAllPersons();      // 立即生效，不等下一轮热加载
                        personsDirty = false;
                        showToast(ToastKind::Success, "REGISTERED:", who,
                                  cv::Scalar(0, 255, 0), 2000);
                        logLine("DB", "本地注册成功: " + who +
                                      "，库中现有 " + std::to_string(persons.size()) + " 人");
                    }
                    else
                    {
                        logLine("ERR", "本地注册失败: " + db.lastError());
                    }
                }
                // 提取失败（脸太小/太偏）就保留 pendingEnroll，下一轮继续尝试
            }

            if (det.row >= 0)
            {
                // 底库为空时 recognizeLargestFace 内部直接返回「未匹配」，
                // 所以下面一定会调用 handleMatch()，画面上必然给出红字提示。
                const MatchResult mr = recognizeLargestFace(frame, faces, det.row,
                                                            recognizer, persons, cfg.threshold);
                lastBestScore = mr.bestScore;
                lastMatched   = mr.matched;

                // 每秒一行诊断：现场排查「到底有没有检到脸、最高分多少」
                if (tNow - lastFaceLogMs >= 1000)
                {
                    lastFaceLogMs = tNow;
                    char info[256] = {0};
                    std::snprintf(info, sizeof(info),
                                  "faces=%d  最大脸=%dx%d  best=%.3f(%s)  -> %s",
                                  detectedFaces, det.rect.width, det.rect.height,
                                  mr.bestScore,
                                  mr.bestName.empty() ? "-" : mr.bestName.c_str(),
                                  mr.matched ? "MATCH" : "NO MATCH");
                    logLine("FACE", info);
                }

                // 匹配上了但脸离得太远：拒绝打卡，但仍然给出提示
                if (mr.matched && det.rect.area() < 80 * 80)
                {
                    showToast(ToastKind::NoMatch, "MOVE CLOSER", mr.name,
                              cv::Scalar(0, 0, 255), 1200);
                }
                else
                {
                    handleMatch(mr);   // 命中 -> 绿字；未命中 -> 红字 NO FACE MATCHED
                }
            }
            else if (tNow - lastFaceLogMs >= 1000)
            {
                lastFaceLogMs = tNow;
                logLine("FACE", "未检测到人脸（faces=" + std::to_string(detectedFaces) +
                                "），若画面里明明有人，可降低检测阈值："
                                "FACE_DET_SCORE=0.75 ./build2/face_attendance");
            }
        }

        // ---- 9.5 MQTT 首次连接失败时定时重试 ----
        if (!mqtt->connected() && !mqtt->everConnected() && tNow - lastRetryMs > 10000)
        {
            lastRetryMs = tNow;
            logLine("MQTT", "尝试重新连接 " + cfg.mqttUri);
            mqtt->start();
        }

        // ---- 9.6 绘制提示与状态栏 ----
        drawHint(frame);
        drawDebugCorner(frame, detectedFaces, lastBestScore, lastMatched);
        if (toast.kind != ToastKind::None && tNow < toast.expireMs)
            drawToast(frame, toast, cjk);
        else
            toast.kind = ToastKind::None;

        drawStatusBar(frame, static_cast<int>(persons.size()), detectedFaces, fps,
                      mqtt->connected(), lastBestScore, lastMatched);

        // ---- 9.7 显示 + 按键 ----
        cv::imshow(kWindow, frame);
        const int key = cv::waitKey(1) & 0xFF;
        if (key == 27 || key == 'q' || key == 'Q')   // ESC 退出
        {
            logLine("INFO", "窗口按键退出");
            break;
        }

        // ---- 9.8 控制台命令（非阻塞）----
        pollConsole(db, personsDirty, g_running, pendingEnroll);

        // ---- 9.9 帧率统计 ----
        ++frameCount;
        if (tNow - lastFpsMs >= 1000)
        {
            fps = frameCount * 1000.0 / static_cast<double>(tNow - lastFpsMs);
            frameCount = 0;
            lastFpsMs  = tNow;
        }
    }

    // ------------------------------------------------------------------
    // ★ 第 10 步：收尾
    // ------------------------------------------------------------------
    logLine("INFO", g_signalCaught ? "收到退出信号，正在关闭..." : "正在关闭...");
    g_running = false;

    mqtt->stop();
    cap.release();
    cv::destroyAllWindows();
    db.close();

    logLine("INFO", "已退出。");
    return 0;
}
