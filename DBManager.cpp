// ============================================================================
//  DBManager.cpp —— SQLite3 特征库 / 考勤库实现
//
//  设计要点：
//    1. Person.name 加 UNIQUE 约束：Qt 页面重复下发同一个人时，只更新特征，
//       不会产生重复记录。先 UPDATE 再判断 sqlite3_changes()，兼容旧版 SQLite
//       （不用 3.24+ 才有的 UPSERT 语法）。
//    2. 特征统一存成 128 * 4 = 512 字节的 blob，读出时 memcpy 回 Mat(1,128,CV_32F)。
//       小端平台（x86 / ARM 均如此）可直接互操作。
//    3. 每条 SQL 都做参数绑定，避免注入以及姓名带引号时的语法错误。
//    4. 打开 WAL 模式 + busy_timeout，MQTT 线程写库时不会把主线程卡死。
// ============================================================================

#include "DBManager.h"

#include <cstring>
#include <iostream>

namespace {

// 建表 SQL：sqlite3_exec 支持一次执行多条语句
const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS Person ("
    "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    name        TEXT    NOT NULL UNIQUE,"                                    // 姓名唯一
    "    feature     BLOB    NOT NULL,"                                            // 512 字节特征
    "    created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),"
    "    updated_at  TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS Attendance ("
    "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    person_id   INTEGER,"
    "    name        TEXT    NOT NULL,"
    "    check_time  TEXT    NOT NULL,"                                            // "2026-11-14 08:30:00"
    "    epoch       INTEGER NOT NULL,"                                            // Unix 秒，便于去重/统计
    "    score       REAL    NOT NULL DEFAULT 0,"
    "    FOREIGN KEY (person_id) REFERENCES Person(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_attendance_name_epoch ON Attendance(name, epoch DESC);"
    "CREATE INDEX IF NOT EXISTS idx_attendance_epoch ON Attendance(epoch DESC);";

// 把外部传入的特征整理成「1 x 128、CV_32F、内存连续」的 Mat
// 返回空 Mat 表示格式非法
cv::Mat asFeatureMat(const cv::Mat& in)
{
    if (in.empty())
        return cv::Mat();

    cv::Mat f;
    if (in.type() != CV_32F)
        in.convertTo(f, CV_32F);          // 例如模型输出被转成了 CV_64F
    else
        f = in;

    if (f.total() != static_cast<size_t>(kFeatureDim))
        return cv::Mat();

    try
    {
        // reshape(1,1)：单通道、单行；clone 保证内存连续，方便 memcpy 成 blob
        return f.reshape(1, 1).clone();
    }
    catch (const cv::Exception&)
    {
        return cv::Mat();
    }
}

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================
DBManager::~DBManager()
{
    close();
}

// ============================================================================
//  打开数据库
// ============================================================================
bool DBManager::open(const std::string& dbPath)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    close();

    // FULLMUTEX：即便将来在别的线程误用同一连接也不会崩，配合本类互斥锁双保险
    int rc = sqlite3_open_v2(dbPath.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK)
    {
        lastError_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 失败";
        if (db_)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    dbPath_ = dbPath;

    // 3 秒忙等待：MQTT 线程和主线程同时写库时自动排队，而不是直接返回 SQLITE_BUSY
    sqlite3_busy_timeout(db_, 3000);

    exec("PRAGMA journal_mode=WAL;");       // 读写并发更好，掉电安全性也够用
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA foreign_keys=ON;");

    if (!createTables())
    {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

// ============================================================================
//  关闭数据库
// ============================================================================
void DBManager::close()
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ============================================================================
//  建表
// ============================================================================
bool DBManager::createTables()
{
    // 已经是递归锁，这里可以直接调用 exec（内部再取一次锁也没问题）
    return exec(kSchemaSql);
}

// ============================================================================
//  执行 SQL
// ============================================================================
bool DBManager::exec(const std::string& sql)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_)
    {
        lastError_ = "数据库未打开";
        return false;
    }

    char* errMsg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        lastError_ = errMsg ? errMsg : "SQL 执行失败";
        if (errMsg)
            sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// ============================================================================
//  新增 / 更新人员特征
// ============================================================================
bool DBManager::upsertPerson(const std::string& name, const cv::Mat& feature)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    if (name.empty())
    {
        lastError_ = "姓名为空，拒绝注册";
        return false;
    }

    cv::Mat f = asFeatureMat(feature);
    if (f.empty())
    {
        lastError_ = "特征格式非法（需要 128 维 float）";
        return false;
    }

    if (!db_)
    {
        lastError_ = "数据库未打开";
        return false;
    }

    // 放进一个事务里，保证「更新/插入」原子完成
    if (!exec("BEGIN IMMEDIATE;"))
        return false;

    bool ok = false;

    // ---- 第一步：按姓名更新 ----
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "UPDATE Person SET feature = ?, updated_at = datetime('now','localtime') WHERE name = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            // SQLITE_STATIC 是安全的：f 在本作用域内始终存活，且 step 会立即使用数据
            sqlite3_bind_blob(stmt, 1, f.data, static_cast<int>(kFeatureBytes), SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            if (!ok)
                lastError_ = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
        }
        else
        {
            lastError_ = sqlite3_errmsg(db_);
        }
    }

    // ---- 第二步：没有更新到任何行，说明是新人员 -> INSERT ----
    if (ok && sqlite3_changes(db_) == 0)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO Person(name, feature) VALUES(?, ?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(stmt, 2, f.data, static_cast<int>(kFeatureBytes), SQLITE_STATIC);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            if (!ok)
                lastError_ = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
        }
        else
        {
            ok = false;
            lastError_ = sqlite3_errmsg(db_);
        }
    }

