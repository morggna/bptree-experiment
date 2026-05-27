# B+树索引实验报告材料与关键代码

整理者：助手（Codex）
项目路径：`/Users/xuer/MONA/bptree-experiment`
数据集：GEOLIFE 轨迹数据集示例
索引对象：轨迹点时间戳（Unix timestamp，秒）
页大小：8 KB

---

## 1. 实验目标

本实验实现一个基于磁盘页的 B+树索引，用于对 GEOLIFE 轨迹数据集中的轨迹点按时间戳建立索引，并测试不同时间范围查询下的响应时间、逻辑 IO 次数和命中记录数。

核心目标包括：

1. 设计定长页结构，将 B+树节点以 8 KB 页存储在索引文件中。
2. 实现 B+树插入、节点分裂、父节点维护、范围查询等核心操作。
3. 解析 GEOLIFE `.plt` 轨迹文件，将时间戳作为 key，纬度、经度、高度作为记录内容。
4. 随机生成四类时间范围查询，比较不同查询范围下的性能差异。
5. 使用 Python 对实验结果进行可视化分析。

---

## 2. 实验整体流程

程序运行流程如下：

1. 递归扫描数据集目录，收集所有 `.plt` 文件。
2. 创建新的 B+树索引文件 `geolife.idx`。
3. 逐行解析轨迹文件，将每个轨迹点转换成 `Record` 并插入 B+树。
4. 记录全局最小时间戳 `gmin` 和最大时间戳 `gmax`。
5. 按总时间跨度生成四类范围查询：
   - `1/1000`
   - `1/10000`
   - `1/100000`
   - `1/1000000`
6. 每类随机生成 100 个查询，统计：
   - 查询响应时间
   - 逻辑 IO 次数
   - 命中记录数
7. 输出 `query_results.csv` 和 `query_summary.csv`。
8. 使用 `visualize.py` 生成 `bptree_performance.png`。

---

## 3. B+树页结构设计

B+树以磁盘页为基本读写单位。每页大小固定为 8192 字节。页类型分为：

- `PAGE_META`：元数据页，固定为页 0。
- `PAGE_INTERNAL`：内部节点页。
- `PAGE_LEAF`：叶子节点页。

关键参数如下：

```cpp
static const int     PAGE_SIZE         = 8192;
static const int     INTERNAL_MAX_KEYS = 510;
static const int     LEAF_MAX_KEYS     = 255;
static const int     INTERNAL_MIN_KEYS = 255;
static const int     LEAF_MIN_KEYS     = 128;
static const int64_t NULL_PG           = -1LL;

enum : int32_t { PAGE_META = 0, PAGE_INTERNAL = 1, PAGE_LEAF = 2 };
```

每条记录固定为 32 字节：

```cpp
struct Record {
    int64_t key;   // Unix 时间戳（秒）
    double  lat;
    double  lon;
    int32_t alt;
    int32_t _pad;
};
static_assert(sizeof(Record) == 32, "Record must be 32 bytes");
```

页内布局由 `PageBuf` 封装，使用固定偏移访问页头、内部节点数组、叶节点记录数组和元数据字段。

```cpp
struct PageBuf {
    char data[PAGE_SIZE];

    int32_t  type()   const { return *(const int32_t*)(data +  0); }
    int32_t& type()         { return *(      int32_t*)(data +  0); }
    int32_t  count()  const { return *(const int32_t*)(data +  4); }
    int32_t& count()        { return *(      int32_t*)(data +  4); }
    int64_t  parent() const { return *(const int64_t*)(data +  8); }
    int64_t& parent()       { return *(      int64_t*)(data +  8); }

    int64_t*       i_children()       { return (      int64_t*)(data + 16);   }
    const int64_t* i_children() const { return (const int64_t*)(data + 16);   }
    int64_t*       i_keys()           { return (      int64_t*)(data + 4104); }
    const int64_t* i_keys()     const { return (const int64_t*)(data + 4104); }

    int64_t  next()   const { return *(const int64_t*)(data + 16); }
    int64_t& next()         { return *(      int64_t*)(data + 16); }
    Record*       records()       { return (      Record*)(data + 24); }
    const Record* records() const { return (const Record*)(data + 24); }

    int64_t  root_pg()   const { return *(const int64_t*)(data +  8); }
    int64_t& root_pg()         { return *(      int64_t*)(data +  8); }
    int64_t  pg_count()  const { return *(const int64_t*)(data + 16); }
    int64_t& pg_count()        { return *(      int64_t*)(data + 16); }
    int64_t  rec_count() const { return *(const int64_t*)(data + 24); }
    int64_t& rec_count()       { return *(      int64_t*)(data + 24); }
};
static_assert(sizeof(PageBuf) == PAGE_SIZE, "PageBuf size mismatch");
```

