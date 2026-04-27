// main.cpp  ——  GEOLIFE 数据集 B+树索引构建与性能测试
#include "bptree.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>

// MinGW 同样支持 POSIX dirent.h，统一使用
#include <dirent.h>
#include <sys/stat.h>

// ============================================================
//  工具：递归遍历目录，收集所有 .plt 文件
// ============================================================
static void collect_plt_files(const std::string& dir, std::vector<std::string>& out) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) return;
    struct dirent* ep;
    while ((ep = readdir(dp))) {
        if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) continue;
        // 兼容 Windows 路径分隔符
        std::string full = dir + "/" + ep->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_plt_files(full, out);
        } else {
            size_t len = strlen(ep->d_name);
            if (len > 4 && strcmp(ep->d_name + len - 4, ".plt") == 0)
                out.push_back(full);
        }
    }
    closedir(dp);
}

// ============================================================
//  工具：将日期/时间字符串转 Unix 时间戳（UTC，秒）
//  date_str: "YYYY-MM-DD"   time_str: "HH:MM:SS"
// ============================================================
static int64_t datetime_to_unix(const char* date_str, const char* time_str) {
    int year, month, day, hour, min, sec;
    sscanf(date_str, "%d-%d-%d", &year, &month, &day);
    sscanf(time_str, "%d:%d:%d", &hour, &min, &sec);
    // Zeller 公式变体计算距 1970-01-01 的天数
    if (month <= 2) { month += 12; year--; }
    int64_t jdn = (int64_t)365 * year + year / 4 - year / 100 + year / 400
                  + (153 * month - 457) / 5 + day + 1721119;
    int64_t days = jdn - 2440588LL; // JDN(1970-01-01) = 2440588
    return days * 86400LL + hour * 3600 + min * 60 + sec;
}

// ============================================================
//  解析单个 .plt 文件，将轨迹点插入 B+树
//  返回插入的记录数
// ============================================================
static int parse_plt_and_insert(const std::string& path, BPTree& tree,
                                 int64_t& gmin, int64_t& gmax) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return 0;

    char line[256];
    // 跳过 6 行头
    for (int i = 0; i < 6; i++) {
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    }

    int cnt = 0;
    double lat, lon, dummy;
    int    alt;
    char   date_str[32], time_str[32];
    char   days_str[32];

    while (fgets(line, sizeof(line), fp)) {
        // 格式: lat,lon,0,altitude,days_since_1900,YYYY-MM-DD,HH:MM:SS
        if (sscanf(line, "%lf,%lf,%lf,%d,%s", &lat, &lon, &dummy, &alt, days_str) < 4)
            continue;
        // 提取日期和时间（最后两个逗号分隔字段）
        char* p = line;
        int comma = 0;
        while (*p && comma < 5) { if (*p == ',') comma++; p++; }
        if (comma < 5) continue;
        sscanf(p, "%31[^,],%31s", date_str, time_str);
        // 去掉 time_str 末尾换行
        for (char* q = time_str; *q; q++) if (*q == '\r' || *q == '\n') { *q = 0; break; }

        int64_t ts = datetime_to_unix(date_str, time_str);
        if (ts <= 0) continue;

        tree.insert(ts, lat, lon, alt);
        cnt++;
        if (ts < gmin) gmin = ts;
        if (ts > gmax) gmax = ts;
    }
    fclose(fp);
    return cnt;
}

// ============================================================
//  高精度计时（毫秒，double）
// ============================================================
static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(
        high_resolution_clock::now().time_since_epoch()).count();
}

// ============================================================
//  主程序
// ============================================================
int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);  // 设置控制台输出为 UTF-8，解决中文乱码
    SetConsoleCP(65001);
