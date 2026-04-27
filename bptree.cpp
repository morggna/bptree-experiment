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

int64_t BPTree::find_leaf(int64_t key, int& io_cnt) {
    io_cnt = 0;
    int64_t pgno = meta_.root_pg();
    if (pgno == NULL_PG) return NULL_PG;

    PageBuf pg;
    while (true) {
        read_page(pgno, pg);
        io_cnt++;
        if (pg.type() == PAGE_LEAF) return pgno;

        // 线性扫描找子树区间
        int n = pg.count();
        const int64_t* keys = pg.i_keys();
        const int64_t* chld = pg.i_children();
        int i = 0;
        while (i < n && key >= keys[i]) i++;
        pgno = chld[i];
    }
}

bool BPTree::search(int64_t key, Record& out, int* io_count) {
    int io = 0;
    int64_t leaf_pgno = find_leaf(key, io);
    if (leaf_pgno == NULL_PG) { if (io_count) *io_count = io; return false; }

    PageBuf leaf;
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
    }
    if (io_count) *io_count = io;
    return false;
}

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
        while (i < n && key_min >= keys[i]) i++;
        pgno = chld[i];
    }

    // 顺序扫描叶链
    while (pgno != NULL_PG) {
        read_page(pgno, pg);
        io_count++;
        int n = pg.count();
        const Record* recs = pg.records();
        bool past = false;
        for (int i = 0; i < n; i++) {
            if (recs[i].key > key_max) { past = true; break; }
            if (recs[i].key >= key_min) results.push_back(recs[i]);
        }
        if (past) break;
        pgno = pg.next();
    }
    return (int)results.size();
}

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