这种设计的特点是：

- 所有节点都按页号定位。
- 页号乘以 `PAGE_SIZE` 即为文件偏移。
- 内部节点保存 key 和子页号。
- 叶子节点保存实际记录，并通过 `next_pgno` 构成链表，便于范围查询。
- 页 0 记录根页号、总页数和总记录数。

---

## 4. B+树类接口

B+树对外提供打开、关闭、插入、删除、单点查询、范围查询等接口。

```cpp
class BPTree {
public:
    BPTree();
    ~BPTree();

    bool open(const std::string& path, bool create = false);
    void close();
    bool is_open() const { return fp_ != nullptr; }

    bool insert(int64_t key, double lat, double lon, int32_t alt);
    bool remove(int64_t key);
    bool search(int64_t key, Record& out, int* io_count = nullptr);
    int  range_query(int64_t key_min, int64_t key_max,
                     std::vector<Record>& results, int& io_count);

    int64_t total_records() const { return meta_.rec_count(); }
    int64_t total_pages()   const { return meta_.pg_count();  }

private:
    FILE*   fp_;
    PageBuf meta_;
    bool    meta_dirty_;

    void    flush_meta();
    int64_t alloc_page();
    void    read_page (int64_t pgno, PageBuf& pg);
    void    write_page(int64_t pgno, const PageBuf& pg);

    int64_t find_leaf(int64_t key, int& io_cnt);
    void    insert_record_in_leaf(PageBuf& leaf, const Record& rec);
    void    insert_into_parent(int64_t left_pgno, int64_t sep_key, int64_t right_pgno);
    void    delete_entry(int64_t pgno, int64_t key, int64_t ptr_to_remove);
};
```

---

## 5. 索引文件创建与页读写

索引文件采用二进制方式打开。创建新索引时，页 0 被初始化为元数据页。

```cpp
bool BPTree::open(const std::string& path, bool create) {
    if (fp_) close();

    if (create) {
        fp_ = fopen(path.c_str(), "w+b");
        if (!fp_) return false;

        memset(&meta_, 0, sizeof(meta_));
        meta_.type()      = PAGE_META;
        meta_.root_pg()   = NULL_PG;
        meta_.pg_count()  = 1;
        meta_.rec_count() = 0;
        write_page(0, meta_);
    } else {
        fp_ = fopen(path.c_str(), "r+b");
        if (!fp_) return false;
        read_page(0, meta_);
    }
    meta_dirty_ = false;
    return true;
}
```

页分配和读写代码：

```cpp
int64_t BPTree::alloc_page() {
    int64_t pgno = meta_.pg_count();
    meta_.pg_count()++;
    meta_dirty_ = true;
    PageBuf blank;
    memset(&blank, 0, sizeof(blank));
    write_page(pgno, blank);
    return pgno;
}

void BPTree::read_page(int64_t pgno, PageBuf& pg) {
    fseek(fp_, (long)(pgno * PAGE_SIZE), SEEK_SET);
    fread(pg.data, PAGE_SIZE, 1, fp_);
}

void BPTree::write_page(int64_t pgno, const PageBuf& pg) {
    fseek(fp_, (long)(pgno * PAGE_SIZE), SEEK_SET);
    fwrite(pg.data, PAGE_SIZE, 1, fp_);
}
```

---

## 6. 查找叶子节点

插入时使用 `find_leaf` 定位 key 应落入的叶子节点。每读取一页，`io_cnt` 加一，用于统计逻辑页访问次数。

