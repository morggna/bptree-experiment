#include "bptree.h"
#include <cassert>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

BPTree::BPTree() : fp_(nullptr), meta_dirty_(false) {
    memset(&meta_, 0, sizeof(meta_));
}

BPTree::~BPTree() {
    close();
}

bool BPTree::open(const std::string& path, bool create) {
    if (fp_) close();

    if (create) {
        // 新建索引文件，初始化元数据页（页0）
        // 元数据页记录根页号、总页数、总记录数
        fp_ = fopen(path.c_str(), "w+b");
        if (!fp_) return false;

        memset(&meta_, 0, sizeof(meta_));
        meta_.type()      = PAGE_META;
        meta_.root_pg()   = NULL_PG;   // 还没有根节点
        meta_.pg_count()  = 1;         // 当前只有页0（元数据页）
        meta_.rec_count() = 0;
        write_page(0, meta_);          // 写入磁盘
    } else {
        // 打开已有索引文件，从磁盘读入元数据
        fp_ = fopen(path.c_str(), "r+b");
        if (!fp_) return false;
        read_page(0, meta_);
    }
    meta_dirty_ = false;
    return true;
}

void BPTree::close() {
    if (!fp_) return;
    flush_meta();
    fclose(fp_);
    fp_ = nullptr;
}

void BPTree::flush_meta() {
    if (meta_dirty_) {
        write_page(0, meta_);
        meta_dirty_ = false;
    }
}

// 在文件末尾分配一个新页，返回页号
// 页号 × PAGE_SIZE = 该页在文件中的字节偏移
int64_t BPTree::alloc_page() {
    int64_t pgno = meta_.pg_count();
    meta_.pg_count()++;
    meta_dirty_ = true;
    PageBuf blank;
    memset(&blank, 0, sizeof(blank));
    write_page(pgno, blank);   // 先把空页写入磁盘，占好位置
    return pgno;
}

// 从磁盘读一页到内存（外存 → 内存）
// pgno × PAGE_SIZE 定位文件偏移，读 8KB 到 pg.data
void BPTree::read_page(int64_t pgno, PageBuf& pg) {
    fseek(fp_, (long)(pgno * PAGE_SIZE), SEEK_SET);
    fread(pg.data, PAGE_SIZE, 1, fp_);
}

// 把内存中的一页写回磁盘（内存 → 外存）
void BPTree::write_page(int64_t pgno, const PageBuf& pg) {
    fseek(fp_, (long)(pgno * PAGE_SIZE), SEEK_SET);
    fwrite(pg.data, PAGE_SIZE, 1, fp_);
}

// 从根节点向下查找，返回 key 应该落在的叶节点页号
// 每读一页磁盘 io_cnt++，调用方用此统计 IO 次数
int64_t BPTree::find_leaf(int64_t key, int& io_cnt) {
    io_cnt = 0;
    int64_t pgno = meta_.root_pg();
    if (pgno == NULL_PG) return NULL_PG;

    PageBuf pg;
    while (true) {
        read_page(pgno, pg);   // 从磁盘读入当前节点（1次IO）
        io_cnt++;
        if (pg.type() == PAGE_LEAF) return pgno;   // 到达叶层，返回

        // 内部节点：线性扫描键数组，找到 key 所属的子区间
        // 规则：找到第一个 keys[i] > key 的位置，走 chld[i]
        int n = pg.count();
        const int64_t* keys = pg.i_keys();
        const int64_t* chld = pg.i_children();
        int i = 0;
        while (i < n && key >= keys[i]) i++;
        pgno = chld[i];   // 下钻到对应子节点
    }
}

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

int BPTree::range_query(int64_t key_min, int64_t key_max,
                        std::vector<Record>& results, int& io_count) {
    results.clear();
    io_count = 0;
    if (meta_.root_pg() == NULL_PG) return 0;

    // 第一阶段：沿树下钻，找到 key_min 所在的起始叶节点
    // 与 find_leaf 逻辑相同，但直接把叶节点读入 pg，避免后续重复 IO
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

    // 第二阶段：沿叶节点链表顺序扫描，收集 [key_min, key_max] 内的记录
    // 第一个叶节点已在上方读入 pg，直接使用，不需要再读一次磁盘
    while (true) {
        int n = pg.count();
        const Record* recs = pg.records();
        bool past = false;
        for (int i = 0; i < n; i++) {
            if (recs[i].key > key_max) { past = true; break; }  // 超出范围，停止
            if (recs[i].key >= key_min) results.push_back(recs[i]);
        }
        if (past) break;
        pgno = pg.next();              // 顺着链表指针走到下一个叶节点
        if (pgno == NULL_PG) break;    // 已到链表末尾
        read_page(pgno, pg);           // 读下一页（1次IO）
        io_count++;
    }
    return (int)results.size();
}