    exec(ok ? "COMMIT;" : "ROLLBACK;");
    return ok;
}

// ============================================================================
//  读取全部人员
// ============================================================================
std::vector<PersonRecord> DBManager::getAllPersons(std::string* err)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    std::vector<PersonRecord> out;
    if (!db_)
    {
        lastError_ = "数据库未打开";
        if (err)
            *err = lastError_;
        return out;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, feature, created_at FROM Person ORDER BY id;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(db_);
        if (err)
            *err = lastError_;
        return out;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        PersonRecord p;
        p.id = sqlite3_column_int64(stmt, 0);

        const unsigned char* name = sqlite3_column_text(stmt, 1);
        p.name = name ? reinterpret_cast<const char*>(name) : "";

        const void* blob  = sqlite3_column_blob(stmt, 2);
        const int   bytes = sqlite3_column_bytes(stmt, 2);

        // 特征长度必须严格是 512 字节，否则视为坏数据跳过（不影响其它人打卡）
        if (!blob || bytes != static_cast<int>(kFeatureBytes))
        {
            std::cerr << "[WARN] 跳过一条特征长度异常的人员记录: " << p.name
                      << " (" << bytes << " 字节)" << std::endl;
            continue;
        }

        cv::Mat f(1, kFeatureDim, CV_32F);
        std::memcpy(f.data, blob, kFeatureBytes);
        p.feature = f;

        const unsigned char* ts = sqlite3_column_text(stmt, 3);
        p.createdAt = ts ? reinterpret_cast<const char*>(ts) : "";

        out.push_back(p);
    }

    sqlite3_finalize(stmt);
    return out;
}

// ============================================================================
//  人员数量
// ============================================================================
int DBManager::countPersons()
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_)
        return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM Person;", -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