void BPTree::insert_into_parent(int64_t left_pgno, int64_t sep_key, int64_t right_pgno) {
    PageBuf left, right;
    read_page(left_pgno,  left);
    read_page(right_pgno, right);
    int64_t parent_pgno = left.parent();

    right.parent() = parent_pgno;
    write_page(right_pgno, right);

    if (parent_pgno == NULL_PG) {
        // 创建新根
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

    PageBuf parent;
    read_page(parent_pgno, parent);
    int n = parent.count();
    int64_t* keys = parent.i_keys();
    int64_t* chld = parent.i_children();

    int pos = 0;
    while (pos <= n && chld[pos] != left_pgno) pos++;

    if (n < INTERNAL_MAX_KEYS) {
        for (int i = n;     i > pos;     i--) keys[i] = keys[i - 1];
        for (int i = n + 1; i > pos + 1; i--) chld[i] = chld[i - 1];
        keys[pos]     = sep_key;
        chld[pos + 1] = right_pgno;
        parent.count()++;
        write_page(parent_pgno, parent);

        right.parent() = parent_pgno;
        write_page(right_pgno, right);
    } else {
        // 父节点满，分裂：临时数组存 n+1 个键、n+2 个子指针
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
        int64_t push_key = tmp_keys[left_cnt]; // 上推到祖父的键
        int right_cnt = total - left_cnt - 1;  // 右内部节点键数

        parent.count() = left_cnt;
        memcpy(parent.i_keys(),     tmp_keys,           left_cnt * sizeof(int64_t));
        memcpy(parent.i_children(), tmp_chld,       (left_cnt + 1) * sizeof(int64_t));
        write_page(parent_pgno, parent);

        // 创建右内部节点
        int64_t new_int_pgno = alloc_page();
        PageBuf new_int;
        memset(&new_int, 0, sizeof(new_int));
        new_int.type()   = PAGE_INTERNAL;
        new_int.count()  = right_cnt;
        new_int.parent() = parent.parent();
        memcpy(new_int.i_keys(),     tmp_keys + left_cnt + 1, right_cnt * sizeof(int64_t));
        memcpy(new_int.i_children(), tmp_chld + left_cnt + 1, (right_cnt + 1) * sizeof(int64_t));
        write_page(new_int_pgno, new_int);

        // 右内部节点的子页父指针全部更新
        for (int i = 0; i <= right_cnt; i++) {
            PageBuf child;
            read_page(new_int.i_children()[i], child);
            child.parent() = new_int_pgno;
            write_page(new_int.i_children()[i], child);
        }

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

    // 空树：创建第一个叶节点作为根
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

    int io_dummy;
    int64_t leaf_pgno = find_leaf(key, io_dummy);
    PageBuf leaf;
    read_page(leaf_pgno, leaf);

    if (leaf.count() < LEAF_MAX_KEYS) {
        insert_record_in_leaf(leaf, rec);
        write_page(leaf_pgno, leaf);
    } else {
        // 叶满，分裂
        Record tmp[LEAF_MAX_KEYS + 1];
        int n = leaf.count();
        const Record* recs = leaf.records();
        int ins = n;
        for (int i = 0; i < n; i++) { if (recs[i].key > key) { ins = i; break; } }
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
    }

    meta_.rec_count()++;
    meta_dirty_ = true;
    flush_meta();
    return true;
}

void BPTree::delete_entry(int64_t pgno, int64_t key, int64_t ptr_to_remove) {
    PageBuf pg;
    read_page(pgno, pg);

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
    } else {
        int n = pg.count();
        int64_t* keys = pg.i_keys();
        int64_t* chld = pg.i_children();
        int cidx = -1;
        for (int i = 0; i <= n; i++) { if (chld[i] == ptr_to_remove) { cidx = i; break; } }
        int kidx = -1;
        for (int i = 0; i < n; i++) { if (keys[i] == key) { kidx = i; break; } }
        if (kidx >= 0) {
            for (int i = kidx; i < n - 1; i++) keys[i] = keys[i + 1];
        }
        if (cidx >= 0) {
            for (int i = cidx; i <= n - 1; i++) chld[i] = chld[i + 1];
        }
        pg.count()--;
        write_page(pgno, pg);
    }

    if (pgno == meta_.root_pg()) {
        PageBuf root;
        read_page(pgno, root);
        if (root.count() == 0) {
            if (root.type() == PAGE_INTERNAL) {
                // 唯一子节点提升为新根
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

    read_page(pgno, pg);
    int min_keys = (pg.type() == PAGE_LEAF) ? LEAF_MIN_KEYS : INTERNAL_MIN_KEYS;
    if (pg.count() >= min_keys) return;

    int64_t parent_pgno = pg.parent();
    PageBuf parent;
    read_page(parent_pgno, parent);
    int pn = parent.count();
    int64_t* pkeys = parent.i_keys();
    int64_t* pchld = parent.i_children();

    int idx = 0;
    while (idx <= pn && pchld[idx] != pgno) idx++;

    // 优先使用左兄弟，否则右兄弟
    int sib_idx   = (idx > 0) ? idx - 1 : idx + 1;
    int64_t k_idx = (idx > 0) ? idx - 1 : idx; // 父节点中分隔键下标
    int64_t sib_pgno = pchld[sib_idx];

    PageBuf sib;
    read_page(sib_pgno, sib);

    bool node_is_right = (idx > 0); // pgno 在 sib 右边（sib 是左兄弟）

    if (sib.count() > min_keys) {
        if (pg.type() == PAGE_LEAF) {
            if (node_is_right) {
                // 从左兄弟借最后一条
                int sn = sib.count();
                Record borrowed = sib.records()[sn - 1];
                sib.count()--;
                Record* recs = pg.records();
                int n = pg.count();
                for (int i = n; i > 0; i--) recs[i] = recs[i - 1];
                recs[0] = borrowed;
                pg.count()++;
                pkeys[k_idx] = borrowed.key;
            } else {
                // 从右兄弟借第一条
                Record* srecs = sib.records();
                Record borrowed = srecs[0];
                int sn = sib.count();
                for (int i = 0; i < sn - 1; i++) srecs[i] = srecs[i + 1];
                sib.count()--;
                Record* recs = pg.records();
                recs[pg.count()] = borrowed;
                pg.count()++;
                pkeys[k_idx] = srecs[0].key;
            }
            write_page(pgno, pg);
            write_page(sib_pgno, sib);
            write_page(parent_pgno, parent);
        } else {
            if (node_is_right) {
                // 从左兄弟借最后一个子
                int sn = sib.count();
                int64_t* sk = sib.i_keys();
                int64_t* sc = sib.i_children();
                int64_t* k  = pg.i_keys();
                int64_t* c  = pg.i_children();
                int n = pg.count();
                for (int i = n; i > 0; i--) k[i] = k[i - 1];
                for (int i = n + 1; i > 0; i--) c[i] = c[i - 1];
                k[0] = pkeys[k_idx];
                c[0] = sc[sn];
                pg.count()++;
                PageBuf bchild;
                read_page(sc[sn], bchild);
                bchild.parent() = pgno;
                write_page(sc[sn], bchild);
                pkeys[k_idx] = sk[sn - 1];
                sib.count()--;
            } else {
                // 从右兄弟借第一个子
                int64_t* sk = sib.i_keys();
                int64_t* sc = sib.i_children();
                int64_t* k  = pg.i_keys();
                int64_t* c  = pg.i_children();
                int n = pg.count();
                int sn = sib.count();
                k[n] = pkeys[k_idx];
                c[n + 1] = sc[0];
                pg.count()++;
                PageBuf bchild;
                read_page(sc[0], bchild);
                bchild.parent() = pgno;
                write_page(sc[0], bchild);
                pkeys[k_idx] = sk[0];
                for (int i = 0; i < sn - 1; i++) sk[i] = sk[i + 1];
                for (int i = 0; i < sn;     i++) sc[i] = sc[i + 1];
                sib.count()--;
            }
            write_page(pgno, pg);
            write_page(sib_pgno, sib);
            write_page(parent_pgno, parent);
        }
    } else {
        // 合并：保证 left 在前，right 在后
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
            int ln = left_pg.count();
            int rn = right_pg.count();
            memcpy(left_pg.records() + ln, right_pg.records(), rn * sizeof(Record));
            left_pg.count() = ln + rn;
            left_pg.next()  = right_pg.next();
            write_page(left_pgno, left_pg);
        } else {
            // 内部节点合并：下拉父键，追加 right 的键和子
            int ln = left_pg.count();
            int rn = right_pg.count();
            int64_t* lk = left_pg.i_keys();
            int64_t* lc = left_pg.i_children();
            int64_t* rk = right_pg.i_keys();
            int64_t* rc = right_pg.i_children();
            lk[ln] = pkeys[k_idx];
            memcpy(lk + ln + 1, rk, rn * sizeof(int64_t));
            memcpy(lc + ln + 1, rc, (rn + 1) * sizeof(int64_t));
            left_pg.count() = ln + 1 + rn;
            write_page(left_pgno, left_pg);
            for (int i = 0; i <= rn; i++) {
                PageBuf child;
                read_page(rc[i], child);
                child.parent() = left_pgno;
                write_page(rc[i], child);
            }
        }

        delete_entry(parent_pgno, pkeys[k_idx], right_pgno);
    }
}

bool BPTree::remove(int64_t key) {
    int io_dummy;
    int64_t leaf_pgno = find_leaf(key, io_dummy);
    if (leaf_pgno == NULL_PG) return false;

    PageBuf leaf;
    read_page(leaf_pgno, leaf);
    int n = leaf.count();
    const Record* recs = leaf.records();
    bool found = false;
    for (int i = 0; i < n; i++) { if (recs[i].key == key) { found = true; break; } }
    if (!found) return false;

    delete_entry(leaf_pgno, key, -1LL);
    meta_.rec_count()--;
    meta_dirty_ = true;
    flush_meta();
    return true;
}