// 将一条记录插入叶节点，插入后保持页内按 key 有序
// 方法：从末尾往前，把比 rec.key 大的记录依次后移，腾出插入位置
void BPTree::insert_record_in_leaf(PageBuf& leaf, const Record& rec) {
    int n = leaf.count();
    Record* recs = leaf.records();
    int i = n;
    while (i > 0 && recs[i - 1].key > rec.key) {
        recs[i] = recs[i - 1];   // 后移
        i--;
    }
    recs[i] = rec;   // 插入正确位置
    leaf.count()++;
}

// 叶节点或内部节点分裂后，将分隔键 sep_key 插入父节点
// left_pgno 是原节点，right_pgno 是新分裂出的右节点
void BPTree::insert_into_parent(int64_t left_pgno, int64_t sep_key, int64_t right_pgno) {
    PageBuf left, right;
    read_page(left_pgno,  left);
    read_page(right_pgno, right);
    int64_t parent_pgno = left.parent();

    right.parent() = parent_pgno;
    write_page(right_pgno, right);

    if (parent_pgno == NULL_PG) {
        // 没有父节点，说明分裂的是根节点
        // 需要创建新根，树高度 +1
        int64_t new_root = alloc_page();
        PageBuf root_pg;
        memset(&root_pg, 0, sizeof(root_pg));
        root_pg.type()          = PAGE_INTERNAL;
        root_pg.count()         = 1;
        root_pg.parent()        = NULL_PG;
        root_pg.i_keys()[0]     = sep_key;       // 新根只有一个分隔键
        root_pg.i_children()[0] = left_pgno;     // 左子
        root_pg.i_children()[1] = right_pgno;    // 右子
        write_page(new_root, root_pg);

        // 更新左右子节点的父指针指向新根
        left.parent()  = new_root;
        right.parent() = new_root;
        write_page(left_pgno,  left);
        write_page(right_pgno, right);

        meta_.root_pg() = new_root;
        meta_dirty_ = true;
        return;
    }

    // 找到父节点，在父节点中 left_pgno 的右侧插入 sep_key 和 right_pgno
    PageBuf parent;
    read_page(parent_pgno, parent);
    int n = parent.count();
    int64_t* keys = parent.i_keys();
    int64_t* chld = parent.i_children();

    // 找到 left_pgno 在父节点子指针数组中的位置
    int pos = 0;
    while (pos <= n && chld[pos] != left_pgno) pos++;

    if (n < INTERNAL_MAX_KEYS) {
        // 父节点未满，直接插入分隔键和右子指针
        for (int i = n;     i > pos;     i--) keys[i] = keys[i - 1];
        for (int i = n + 1; i > pos + 1; i--) chld[i] = chld[i - 1];
        keys[pos]     = sep_key;
        chld[pos + 1] = right_pgno;
        parent.count()++;
        write_page(parent_pgno, parent);

        right.parent() = parent_pgno;
        write_page(right_pgno, right);
    } else {
        // 父节点已满（510个键），需要分裂父节点
        // 先把新键/指针插入临时数组，再对半分裂
        int64_t tmp_keys[INTERNAL_MAX_KEYS + 1];
        int64_t tmp_chld[INTERNAL_MAX_KEYS + 2];
        memcpy(tmp_keys, keys, n * sizeof(int64_t));
        memcpy(tmp_chld, chld, (n + 1) * sizeof(int64_t));

        for (int i = n;     i > pos;     i--) tmp_keys[i] = tmp_keys[i - 1];
        for (int i = n + 1; i > pos + 1; i--) tmp_chld[i] = tmp_chld[i - 1];
        tmp_keys[pos]     = sep_key;
        tmp_chld[pos + 1] = right_pgno;

        int total    = n + 1;                  // 分裂后总键数
        int left_cnt = (total - 1) / 2;        // 左内部节点保留的键数
        int64_t push_key = tmp_keys[left_cnt]; // 中间键上推到祖父（不保留在左右，与叶分裂不同）
        int right_cnt = total - left_cnt - 1;  // 右内部节点键数

        // 左内部节点（复用原父节点页）
        parent.count() = left_cnt;
        memcpy(parent.i_keys(),     tmp_keys,           left_cnt * sizeof(int64_t));
        memcpy(parent.i_children(), tmp_chld,       (left_cnt + 1) * sizeof(int64_t));
        write_page(parent_pgno, parent);

        // 创建右内部节点，存放 push_key 右侧的键和子指针
        int64_t new_int_pgno = alloc_page();
        PageBuf new_int;
        memset(&new_int, 0, sizeof(new_int));
        new_int.type()   = PAGE_INTERNAL;
        new_int.count()  = right_cnt;
        new_int.parent() = parent.parent();
        memcpy(new_int.i_keys(),     tmp_keys + left_cnt + 1, right_cnt * sizeof(int64_t));
        memcpy(new_int.i_children(), tmp_chld + left_cnt + 1, (right_cnt + 1) * sizeof(int64_t));
        write_page(new_int_pgno, new_int);

        // 右内部节点的所有子页父指针需要更新为新节点页号
        for (int i = 0; i <= right_cnt; i++) {
            PageBuf child;
            read_page(new_int.i_children()[i], child);
            child.parent() = new_int_pgno;
            write_page(new_int.i_children()[i], child);
        }

        // 递归向上传播 push_key，直到某层父节点未满或创建新根为止
        insert_into_parent(parent_pgno, push_key, new_int_pgno);
    }
}

