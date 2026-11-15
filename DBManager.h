// ============================================================================
//  DBManager.h —— 本地人脸特征库 / 考勤记录库（SQLite3 封装）
//
//  两张表：
//    Person     : 姓名 + 128 维人脸特征（BLOB，512 字节），姓名唯一
//    Attendance : 打卡流水（姓名 / 时间字符串 / epoch 秒 / 相似度）
//
//  ★ 线程安全：内部使用递归互斥锁，MQTT 回调线程（Paho 内部线程）与
//    主线程（摄像头循环）可以同时调用本类的任何接口。
//  ★ 每次调用都新建 sqlite3_stmt，避免语句对象跨线程复用的问题；
//    人脸底库规模只有几十~几百人，这点开销可以忽略。
// ============================================================================

#ifndef FACE_ATTENDANCE_DBMANAGER_H
#define FACE_ATTENDANCE_DBMANAGER_H

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <sqlite3.h>

// SFace（face_recognizer_fast.onnx）输出 1x128 的 float 特征，
// 128 * sizeof(float) = 512 字节，正好对应 Qt 页面 base64 解码后的长度。
constexpr int    kFeatureDim   = 128;
constexpr size_t kFeatureBytes = static_cast<size_t>(kFeatureDim) * sizeof(float);

// 一条人员记录（特征为 1x128 CV_32F，浅拷贝共享数据，读的时候不要改）
struct PersonRecord
{
    long long   id = 0;
    std::string name;
    cv::Mat     feature;      // 1 x 128, CV_32F
    std::string createdAt;
};

// 一条考勤记录
struct AttendanceRecord
{
    long long   id = 0;
    std::string name;
    std::string checkTime;    // "2026-11-14 08:30:00"
    long long   epoch = 0;    // Unix 时间戳（秒），用于防重复打卡
    double      score = 0.0;  // 余弦相似度
};

class DBManager
{
public:
    DBManager() = default;
    ~DBManager();

    // 禁止拷贝：内部持有 sqlite3* 和互斥锁
    DBManager(const DBManager&) = delete;
    DBManager& operator=(const DBManager&) = delete;

    // ------------------------------------------------------------------
    // 打开 / 关闭数据库；打开时自动建表、设置 WAL 与 busy_timeout
    // ------------------------------------------------------------------
    bool open(const std::string& dbPath);
    void close();

    // ------------------------------------------------------------------
    // 新增或更新人员（同名视为“重新注册”，直接覆盖特征）
    // feature 必须是 1x128 CV_32F（128x1 也可以，内部会 reshape）
    // ------------------------------------------------------------------
    bool upsertPerson(const std::string& name, const cv::Mat& feature);

    // 取出全部人员特征（用于人证比对时遍历）
    std::vector<PersonRecord> getAllPersons(std::string* err = nullptr);

    // 按姓名删除人员（来自 MQTT terminal/delete）
    // 同一事务里：先把 Attendance.person_id 置空（避免外键冲突），再删 Person 行。
    // 打卡历史保留（姓名仍在 Attendance.name 里），方便事后查考勤。
    bool deletePerson(const std::string& name);

    // 人员数量（控制台统计用）
    int countPersons();

    // ------------------------------------------------------------------
    // 写一条打卡记录
    // personId <= 0 时写 NULL；score 为本次比对的余弦相似度
    // ------------------------------------------------------------------
    bool addAttendance(const std::string& name,
                       const std::string& checkTime,
                       long long          epoch,
                       double             score,
                       long long          personId);

    // 某人最近一次打卡的 epoch（0 表示从未打卡），用于“冷却时间”防刷屏
    long long lastCheckInEpoch(const std::string& name);

    // 最近 N 条打卡记录，按时间倒序
    std::vector<AttendanceRecord> queryRecentAttendance(int limit);

    // 最近一次错误描述（线程安全快照）
    std::string lastError();

private:
    // 建表
    bool createTables();

    // 执行一段（可包含多条语句的）SQL
    bool exec(const std::string& sql);

    // 记录错误信息（调用方需已持锁或允许重入）
    void setError(const std::string& msg);

    sqlite3*                     db_ = nullptr;
    mutable std::recursive_mutex mtx_;      // 递归锁：允许内部函数重复加锁
    std::string                  dbPath_;
    std::string                  lastError_;
};

#endif // FACE_ATTENDANCE_DBMANAGER_H