```cpp
int64_t BPTree::find_leaf(int64_t key, int& io_cnt) {
    io_cnt = 0;
    int64_t pgno = meta_.root_pg();
    if (pgno == NULL_PG) return NULL_PG;

    PageBuf pg;
    while (true) {
        read_page(pgno, pg);
        io_cnt++;
        if (pg.type() == PAGE_LEAF) return pgno;

        int n = pg.count();
        const int64_t* keys = pg.i_keys();
        const int64_t* chld = pg.i_children();
        int i = 0;
        while (i < n && key >= keys[i]) i++;
        pgno = chld[i];
    }
}
```

注意：范围查询和单点查询为了正确处理重复时间戳，使用了“遇到相等分隔键时优先走左侧”的逻辑，即：

```cpp
while (i < n && key_min > keys[i]) i++;
```

这样可以避免在重复 key 跨叶节点时，从右侧叶子开始查询而漏掉左侧重复记录。

---

## 7. 单点查询

单点查询会先定位到可能包含目标 key 的最左侧叶子，然后沿叶子链表向右扫描，直到找到记录或遇到更大的 key。

```cpp
bool BPTree::search(int64_t key, Record& out, int* io_count) {
    int io = 0;
    int64_t pgno = meta_.root_pg();
    if (pgno == NULL_PG) { if (io_count) *io_count = io; return false; }

    PageBuf leaf;
    while (true) {
        read_page(pgno, leaf);
        io++;
        if (leaf.type() == PAGE_LEAF) break;

        int n = leaf.count();
        const int64_t* keys = leaf.i_keys();
        const int64_t* chld = leaf.i_children();
        int i = 0;
        while (i < n && key > keys[i]) i++;
        pgno = chld[i];
    }

    int64_t leaf_pgno = pgno;
    while (leaf_pgno != NULL_PG) {
        read_page(leaf_pgno, leaf);
        io++;

        int n = leaf.count();
        const Record* recs = leaf.records();
        for (int i = 0; i < n; i++) {
            if (recs[i].key == key) {
                out = recs[i];
                if (io_count) *io_count = io;
                return true;
            }
            if (recs[i].key > key) {
                if (io_count) *io_count = io;
                return false;
            }
        }
        leaf_pgno = leaf.next();
    }
    if (io_count) *io_count = io;
    return false;
}
```

---

## 8. 范围查询

范围查询是本实验最重要的操作。它分两阶段执行：

1. 从根节点下钻到 `key_min` 所在的起始叶节点。
2. 沿叶节点链表向后顺序扫描，收集 `[key_min, key_max]` 范围内的记录。

```cpp
int BPTree::range_query(int64_t key_min, int64_t key_max,
                        std::vector<Record>& results, int& io_count) {
    results.clear();
    io_count = 0;
    if (meta_.root_pg() == NULL_PG) return 0;

    int64_t pgno = meta_.root_pg();
    PageBuf pg;
    while (true) {
        read_page(pgno, pg);
        io_count++;
        if (pg.type() == PAGE_LEAF) break;
        int n = pg.count();
        const int64_t* keys = pg.i_keys();
        const int64_t* chld = pg.i_children();
        int i = 0;
        while (i < n && key_min > keys[i]) i++;
        pgno = chld[i];
    }

    while (true) {
        int n = pg.count();
        const Record* recs = pg.records();
        bool past = false;
        for (int i = 0; i < n; i++) {
            if (recs[i].key > key_max) { past = true; break; }
            if (recs[i].key >= key_min) results.push_back(recs[i]);
        }
        if (past) break;
        pgno = pg.next();
        if (pgno == NULL_PG) break;
        read_page(pgno, pg);
        io_count++;
    }
    return (int)results.size();
}
```

范围查询的 IO 次数由两部分组成：

- 树高路径上的页读取次数。
- 叶节点链表顺序扫描的页读取次数。

因此查询范围越大，命中记录越多，通常需要扫描的叶子页越多，平均 IO 次数也会增加。

---

## 9. 叶节点插入

当叶节点未满时，直接在页内按 key 有序插入记录。