// ============================================================================
//  按姓名删除人员（来自 MQTT terminal/delete）
//
//  ★ 为什么不能直接 DELETE：
//    Attendance.person_id 上有外键 REFERENCES Person(id)，而打开数据库时执行了
//    PRAGMA foreign_keys=ON。只要这个人有打卡历史，直接
//        DELETE FROM Person WHERE id = ?
//    就会报 "FOREIGN KEY constraint failed"，删除失败。
//    所以先把历史打卡的 person_id 置空（打卡记录本身保留，姓名还在
//    Attendance.name 里，考勤历史不丢），再删 Person 行；两条语句放在
//    同一个事务里，避免删一半留下脏数据。
// ============================================================================
bool DBManager::deletePerson(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    if (!db_)
    {
        lastError_ = "数据库未打开";
        return false;
    }
    if (name.empty())
    {
        lastError_ = "姓名为空";
        return false;
    }

    if (!exec("BEGIN IMMEDIATE;"))
        return false;

    // ---- 1) 按姓名查 id ----
    long long id = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id FROM Person WHERE name = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            lastError_ = sqlite3_errmsg(db_);
            exec("ROLLBACK;");
            return false;
        }
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (id <= 0)
    {
        lastError_ = "没有这个人员: " + name;
        exec("ROLLBACK;");
        return false;
    }

    // ---- 2) 历史打卡先解除外键引用 ----
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "UPDATE Attendance SET person_id = NULL WHERE person_id = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        else
        {
            lastError_ = sqlite3_errmsg(db_);
            exec("ROLLBACK;");
            return false;
        }
    }

    // ---- 3) 删人员 ----
    bool ok = false;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "DELETE FROM Person WHERE id = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, id);
            ok = (sqlite3_step(stmt) == SQLITE_DONE);
            if (!ok)
                lastError_ = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
        }
        else
        {
            lastError_ = sqlite3_errmsg(db_);
        }
    }

    exec(ok ? "COMMIT;" : "ROLLBACK;");
    return ok;
}

// ============================================================================
//  写打卡记录
// ============================================================================
bool DBManager::addAttendance(const std::string& name,
                              const std::string& checkTime,
                              long long          epoch,
                              double             score,
                              long long          personId)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_)
    {
        lastError_ = "数据库未打开";
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO Attendance(person_id, name, check_time, epoch, score) VALUES(?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }

    if (personId > 0)
        sqlite3_bind_int64(stmt, 1, personId);
    else
        sqlite3_bind_null(stmt, 1);

    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, checkTime.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, epoch);
    sqlite3_bind_double(stmt, 5, score);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok)
        lastError_ = sqlite3_errmsg(db_);

    sqlite3_finalize(stmt);
    return ok;
}

// ============================================================================
//  最近一次打卡时间（epoch）
// ============================================================================
long long DBManager::lastCheckInEpoch(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_)
        return 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COALESCE(MAX(epoch), 0) FROM Attendance WHERE name = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    long long last = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        last = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    return last;
}

// ============================================================================
//  最近 N 条打卡记录
// ============================================================================
std::vector<AttendanceRecord> DBManager::queryRecentAttendance(int limit)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    std::vector<AttendanceRecord> out;
    if (!db_)
        return out;
    if (limit <= 0)
        limit = 10;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, check_time, epoch, score FROM Attendance ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return out;

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        AttendanceRecord r;
        r.id = sqlite3_column_int64(stmt, 0);

        const unsigned char* name = sqlite3_column_text(stmt, 1);
        r.name = name ? reinterpret_cast<const char*>(name) : "";

        const unsigned char* ct = sqlite3_column_text(stmt, 2);
        r.checkTime = ct ? reinterpret_cast<const char*>(ct) : "";

        r.epoch = sqlite3_column_int64(stmt, 3);
        r.score = sqlite3_column_double(stmt, 4);
        out.push_back(r);
    }

    sqlite3_finalize(stmt);
    return out;
}

// ============================================================================
//  错误信息
// ============================================================================
std::string DBManager::lastError()
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return lastError_;
}

void DBManager::setError(const std::string& msg)
{
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    lastError_ = msg;
}