#endif
    // 默认数据集路径（可通过命令行参数 argv[1] 覆盖）
    // Git Bash 路径格式：使用正斜杠
    const char* default_dir =
        "C:/Users/\xe9\x9b\xaa\xe9\xa5\xb5/iCloudDrive"
        "/\xe6\x95\x99\xe8\x82\xb2/\xe5\xae\x9e\xe9\xaa\x8c\xe6\x8a\xa5\xe5\x91\x8a"
        "/\xe6\xaf\x95\xe4\xb8\x9a\xe8\xae\xbe\xe8\xae\xa1/\xe7\xa8\x8b\xe5\xba\x8f"
        "/GEOLIFE\xe8\xbd\xa8\xe6\x8a\x80\xe6\x95\xb0\xe6\x8d\xae\xe9\x9b\x86\xe7\xa4\xba\xe4\xbe\x8b";
    std::string data_dir = default_dir;
    std::string idx_file = "geolife.idx";

    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) idx_file = argv[2];

    printf("=================================================\n");
    printf("  B+\u6811\u7d22\u5f15\u6027\u80fd\u6d4b\u8bd5  (IO\u5757\u5927\u5c0f = %d KB)\n", PAGE_SIZE / 1024);
    printf("=================================================\n\n");

    // --------------------------------------------------------
    //  (1) 构建索引
    // --------------------------------------------------------
    printf("[1] \u6536\u96c6 .plt \u6587\u4ef6...\n");
    std::vector<std::string> plt_files;
    collect_plt_files(data_dir, plt_files);
    std::sort(plt_files.begin(), plt_files.end());
    printf("    \u5171\u627e\u5230 %zu \u4e2a .plt \u6587\u4ef6\n", plt_files.size());

    BPTree tree;
    if (!tree.open(idx_file, true)) {
        fprintf(stderr, "\u65e0\u6cd5\u521b\u5efa\u7d22\u5f15\u6587\u4ef6: %s\n", idx_file.c_str());
        return 1;
    }

    int64_t gmin = INT64_MAX, gmax = INT64_MIN;
    int total_recs = 0;
    double t0 = now_ms();

    printf("[2] \u89e3\u6790\u5e76\u63d2\u5165\u8bb0\u5f55...\n");
    for (size_t i = 0; i < plt_files.size(); i++) {
        int n = parse_plt_and_insert(plt_files[i], tree, gmin, gmax);
        total_recs += n;
        if ((i + 1) % 50 == 0 || i + 1 == plt_files.size())
            printf("    \u5df2\u5904\u7406 %zu/%zu \u6587\u4ef6, \u5171 %d \u6761\u8bb0\u5f55\r", i+1, plt_files.size(), total_recs);
    }
    printf("\n");
    double build_ms = now_ms() - t0;

    printf("    \u6784\u5efa\u5b8c\u6210: %d \u6761\u8bb0\u5f55, %.1f ms\n", total_recs, build_ms);
    printf("    \u65f6\u95f4\u8303\u56f4: [%lld, %lld] (%lld \u79d2)\n",
           (long long)gmin, (long long)gmax, (long long)(gmax - gmin));
    printf("    \u7d22\u5f15\u6587\u4ef6\u9875\u6570: %lld (%.1f MB)\n",
           (long long)tree.total_pages(),
           tree.total_pages() * PAGE_SIZE / 1024.0 / 1024.0);

    // --------------------------------------------------------
    //  (3) 随机生成范围查询
    // --------------------------------------------------------
    printf("\n[3] \u751f\u6210\u8303\u56f4\u67e5\u8be2...\n");

    if (gmin >= gmax) {
        fprintf(stderr, "\u65f6\u95f4\u8303\u56f4\u65e0\u6548\uff01\n");
        tree.close();
        return 1;
    }

    int64_t total_span = gmax - gmin;
    // 4类覆盖比例：1/1000, 1/10000, 1/100000, 1/1000000
    const int64_t divisors[4] = { 1000LL, 10000LL, 100000LL, 1000000LL };
    const char* cat_names[4] = { "1/1000", "1/10000", "1/100000", "1/1000000" };
    const int QUERIES_PER_CAT = 100;

    struct QueryResult {
        int    category;    // 0-3
        int64_t qmin, qmax; // 查询区间
        int    result_cnt;  // 命中记录数
        double time_ms;     // 响应时间 (ms)
        int    io_count;    // IO 块次数
    };
    std::vector<QueryResult> results;
    results.reserve(400);

    std::mt19937_64 rng(42);

    for (int cat = 0; cat < 4; cat++) {
        int64_t range_len = total_span / divisors[cat];
        if (range_len < 1) range_len = 1;
        int64_t max_start = gmax - range_len;
        if (max_start < gmin) max_start = gmin;

        std::uniform_int_distribution<int64_t> dist(gmin, max_start);
        for (int q = 0; q < QUERIES_PER_CAT; q++) {
            int64_t qmin = dist(rng);
            int64_t qmax = qmin + range_len;

            std::vector<Record> hits;
            int io = 0;
            double t_start = now_ms();
            tree.range_query(qmin, qmax, hits, io);
            double elapsed = now_ms() - t_start;

            QueryResult r;
            r.category   = cat;
            r.qmin       = qmin;
            r.qmax       = qmax;
            r.result_cnt = (int)hits.size();
            r.time_ms    = elapsed;
            r.io_count   = io;
            results.push_back(r);
        }
        printf("    \u7c7b\u522b %s: %d \u4e2a\u67e5\u8be2\u5b8c\u6210\n", cat_names[cat], QUERIES_PER_CAT);
    }

    // --------------------------------------------------------
    //  统计输出
    // --------------------------------------------------------
    printf("\n[4] \u67e5\u8be2\u6027\u80fd\u7ed3\u679c\n");
    printf("%-12s %10s %10s %10s %10s\n",
           "\u7c7b\u522b", "\u5e73\u5747\u65f6\u95f4(ms)", "\u6700\u5927(ms)", "\u5e73\u5747IO\u6b21", "\u5e73\u5747\u547d\u4e2d\u6570");
    printf("%-12s %10s %10s %10s %10s\n",
           "----------", "----------", "----------", "----------", "----------");

    // 收集每类数据
    double avg_time[4]={}, max_time[4]={}, avg_io[4]={}, avg_hits[4]={};
    int    cnt_per_cat[4] = {};

    for (auto& r : results) {
        int c = r.category;
        cnt_per_cat[c]++;
        avg_time[c] += r.time_ms;
        avg_io  [c] += r.io_count;
        avg_hits[c] += r.result_cnt;
        if (r.time_ms > max_time[c]) max_time[c] = r.time_ms;
    }
    for (int c = 0; c < 4; c++) {
        avg_time[c] /= QUERIES_PER_CAT;
        avg_io  [c] /= QUERIES_PER_CAT;
        avg_hits[c] /= QUERIES_PER_CAT;
        printf("%-12s %10.4f %10.4f %10.1f %10.1f\n",
               cat_names[c], avg_time[c], max_time[c], avg_io[c], avg_hits[c]);
    }

    // --------------------------------------------------------
    //  输出 CSV（供 Python 可视化）
    // --------------------------------------------------------
    {
        FILE* csv = fopen("query_results.csv", "w");
        if (csv) {
            fprintf(csv, "category,query_id,range_start,range_end,result_count,time_ms,io_count\n");
            for (size_t i = 0; i < results.size(); i++) {
                auto& r = results[i];
                fprintf(csv, "%d,%zu,%lld,%lld,%d,%.6f,%d\n",
                        r.category, i,
                        (long long)r.qmin, (long long)r.qmax,
                        r.result_cnt, r.time_ms, r.io_count);
            }
            fclose(csv);
            printf("\n\u67e5\u8be2\u7ed3\u679c\u5df2\u5199\u5165: query_results.csv\n");
        }
    }

    // 输出汇总 CSV
    {
        FILE* csv = fopen("query_summary.csv", "w");
        if (csv) {
            fprintf(csv, "category,avg_time_ms,max_time_ms,avg_io,avg_hits\n");
            for (int c = 0; c < 4; c++) {
                fprintf(csv, "%s,%.6f,%.6f,%.2f,%.2f\n",
                        cat_names[c], avg_time[c], max_time[c], avg_io[c], avg_hits[c]);
            }
            fclose(csv);
            printf("\u6c47\u603b\u7ed3\u679c\u5df2\u5199\u5165: query_summary.csv\n");
        }
    }

    tree.close();
    printf("\n\u5b8c\u6210\u3002\u8fd0\u884c 'python visualize.py' \u751f\u6210\u56fe\u8868\u3002\n");
    return 0;
}