```cpp
void BPTree::insert_record_in_leaf(PageBuf& leaf, const Record& rec) {
    int n = leaf.count();
    Record* recs = leaf.records();
    int i = n;
    while (i > 0 && recs[i - 1].key > rec.key) {
        recs[i] = recs[i - 1];
        i--;
    }
    recs[i] = rec;
    leaf.count()++;
}
```

---

## 10. B+树插入与叶节点分裂

插入首先处理空树情况；如果根不存在，则创建第一个叶节点作为根。

```cpp
if (meta_.root_pg() == NULL_PG) {
    int64_t pgno = alloc_page();
    PageBuf leaf;
    memset(&leaf, 0, sizeof(leaf));
    leaf.type()   = PAGE_LEAF;
    leaf.count()  = 0;
    leaf.parent() = NULL_PG;
    leaf.next()   = NULL_PG;
    insert_record_in_leaf(leaf, rec);
    write_page(pgno, leaf);
    meta_.root_pg() = pgno;
    meta_.rec_count()++;
    meta_dirty_ = true;
    flush_meta();
    return true;
}
```

如果目标叶节点未满，则直接插入：

```cpp
if (leaf.count() < LEAF_MAX_KEYS) {
    insert_record_in_leaf(leaf, rec);
    write_page(leaf_pgno, leaf);
}
```

如果叶节点已满，则将原有 255 条记录和新记录合并为 256 条临时记录，再拆分为左右两个叶节点。

```cpp
Record tmp[LEAF_MAX_KEYS + 1];
int n = leaf.count();
const Record* recs = leaf.records();
int ins = n;
for (int i = 0; i < n; i++) {
    if (recs[i].key > key) { ins = i; break; }
}
for (int i = 0; i < ins;  i++) tmp[i]     = recs[i];
tmp[ins] = rec;
for (int i = ins; i < n; i++) tmp[i + 1]  = recs[i];

int total     = n + 1;
int left_cnt  = (total + 1) / 2;
int right_cnt = total - left_cnt;

int64_t new_pgno = alloc_page();
PageBuf new_leaf;
memset(&new_leaf, 0, sizeof(new_leaf));
new_leaf.type()   = PAGE_LEAF;
new_leaf.count()  = right_cnt;
new_leaf.parent() = leaf.parent();
new_leaf.next()   = leaf.next();

leaf.count() = left_cnt;
leaf.next()  = new_pgno;

memcpy(leaf.records(),     tmp,             left_cnt  * sizeof(Record));
memcpy(new_leaf.records(), tmp + left_cnt,  right_cnt * sizeof(Record));

write_page(leaf_pgno, leaf);
write_page(new_pgno, new_leaf);

int64_t separator = new_leaf.records()[0].key;
insert_into_parent(leaf_pgno, separator, new_pgno);
```

叶节点分裂时，右叶的第一个 key 会作为分隔键上推到父节点，但该 key 仍保留在右叶中，这是 B+树区别于 B 树的重要特征。

---

## 11. 父节点插入与内部节点分裂

叶节点或内部节点分裂后，需要把分隔键插入父节点。如果原节点没有父节点，则创建新根。

```cpp
if (parent_pgno == NULL_PG) {
    int64_t new_root = alloc_page();
    PageBuf root_pg;
    memset(&root_pg, 0, sizeof(root_pg));
    root_pg.type()          = PAGE_INTERNAL;
    root_pg.count()         = 1;
    root_pg.parent()        = NULL_PG;
    root_pg.i_keys()[0]     = sep_key;
    root_pg.i_children()[0] = left_pgno;
    root_pg.i_children()[1] = right_pgno;
    write_page(new_root, root_pg);

    left.parent()  = new_root;
    right.parent() = new_root;
    write_page(left_pgno,  left);
    write_page(right_pgno, right);

    meta_.root_pg() = new_root;
    meta_dirty_ = true;
    return;
}
```

如果父节点未满，则直接在父节点中插入分隔键和右子指针。