bool BPTree::insert(int64_t key, double lat, double lon, int32_t alt) {
    Record rec;
    rec.key  = key;
    rec.lat  = lat;
    rec.lon  = lon;
    rec.alt  = alt;
    rec._pad = 0;

    // 空树：创建第一个叶节点，它同时也是根
    if (meta_.root_pg() == NULL_PG) {
        int64_t pgno = alloc_page();
        PageBuf leaf;
        memset(&leaf, 0, sizeof(leaf));
        leaf.type()   = PAGE_LEAF;
        leaf.count()  = 0;
        leaf.parent() = NULL_PG;
        leaf.next()   = NULL_PG;   // 链表中唯一的节点，无下一页
        insert_record_in_leaf(leaf, rec);
        write_page(pgno, leaf);
        meta_.root_pg() = pgno;
        meta_.rec_count()++;
        meta_dirty_ = true;
        flush_meta();
        return true;
    }

    // 沿树下钻，找到 key 应该插入的叶节点
    int io_dummy;
    int64_t leaf_pgno = find_leaf(key, io_dummy);
    PageBuf leaf;
    read_page(leaf_pgno, leaf);   // 把目标叶节点从磁盘读进内存

    if (leaf.count() < LEAF_MAX_KEYS) {
        // 叶节点未满（< 255条），直接插入，保持页内有序
        insert_record_in_leaf(leaf, rec);
        write_page(leaf_pgno, leaf);
    } else {
        // 叶节点已满（255条），触发叶分裂
        // 步骤1：把 256 条记录（原255 + 新1）按序放入临时数组
        Record tmp[LEAF_MAX_KEYS + 1];
        int n = leaf.count();
        const Record* recs = leaf.records();
        int ins = n;
        for (int i = 0; i < n; i++) { if (recs[i].key > key) { ins = i; break; } }
        for (int i = 0; i < ins;  i++) tmp[i]     = recs[i];
        tmp[ins] = rec;
        for (int i = ins; i < n; i++) tmp[i + 1]  = recs[i];

        // 步骤2：对半切分
        int total     = n + 1;
        int left_cnt  = (total + 1) / 2;   // 原叶保留前半部分
        int right_cnt = total - left_cnt;   // 新叶持有后半部分

        // 步骤3：创建新叶节点，维护叶链表
        int64_t new_pgno = alloc_page();
        PageBuf new_leaf;
        memset(&new_leaf, 0, sizeof(new_leaf));
        new_leaf.type()   = PAGE_LEAF;
        new_leaf.count()  = right_cnt;
        new_leaf.parent() = leaf.parent();
        new_leaf.next()   = leaf.next();   // 新叶的下一页 = 原叶原来的下一页

        leaf.count() = left_cnt;
        leaf.next()  = new_pgno;           // 原叶的下一页 = 新叶（插入链表）

        memcpy(leaf.records(),     tmp,             left_cnt  * sizeof(Record));
        memcpy(new_leaf.records(), tmp + left_cnt,  right_cnt * sizeof(Record));

        write_page(leaf_pgno, leaf);
        write_page(new_pgno, new_leaf);

        // 步骤4：取新叶第一个 key 作为分隔键，上推到父节点
        // 注意：分隔键同时留在新叶里（B+树特征，与内部节点分裂不同）
        int64_t separator = new_leaf.records()[0].key;
        insert_into_parent(leaf_pgno, separator, new_pgno);
    }

    meta_.rec_count()++;
    meta_dirty_ = true;
    flush_meta();
    return true;
}

