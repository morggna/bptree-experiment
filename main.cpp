#include "bptree.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>

#include <dirent.h>
#include <sys/stat.h>

// 递归收集 GEOLIFE 数据目录下的 .plt 文件。
static void collect_plt_files(const std::string& dir,
                              std::vector<std::string>& files) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) {
        return;
    }

    struct dirent* ep;
    while ((ep = readdir(dp))) {
        if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) {
            continue;
        }

        std::string full = dir + "/" + ep->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_plt_files(full, files);
        } else {
            size_t len = strlen(ep->d_name);
            if (len > 4 && strcmp(ep->d_name + len - 4, ".plt") == 0) {
                files.push_back(full);
            }
        }
    }

    closedir(dp);
}

static bool directory_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 去掉 fgets 读入行末尾的换行符，方便后面解析时间字符串。
static void strip_line_end(char* text) {
    for (char* p = text; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
            return;
        }
    }
}

// 将 .plt 文件中的日期和时间字段转换为 Unix 时间戳。
static bool datetime_to_unix(const char* date_str, const char* time_str, Timestamp& out_timestamp) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;

    if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) != 3) {
        return false;
    }
    if (sscanf(time_str, "%d:%d:%d", &hour, &min, &sec) != 3) {
        return false;
    }

    if (month <= 2) {
        month += 12;
        year--;
    }

    Timestamp jdn = (Timestamp)365 * year + year / 4 - year / 100 + year / 400
                    + (153 * month - 457) / 5 + day + 1721119;
    Timestamp days = jdn - 2440588LL;
    out_timestamp = days * 86400LL + hour * 3600 + min * 60 + sec;
    return true;
}