```cpp
if (n < INTERNAL_MAX_KEYS) {
    for (int i = n;     i > pos;     i--) keys[i] = keys[i - 1];
    for (int i = n + 1; i > pos + 1; i--) chld[i] = chld[i - 1];
    keys[pos]     = sep_key;
    chld[pos + 1] = right_pgno;
    parent.count()++;
    write_page(parent_pgno, parent);

    right.parent() = parent_pgno;
    write_page(right_pgno, right);
}
```

如果父节点已满，则需要分裂内部节点，并将中间 key 上推到更高层。

```cpp
int64_t tmp_keys[INTERNAL_MAX_KEYS + 1];
int64_t tmp_chld[INTERNAL_MAX_KEYS + 2];
memcpy(tmp_keys, keys, n * sizeof(int64_t));
memcpy(tmp_chld, chld, (n + 1) * sizeof(int64_t));

for (int i = n;     i > pos;     i--) tmp_keys[i] = tmp_keys[i - 1];
for (int i = n + 1; i > pos + 1; i--) tmp_chld[i] = tmp_chld[i - 1];
tmp_keys[pos]     = sep_key;
tmp_chld[pos + 1] = right_pgno;

int total    = n + 1;
int left_cnt = (total - 1) / 2;
int64_t push_key = tmp_keys[left_cnt];
int right_cnt = total - left_cnt - 1;

parent.count() = left_cnt;
memcpy(parent.i_keys(),     tmp_keys,           left_cnt * sizeof(int64_t));
memcpy(parent.i_children(), tmp_chld,       (left_cnt + 1) * sizeof(int64_t));
write_page(parent_pgno, parent);

int64_t new_int_pgno = alloc_page();
PageBuf new_int;
memset(&new_int, 0, sizeof(new_int));
new_int.type()   = PAGE_INTERNAL;
new_int.count()  = right_cnt;
new_int.parent() = parent.parent();
memcpy(new_int.i_keys(),     tmp_keys + left_cnt + 1, right_cnt * sizeof(int64_t));
memcpy(new_int.i_children(), tmp_chld + left_cnt + 1, (right_cnt + 1) * sizeof(int64_t));
write_page(new_int_pgno, new_int);

for (int i = 0; i <= right_cnt; i++) {
    PageBuf child;
    read_page(new_int.i_children()[i], child);
    child.parent() = new_int_pgno;
    write_page(new_int.i_children()[i], child);
}

insert_into_parent(parent_pgno, push_key, new_int_pgno);
```

内部节点分裂时，中间 key 上推到父节点，不保留在左右内部节点中。

---

## 12. 删除、借位与合并

虽然本实验的性能测试主要关注插入和范围查询，但 B+树实现中也包含删除逻辑。删除后如果节点低于最小 key 数，会尝试从兄弟节点借记录或与兄弟节点合并。

叶节点删除：

```cpp
if (pg.type() == PAGE_LEAF) {
    int n = pg.count();
    Record* recs = pg.records();
    int del = -1;
    for (int i = 0; i < n; i++) {
        if (recs[i].key == key) { del = i; break; }
    }
    if (del < 0) return;
    for (int i = del; i < n - 1; i++) recs[i] = recs[i + 1];
    pg.count()--;
    write_page(pgno, pg);
}
```

根节点特殊处理：

```cpp
if (pgno == meta_.root_pg()) {
    PageBuf root;
    read_page(pgno, root);
    if (root.count() == 0) {
        if (root.type() == PAGE_INTERNAL) {
            int64_t new_root = root.i_children()[0];
            PageBuf nr;
            read_page(new_root, nr);
            nr.parent() = NULL_PG;
            write_page(new_root, nr);
            meta_.root_pg() = new_root;
            meta_dirty_ = true;
        } else {
            meta_.root_pg() = NULL_PG;
            meta_dirty_ = true;
        }
    }
    return;
}
```

叶节点合并：

```cpp
if (left_pg.type() == PAGE_LEAF) {
    int ln = left_pg.count();
    int rn = right_pg.count();
    memcpy(left_pg.records() + ln, right_pg.records(), rn * sizeof(Record));
    left_pg.count() = ln + rn;
    left_pg.next()  = right_pg.next();
    write_page(left_pgno, left_pg);
}
```

删除接口：