// 从节点 pgno 中删除 key（叶节点）或 key+ptr_to_remove（内部节点）
// 删除后若节点键数低于最小值，触发"借"或"合并"来维持平衡
void BPTree::delete_entry(int64_t pgno, int64_t key, int64_t ptr_to_remove) {
    PageBuf pg;
    read_page(pgno, pg);

    if (pg.type() == PAGE_LEAF) {
        // 叶节点：找到 key 对应的记录，删除并前移填补空缺
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
    } else {
        // 内部节点：删除指定的键和子指针
        int n = pg.count();
        int64_t* keys = pg.i_keys();
        int64_t* chld = pg.i_children();
        int cidx = -1;
        for (int i = 0; i <= n; i++) { if (chld[i] == ptr_to_remove) { cidx = i; break; } }
        if (cidx <= 0) return;
        int kidx = cidx - 1;
        (void)key;
        for (int i = kidx; i < n - 1; i++) keys[i] = keys[i + 1];
        for (int i = cidx; i <= n - 1; i++) chld[i] = chld[i + 1];
        pg.count()--;
        write_page(pgno, pg);
    }

    if (pgno == meta_.root_pg()) {
        // 特殊处理根节点：根节点可以不满足最小键数约束
        PageBuf root;
        read_page(pgno, root);
        if (root.count() == 0) {
            if (root.type() == PAGE_INTERNAL) {
                // 内部根节点空了，把唯一的子节点提升为新根，树高度 -1
                int64_t new_root = root.i_children()[0];
                PageBuf nr;
                read_page(new_root, nr);
                nr.parent() = NULL_PG;
                write_page(new_root, nr);
                meta_.root_pg() = new_root;
                meta_dirty_ = true;
            } else {
                // 叶根节点空了，树变为空树
                meta_.root_pg() = NULL_PG;
                meta_dirty_ = true;
            }
        }
        return;
    }

    // 检查是否低于最小键数（叶128，内部255）
    read_page(pgno, pg);
    int min_keys = (pg.type() == PAGE_LEAF) ? LEAF_MIN_KEYS : INTERNAL_MIN_KEYS;
    if (pg.count() >= min_keys) return;   // 仍满足约束，不需要额外处理

    // 键数不足，需要从兄弟节点借或与兄弟合并
    int64_t parent_pgno = pg.parent();
    PageBuf parent;
    read_page(parent_pgno, parent);
    int pn = parent.count();
    int64_t* pkeys = parent.i_keys();
    int64_t* pchld = parent.i_children();

    // 找到当前节点在父节点子指针数组中的位置
    int idx = 0;
    while (idx <= pn && pchld[idx] != pgno) idx++;

    // 优先选左兄弟，没有左兄弟才选右兄弟
    int sib_idx   = (idx > 0) ? idx - 1 : idx + 1;
    int64_t k_idx = (idx > 0) ? idx - 1 : idx;   // 当前节点与兄弟之间的父分隔键下标
    int64_t sib_pgno = pchld[sib_idx];

    PageBuf sib;
    read_page(sib_pgno, sib);

    bool node_is_right = (idx > 0);   // true 表示当前节点在兄弟右侧（兄弟是左兄弟）

    if (sib.count() > min_keys) {
        // 兄弟节点有富余，从兄弟借一个键/记录
        if (pg.type() == PAGE_LEAF) {
            if (node_is_right) {
                // 从左兄弟借最后一条记录，插到当前节点最前面
                int sn = sib.count();
                Record borrowed = sib.records()[sn - 1];
                sib.count()--;
                Record* recs = pg.records();
                int n = pg.count();
                for (int i = n; i > 0; i--) recs[i] = recs[i - 1];
                recs[0] = borrowed;
                pg.count()++;
                pkeys[k_idx] = borrowed.key;   // 更新父节点分隔键
            } else {
                // 从右兄弟借第一条记录，追加到当前节点末尾
                Record* srecs = sib.records();
                Record borrowed = srecs[0];
                int sn = sib.count();
                for (int i = 0; i < sn - 1; i++) srecs[i] = srecs[i + 1];
                sib.count()--;
                Record* recs = pg.records();
                recs[pg.count()] = borrowed;
                pg.count()++;
                pkeys[k_idx] = srecs[0].key;   // 更新父节点分隔键为右兄弟新的第一个key
            }
            write_page(pgno, pg);
            write_page(sib_pgno, sib);
            write_page(parent_pgno, parent);
        } else {
            if (node_is_right) {
                // 内部节点从左兄弟借最后一个子：父分隔键下移，兄弟最后一个键上升
                int sn = sib.count();
                int64_t* sk = sib.i_keys();
                int64_t* sc = sib.i_children();
                int64_t* k  = pg.i_keys();
                int64_t* c  = pg.i_children();
                int n = pg.count();
                for (int i = n; i > 0; i--) k[i] = k[i - 1];
                for (int i = n + 1; i > 0; i--) c[i] = c[i - 1];
                k[0] = pkeys[k_idx];   // 父分隔键下移到当前节点最前
                c[0] = sc[sn];         // 兄弟最后一个子指针移过来
                pg.count()++;
                PageBuf bchild;
                read_page(sc[sn], bchild);
                bchild.parent() = pgno;
                write_page(sc[sn], bchild);
                pkeys[k_idx] = sk[sn - 1];   // 兄弟最后一个键上升为新的父分隔键
                sib.count()--;
            } else {
                // 内部节点从右兄弟借第一个子：父分隔键下移，兄弟第一个键上升
                int64_t* sk = sib.i_keys();
                int64_t* sc = sib.i_children();
                int64_t* k  = pg.i_keys();
                int64_t* c  = pg.i_children();
                int n = pg.count();
                int sn = sib.count();
                k[n] = pkeys[k_idx];   // 父分隔键下移到当前节点末尾
                c[n + 1] = sc[0];      // 兄弟第一个子指针移过来
                pg.count()++;
                PageBuf bchild;
                read_page(sc[0], bchild);
                bchild.parent() = pgno;
                write_page(sc[0], bchild);
                pkeys[k_idx] = sk[0];   // 兄弟第一个键上升为新的父分隔键
                for (int i = 0; i < sn - 1; i++) sk[i] = sk[i + 1];
                for (int i = 0; i < sn;     i++) sc[i] = sc[i + 1];
                sib.count()--;
            }
            write_page(pgno, pg);
            write_page(sib_pgno, sib);
            write_page(parent_pgno, parent);
        }
    } else {
        // 兄弟也不够借，只能合并：将两个节点合为一个，同时从父节点删除分隔键
        // 保证 left 在链表前面（key 较小），right 在后面
        int64_t left_pgno, right_pgno;
        PageBuf left_pg, right_pg;
        if (node_is_right) {
            left_pgno = sib_pgno; left_pg  = sib;
            right_pgno = pgno;   right_pg = pg;
        } else {
            left_pgno = pgno;    left_pg  = pg;
            right_pgno = sib_pgno; right_pg = sib;
        }

        if (left_pg.type() == PAGE_LEAF) {
            // 叶节点合并：把 right 的所有记录追加到 left 末尾，更新链表指针
            int ln = left_pg.count();
            int rn = right_pg.count();
            memcpy(left_pg.records() + ln, right_pg.records(), rn * sizeof(Record));
            left_pg.count() = ln + rn;
            left_pg.next()  = right_pg.next();   // 跳过被合并的 right 节点
            write_page(left_pgno, left_pg);
        } else {
            // 内部节点合并：把父分隔键下拉，再追加 right 的键和子指针
            int ln = left_pg.count();
            int rn = right_pg.count();
            int64_t* lk = left_pg.i_keys();
            int64_t* lc = left_pg.i_children();
            int64_t* rk = right_pg.i_keys();
            int64_t* rc = right_pg.i_children();
            lk[ln] = pkeys[k_idx];                              // 父分隔键下拉
            memcpy(lk + ln + 1, rk, rn * sizeof(int64_t));
            memcpy(lc + ln + 1, rc, (rn + 1) * sizeof(int64_t));
            left_pg.count() = ln + 1 + rn;
            write_page(left_pgno, left_pg);
            // 更新被合并子节点的父指针
            for (int i = 0; i <= rn; i++) {
                PageBuf child;
                read_page(rc[i], child);
                child.parent() = left_pgno;
                write_page(rc[i], child);
            }
        }

        // 从父节点删除 right 对应的分隔键和子指针，递归向上处理
        delete_entry(parent_pgno, pkeys[k_idx], right_pgno);
    }
}

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