// 读取一个 .plt 文件，将有效轨迹点直接插入 B+树索引。
static int parse_plt_and_insert(const std::string& path, BPTree& tree,
                                 Timestamp& min_ts, Timestamp& max_ts) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) {
        return 0;
    }

    char line[256];

    // GEOLIFE .plt 文件前 6 行是说明信息，真正的轨迹点从第 7 行开始。
    for (int i = 0; i < 6; i++) {
        if (!fgets(line, sizeof(line), fp)) {
            fclose(fp);
            return 0;
        }
    }

    int inserted = 0;
    double lat = 0.0;
    double lon = 0.0;
    double dummy = 0.0;
    int alt = 0;
    char date_str[32];
    char time_str[32];

    while (fgets(line, sizeof(line), fp)) {
        // 每行中纬度、经度、海拔、日期、时间会被转换成一条 Record。
        if (sscanf(line, "%lf,%lf,%lf,%d", &lat, &lon, &dummy, &alt) != 4) {
            continue;
        }

        char* p = line;
        int comma = 0;
        while (*p && comma < 5) {
            if (*p == ',') {
                comma++;
            }
            p++;
        }
        if (comma < 5) {
            continue;
        }

        if (sscanf(p, "%31[^,],%31s", date_str, time_str) != 2) {
            continue;
        }

        strip_line_end(time_str);

        Timestamp timestamp = 0;
        if (!datetime_to_unix(date_str, time_str, timestamp) || timestamp <= 0) {
            // 少量格式异常的行直接跳过，不影响整个数据集导入。
            continue;
        }

        tree.insert(timestamp, lat, lon, alt);
        inserted++;
        // 导入时顺便记录全局时间范围，后面用它生成随机查询窗口。
        if (timestamp < min_ts) {
            min_ts = timestamp;
        }
        if (timestamp > max_ts) {
            max_ts = timestamp;
        }
    }
    fclose(fp);
    return inserted;
}

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(
        high_resolution_clock::now().time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    std::string data_dir = "GEOLIFE轨技数据集示例";
    std::string index_path = "geolife.idx";

    if (argc >= 2) {
        data_dir = argv[1];
    } else if (!directory_exists(data_dir) && directory_exists("../GEOLIFE轨技数据集示例")) {
        data_dir = "../GEOLIFE轨技数据集示例";
    }
    if (argc >= 3) {
        index_path = argv[2];
    }

    printf("=================================================\n");
    printf("  B+\u6811\u7d22\u5f15\u6027\u80fd\u6d4b\u8bd5  (IO\u5757\u5927\u5c0f = %d KB)\n", PAGE_SIZE / 1024);
    printf("=================================================\n\n");

    printf("[1] \u6536\u96c6 .plt \u6587\u4ef6...\n");
    std::vector<std::string> plt_files;
    collect_plt_files(data_dir, plt_files);
    // 排序后每次实验的导入顺序一致，结果更容易复现。
    std::sort(plt_files.begin(), plt_files.end());
    printf("    数据目录: %s\n", data_dir.c_str());
    printf("    \u5171\u627e\u5230 %zu \u4e2a .plt \u6587\u4ef6\n", plt_files.size());

    if (plt_files.empty()) {
        fprintf(stderr,
                "没有找到 .plt 文件。请检查数据目录，或在 CLion Run Configuration 的 Program arguments 中传入数据集路径。\n"
                "示例: make run DATA=/path/to/GEOLIFE轨迹数据集示例\n");
        return 1;
    }

    BPTree tree;
    if (!tree.open(index_path, true)) {
        fprintf(stderr, "\u65e0\u6cd5\u521b\u5efa\u7d22\u5f15\u6587\u4ef6: %s\n", index_path.c_str());
        return 1;
    }

    Timestamp min_ts = INT64_MAX;
    Timestamp max_ts = INT64_MIN;
    int total_recs = 0;
    double build_start_ms = now_ms();

    printf("[2] \u89e3\u6790\u5e76\u63d2\u5165\u8bb0\u5f55...\n");
    for (size_t i = 0; i < plt_files.size(); i++) {
        int inserted = parse_plt_and_insert(plt_files[i], tree, min_ts, max_ts);
        total_recs += inserted;
        if ((i + 1) % 50 == 0 || i + 1 == plt_files.size()) {
            printf("    \u5df2\u5904\u7406 %zu/%zu \u6587\u4ef6, \u5171 %d \u6761\u8bb0\u5f55\r",
                   i + 1, plt_files.size(), total_recs);
        }
    }
    printf("\n");
    double build_ms = now_ms() - build_start_ms;

    printf("    \u6784\u5efa\u5b8c\u6210: %d \u6761\u8bb0\u5f55, %.1f ms\n", total_recs, build_ms);
    if (total_recs == 0 || min_ts >= max_ts) {
        fprintf(stderr, "没有解析到有效轨迹记录，无法生成范围查询。\n");
        tree.close();
        return 1;
    }
    printf("    \u65f6\u95f4\u8303\u56f4: [%lld, %lld] (%lld \u79d2)\n",
           (long long)min_ts,
           (long long)max_ts,
           (long long)(max_ts - min_ts));
    printf("    \u7d22\u5f15\u6587\u4ef6\u9875\u6570: %lld (%.1f MB)\n",
           (long long)tree.total_pages(),
           tree.total_pages() * PAGE_SIZE / 1024.0 / 1024.0);

    printf("\n[3] \u751f\u6210\u8303\u56f4\u67e5\u8be2...\n");

    Timestamp span = max_ts - min_ts;

    const Timestamp divisors[4] = { 1000LL, 10000LL, 100000LL, 1000000LL };
    const char* labels[4] = { "1/1000", "1/10000", "1/100000", "1/1000000" };
    const int QUERIES_PER_CAT = 100;

    // 保存单次查询结果，后面既打印汇总表，也写入 CSV 供可视化脚本使用。
    struct QueryResult {
        int cat;
        Timestamp q_min;
        Timestamp q_max;
        int hits;
        double time_ms;
        int io_count;
    };
    std::vector<QueryResult> results;
    results.reserve(400);

    std::mt19937_64 rng(42);

    for (int cat = 0; cat < 4; cat++) {
        // 四类查询窗口分别覆盖总时间跨度的 1/1000 到 1/1000000。
        Timestamp range_len = span / divisors[cat];
        if (range_len < 1) {
            range_len = 1;
        }

        Timestamp latest_start = max_ts - range_len;
        if (latest_start < min_ts) {
            latest_start = min_ts;
        }

        std::uniform_int_distribution<Timestamp> dist(min_ts, latest_start);
        for (int query_id = 0; query_id < QUERIES_PER_CAT; query_id++) {
            Timestamp q_min = dist(rng);
            Timestamp q_max = q_min + range_len;

            std::vector<Record> hits;
            int io_count = 0;
            double query_start_ms = now_ms();
            tree.range_query(q_min, q_max, hits, io_count);
            double elapsed_ms = now_ms() - query_start_ms;

            // time_ms 是运行耗时，io_count 是 B+树读取的逻辑页数。
            QueryResult r;
            r.cat = cat;
            r.q_min = q_min;
            r.q_max = q_max;
            r.hits = (int)hits.size();
            r.time_ms = elapsed_ms;
            r.io_count = io_count;
            results.push_back(r);
        }
        printf("    \u7c7b\u522b %s: %d \u4e2a\u67e5\u8be2\u5b8c\u6210\n",
               labels[cat], QUERIES_PER_CAT);
    }

    printf("\n[4] \u67e5\u8be2\u6027\u80fd\u7ed3\u679c\n");
    printf("%-12s %10s %10s %10s %10s\n",
           "\u7c7b\u522b", "\u5e73\u5747\u65f6\u95f4(ms)", "\u6700\u5927(ms)", "\u5e73\u5747IO\u6b21", "\u5e73\u5747\u547d\u4e2d\u6570");
    printf("%-12s %10s %10s %10s %10s\n",
           "----------", "----------", "----------", "----------", "----------");

    double avg_time[4]={}, max_time[4]={}, avg_io[4]={}, avg_hits[4]={};
    int counts[4] = {};

    for (const QueryResult& r : results) {
        int cat = r.cat;
        counts[cat]++;
        avg_time[cat] += r.time_ms;
        avg_io[cat] += r.io_count;
        avg_hits[cat] += r.hits;
        if (r.time_ms > max_time[cat]) {
            max_time[cat] = r.time_ms;
        }
    }
    for (int cat = 0; cat < 4; cat++) {
        if (counts[cat] == 0) {
            continue;
        }

        avg_time[cat] /= counts[cat];
        avg_io[cat] /= counts[cat];
        avg_hits[cat] /= counts[cat];
        printf("%-12s %10.4f %10.4f %10.1f %10.1f\n",
               labels[cat],
               avg_time[cat],
               max_time[cat],
               avg_io[cat],
               avg_hits[cat]);
    }

    {
        // query_results.csv 保存 400 次查询的明细。
        FILE* csv = fopen("query_results.csv", "w");
        if (!csv) {
            fprintf(stderr, "无法写入 query_results.csv\n");
            tree.close();
            return 1;
        }

        fprintf(csv, "category,query_id,range_start,range_end,result_count,time_ms,io_count\n");
        for (size_t i = 0; i < results.size(); i++) {
            const QueryResult& r = results[i];
            fprintf(csv, "%d,%zu,%lld,%lld,%d,%.6f,%d\n",
                    r.cat, i,
                    (long long)r.q_min, (long long)r.q_max,
                    r.hits, r.time_ms, r.io_count);
        }
        fclose(csv);
        printf("\n\u67e5\u8be2\u7ed3\u679c\u5df2\u5199\u5165: query_results.csv\n");
    }

    {
        // query_summary.csv 保存每类查询的平均值，直接用于绘图。
        FILE* csv = fopen("query_summary.csv", "w");
        if (!csv) {
            fprintf(stderr, "无法写入 query_summary.csv\n");
            tree.close();
            return 1;
        }

        fprintf(csv, "category,avg_time_ms,max_time_ms,avg_io,avg_hits\n");
        for (int cat = 0; cat < 4; cat++) {
            fprintf(csv, "%s,%.6f,%.6f,%.2f,%.2f\n",
                    labels[cat],
                    avg_time[cat],
                    max_time[cat],
                    avg_io[cat],
                    avg_hits[cat]);
        }
        fclose(csv);
        printf("\u6c47\u603b\u7ed3\u679c\u5df2\u5199\u5165: query_summary.csv\n");
    }

    tree.close();
    printf("\n\u5b8c\u6210\u3002\u8fd0\u884c 'make viz' \u751f\u6210\u56fe\u8868\u3002\n");
    return 0;
}