```cpp
bool BPTree::remove(int64_t key) {
    PageBuf leaf;
    int64_t pgno = meta_.root_pg();
    if (pgno == NULL_PG) return false;

    while (true) {
        read_page(pgno, leaf);
        if (leaf.type() == PAGE_LEAF) break;

        int n = leaf.count();
        const int64_t* keys = leaf.i_keys();
        const int64_t* chld = leaf.i_children();
        int i = 0;
        while (i < n && key > keys[i]) i++;
        pgno = chld[i];
    }

    int64_t leaf_pgno = pgno;
    bool found = false;
    while (leaf_pgno != NULL_PG) {
        read_page(leaf_pgno, leaf);
        int n = leaf.count();
        const Record* recs = leaf.records();
        for (int i = 0; i < n; i++) {
            if (recs[i].key == key) { found = true; break; }
            if (recs[i].key > key) return false;
        }
        if (found) break;
        leaf_pgno = leaf.next();
    }
    if (!found) return false;

    delete_entry(leaf_pgno, key, -1LL);
    meta_.rec_count()--;
    meta_dirty_ = true;
    flush_meta();
    return true;
}
```

---

## 13. GEOLIFE 数据解析

程序递归遍历目录，收集所有 `.plt` 文件。

```cpp
static void collect_plt_files(const std::string& dir, std::vector<std::string>& out) {
    DIR* dp = opendir(dir.c_str());
    if (!dp) return;
    struct dirent* ep;
    while ((ep = readdir(dp))) {
        if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) continue;
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
```

`.plt` 文件中的日期和时间字段被转换为 Unix 时间戳。

```cpp
static int64_t datetime_to_unix(const char* date_str, const char* time_str) {
    int year, month, day, hour, min, sec;
    sscanf(date_str, "%d-%d-%d", &year, &month, &day);
    sscanf(time_str, "%d:%d:%d", &hour, &min, &sec);
    if (month <= 2) { month += 12; year--; }
    int64_t jdn = (int64_t)365 * year + year / 4 - year / 100 + year / 400
                  + (153 * month - 457) / 5 + day + 1721119;
    int64_t days = jdn - 2440588LL;
    return days * 86400LL + hour * 3600 + min * 60 + sec;
}
```

解析轨迹点并插入 B+树：

```cpp
static int parse_plt_and_insert(const std::string& path, BPTree& tree,
                                 int64_t& gmin, int64_t& gmax) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return 0;

    char line[256];
    for (int i = 0; i < 6; i++) {
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    }

    int cnt = 0;
    double lat, lon, dummy;
    int    alt;
    char   date_str[32], time_str[32];
    char   days_str[32];

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%lf,%lf,%lf,%d,%s", &lat, &lon, &dummy, &alt, days_str) < 4)
            continue;

        char* p = line;
        int comma = 0;
        while (*p && comma < 5) { if (*p == ',') comma++; p++; }
        if (comma < 5) continue;
        sscanf(p, "%31[^,],%31s", date_str, time_str);

        for (char* q = time_str; *q; q++) {
            if (*q == '\r' || *q == '\n') { *q = 0; break; }
        }

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
```

---

## 14. 实验查询生成

程序将全局时间跨度划分为四种查询规模。每种规模随机生成 100 个查询。

```cpp
int64_t total_span = gmax - gmin;
const int64_t divisors[4] = { 1000LL, 10000LL, 100000LL, 1000000LL };
const char* cat_names[4] = { "1/1000", "1/10000", "1/100000", "1/1000000" };
const int QUERIES_PER_CAT = 100;

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
}
```

注意：这里的 `1/1000` 等类别表示查询时间区间长度占总时间跨度的比例，而不是命中记录数占总记录数的比例。

---

## 15. CSV 输出

详细查询结果输出为 `query_results.csv`。

```cpp
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
}
```

汇总结果输出为 `query_summary.csv`。

```cpp
FILE* csv = fopen("query_summary.csv", "w");
if (csv) {
    fprintf(csv, "category,avg_time_ms,max_time_ms,avg_io,avg_hits\n");
    for (int c = 0; c < 4; c++) {
        fprintf(csv, "%s,%.6f,%.6f,%.2f,%.2f\n",
                cat_names[c], avg_time[c], max_time[c], avg_io[c], avg_hits[c]);
    }
    fclose(csv);
}
```

---

## 16. Python 可视化代码

Python 脚本读取 CSV 文件，并生成五个图：

1. 各类别平均查询时间。
2. 各类别平均 IO 次数。
3. 各类别平均命中记录数。
4. 查询时间箱线图。
5. IO 次数与命中记录数散点图。

读取 CSV：

```python
def load_summary(path='query_summary.csv'):
    cats, avg_t, max_t, avg_io, avg_hits = [], [], [], [], []
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            cats.append(row['category'])
            avg_t.append(float(row['avg_time_ms']))
            max_t.append(float(row['max_time_ms']))
            avg_io.append(float(row['avg_io']))
            avg_hits.append(float(row['avg_hits']))
    return cats, avg_t, max_t, avg_io, avg_hits

def load_detail(path='query_results.csv'):
    data = {0: [], 1: [], 2: [], 3: []}
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            c = int(row['category'])
            data[c].append((
                float(row['time_ms']),
                int(row['io_count']),
                int(row['result_count'])
            ))
    return data
```

绘制柱状图：

```python
ax1 = fig.add_subplot(gs[0, 0])
bars = ax1.bar(x, avg_t, color=colors, edgecolor='white', linewidth=0.8)
ax1.set_xticks(x)
ax1.set_xticklabels(cat_labels, fontsize=9)
ax1.set_xlabel('覆盖范围类别', fontsize=10)
ax1.set_ylabel('平均响应时间 (ms)', fontsize=10)
ax1.set_title('① 各类别平均查询时间', fontsize=11, fontweight='bold')
for bar, val in zip(bars, avg_t):
    ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + max(avg_t)*0.01,
             f'{val:.4f}', ha='center', va='bottom', fontsize=8)
ax1.grid(axis='y', alpha=0.3)
```

绘制箱线图：

```python
ax4 = fig.add_subplot(gs[1, 0:2])
time_data = [[d[0] for d in detail[c]] for c in range(4)]
bp = ax4.boxplot(time_data, tick_labels=cat_labels, patch_artist=True,
                 medianprops=dict(color='black', linewidth=2),
                 whiskerprops=dict(linewidth=1.5),
                 capprops=dict(linewidth=1.5),
                 flierprops=dict(marker='o', markersize=3, alpha=0.5))
for patch, color in zip(bp['boxes'], colors):
    patch.set_facecolor(color)
    patch.set_alpha(0.7)
ax4.set_xlabel('覆盖范围类别', fontsize=10)
ax4.set_ylabel('查询响应时间 (ms)', fontsize=10)
ax4.set_title('④ 查询时间分布箱线图', fontsize=11, fontweight='bold')
ax4.grid(axis='y', alpha=0.3)
```

绘制 IO 与命中数散点图：

```python
ax5 = fig.add_subplot(gs[1, 2])
for c in range(4):
    ios  = [d[1] for d in detail[c]]
    hits = [d[2] for d in detail[c]]
    ax5.scatter(ios, hits, alpha=0.5, s=20, color=colors[c], label=cat_labels[c])
ax5.set_xlabel('IO块次数', fontsize=10)
ax5.set_ylabel('命中记录数', fontsize=10)
ax5.set_title('⑤ IO次数 vs 命中记录数', fontsize=11, fontweight='bold')
ax5.legend(title='覆盖范围', fontsize=8, title_fontsize=8)
ax5.grid(alpha=0.3)
```

保存图像：

```python
plt.savefig('bptree_performance.png', dpi=150, bbox_inches='tight')
print("图表已保存: bptree_performance.png")
if os.environ.get('BPTREE_SHOW_PLOT') == '1' and 'agg' not in matplotlib.get_backend().lower():
    plt.show()
```

---

## 17. 实验结果

本次实验运行结果如下：

| 查询类别 | 平均时间 ms | 最大时间 ms | 平均 IO 次数 | 平均命中数 |
|---|---:|---:|---:|---:|
| 1/1000 | 0.008908 | 0.057125 | 7.24 | 572.43 |
| 1/10000 | 0.003278 | 0.009917 | 3.54 | 66.43 |
| 1/100000 | 0.002546 | 0.004333 | 3.04 | 4.29 |
| 1/1000000 | 0.002501 | 0.003791 | 3.02 | 0.44 |

详细统计：

| 查询类别 | 命中数范围 | 0 命中次数 | IO 范围 | 时间范围 ms |
|---|---:|---:|---:|---:|
| 1/1000 | 0 ~ 4468 | 39 | 3 ~ 37 | 0.002166 ~ 0.057125 |
| 1/10000 | 0 ~ 654 | 78 | 3 ~ 8 | 0.002166 ~ 0.009917 |
| 1/100000 | 0 ~ 120 | 90 | 3 ~ 4 | 0.002125 ~ 0.004333 |
| 1/1000000 | 0 ~ 7 | 91 | 3 ~ 4 | 0.002166 ~ 0.003791 |

构建结果：

```text
共找到 417 个 .plt 文件
构建完成: 530694 条记录
时间范围: [1224730384, 1246779915]，跨度 22049531 秒
索引文件页数: 3968，约 31.0 MB
```

---

## 18. 结果分析

从结果看，查询范围越大，平均命中记录数越多，平均 IO 次数也越多。`1/1000` 查询平均命中 572.43 条记录，平均 IO 为 7.24；而 `1/1000000` 查询平均命中仅 0.44 条记录，平均 IO 为 3.02。

最小 IO 次数约为 3，说明当前 B+树高度约为 3 层，即一次非常窄的范围查询通常需要读取：

1. 根节点。
2. 中间内部节点。
3. 叶节点。

对于较大范围查询，还需要沿叶节点链表继续读取后续叶页，因此 IO 次数会上升。

部分查询命中 0 条记录是正常现象。GEOLIFE 轨迹数据并不是在整个时间范围内均匀连续分布，而是集中在用户出行时间段。随机生成的时间窗口可能落在没有轨迹点的时间区间，因此会出现大量 0 命中查询。

箱线图中 `1/1000` 类别存在明显离群点。这是因为较大的时间窗口更可能覆盖轨迹密集区，导致命中记录数和扫描叶页数明显增加。散点图中 IO 次数和命中记录数呈正相关，也符合范围查询的代价模型。

需要注意的是，响应时间的绝对值非常小，主要是因为操作系统页缓存发挥作用，不能直接等同于真实磁盘随机 IO 时间。因此实验报告中更应强调逻辑 IO 次数和趋势，而不是将毫秒值解释为物理磁盘访问延迟。

---

## 19. 实验结论

本实验通过 GEOLIFE 轨迹数据验证了 B+树索引在范围查询中的性能特征：

1. B+树能够通过多级索引快速定位范围查询的起始叶节点。
2. 叶节点链表结构适合顺序范围扫描。
3. 查询范围越大，命中记录越多，需要扫描的叶节点越多，逻辑 IO 次数也越高。
4. 由于轨迹数据在时间上分布不均匀，不同查询窗口的命中数差异较大。
5. 逻辑 IO 次数比响应时间更稳定，更适合作为评价索引查询代价的指标。

---

## 20. 报告写作建议

正式实验报告可以按以下结构组织：

1. 实验目的：说明为什么选择 B+树和时间戳索引。
2. 数据集介绍：说明 GEOLIFE 数据字段和 `.plt` 文件格式。
3. 索引结构设计：重点解释 8 KB 页、元数据页、内部节点、叶节点链表。
4. 核心算法：介绍插入、叶节点分裂、内部节点分裂、范围查询。
5. 实验设计：说明四类范围查询的生成方式和统计指标。
6. 实验结果：放入 `query_summary.csv` 表格和可视化图。
7. 结果分析：重点讨论 IO 次数、命中数、离群点和数据稀疏性。
8. 总结：说明 B+树适合范围查询，以及本实验的局限性。

---

助手标注：本 Markdown 文件由助手根据当前项目源码、运行结果和调试过程整理生成，可作为撰写正式实验报告的代码与分析材料。
