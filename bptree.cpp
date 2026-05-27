#include "bptree.h"
#include <cstring>
#include <cstdio>

BPTree::BPTree() : fp_(nullptr), meta_dirty_(false) {
    memset(&meta_, 0, sizeof(meta_));
}

BPTree::~BPTree() {
    close();
}

bool BPTree::open(const std::string& path, bool create) {
    if (fp_) {
        close();
    }

    if (create) {
        // 新建索引文件时，先写入页 0 作为元数据页。
        fp_ = fopen(path.c_str(), "w+b");
        if (!fp_) {
            return false;
        }

        memset(&meta_, 0, sizeof(meta_));
        meta_.type()      = PAGE_META;
        meta_.root_pg()   = NULL_PAGE;
        meta_.pg_count()  = 1;
        meta_.rec_count() = 0;
        write_page(0, meta_);
    } else {
        // 打开已有索引文件时，从页 0 读回根页号、页数和记录数。
        fp_ = fopen(path.c_str(), "r+b");
        if (!fp_) {
            return false;
        }
        read_page(0, meta_);
    }
    meta_dirty_ = false;
    return true;
}

void BPTree::close() {
    if (!fp_) {
        return;
    }
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

PageNo BPTree::alloc_page() {
    // 新页号直接使用当前已分配页数，然后在文件末尾写入空白页。
    PageNo page_no = meta_.pg_count();
    meta_.pg_count()++;
    meta_dirty_ = true;
    PageBuf blank;
    memset(&blank, 0, sizeof(blank));
    write_page(page_no, blank);
    return page_no;
}

void BPTree::read_page(PageNo page_no, PageBuf& page) {
    // 页号乘以 PAGE_SIZE 就是该页在索引文件中的字节偏移。
    fseek(fp_, (long)(page_no * PAGE_SIZE), SEEK_SET);
    fread(page.data, PAGE_SIZE, 1, fp_);
}

void BPTree::write_page(PageNo page_no, const PageBuf& page) {
    // 本实验按整页读写，便于统计和解释逻辑 I/O。
    fseek(fp_, (long)(page_no * PAGE_SIZE), SEEK_SET);
    fwrite(page.data, PAGE_SIZE, 1, fp_);
}

PageNo BPTree::find_leaf(Key key, int& io_count) {
    io_count = 0;
    PageNo page_no = meta_.root_pg();
    if (page_no == NULL_PAGE) {
        return NULL_PAGE;
    }

    PageBuf page;
    while (true) {
        // 从根页逐层向下，直到找到应插入的叶页。
        read_page(page_no, page);
        io_count++;
        if (page.type() == PAGE_LEAF) {
            return page_no;
        }

        int key_count = page.count();
        const Key* keys = page.i_keys();
        const PageNo* children = page.i_children();
        int i = 0;
        while (i < key_count && key > keys[i]) {
            i++;
        }
        page_no = children[i];
    }
}

bool BPTree::search(Key key, Record& out, int* io_count) {
    int io = 0;
    PageNo page_no = meta_.root_pg();
    if (page_no == NULL_PAGE) {
        if (io_count) {
            *io_count = io;
        }
        return false;
    }

    PageBuf leaf;
    while (true) {
        // 点查询先沿内部节点定位到可能包含目标时间戳的叶页。
        read_page(page_no, leaf);
        io++;
        if (leaf.type() == PAGE_LEAF) {
            break;
        }

        int key_count = leaf.count();
        const Key* keys = leaf.i_keys();
        const PageNo* children = leaf.i_children();
        int i = 0;
        while (i < key_count && key > keys[i]) {
            i++;
        }
        page_no = children[i];
    }

    PageNo leaf_page_no = page_no;
    while (leaf_page_no != NULL_PAGE) {
        // GEOLIFE 中可能有重复时间戳，所以命中前继续沿叶链检查。
        read_page(leaf_page_no, leaf);
        io++;

        int record_count = leaf.count();
        const Record* records = leaf.records();
        for (int i = 0; i < record_count; i++) {
            if (records[i].key == key) {
                out = records[i];
                if (io_count) {
                    *io_count = io;
                }
                return true;
            }
            if (records[i].key > key) {
                if (io_count) {
                    *io_count = io;
                }
                return false;
            }
        }
        leaf_page_no = leaf.next();
    }

    if (io_count) {
        *io_count = io;
    }
    return false;
}

int BPTree::range_query(Key key_min, Key key_max,
                        std::vector<Record>& results, int& io_count) {
    results.clear();
    io_count = 0;
    if (meta_.root_pg() == NULL_PAGE) {
        return 0;
    }

    PageNo page_no = meta_.root_pg();
    PageBuf page;
    while (true) {
        // 第一阶段：按照查询下界从根页下降到起始叶页。
        read_page(page_no, page);
        io_count++;
        if (page.type() == PAGE_LEAF) {
            break;
        }
        int key_count = page.count();
        const Key* keys = page.i_keys();
        const PageNo* children = page.i_children();
        int i = 0;
        // 这里用 > 而不是 >=，避免重复时间戳被分到左侧叶页时漏查。
        while (i < key_count && key_min > keys[i]) {
            i++;
        }
        page_no = children[i];
    }

    while (true) {
        // 第二阶段：沿叶节点链表顺序扫描，直到超过查询上界。
        int record_count = page.count();
        const Record* records = page.records();
        bool past = false;
        for (int i = 0; i < record_count; i++) {
            if (records[i].key > key_max) {
                past = true;
                break;
            }
            if (records[i].key >= key_min) {
                results.push_back(records[i]);
            }
        }
        if (past) {
            break;
        }

        page_no = page.next();
        if (page_no == NULL_PAGE) {
            break;
        }
        read_page(page_no, page);
        io_count++;
    }
    return (int)results.size();
}

void BPTree::insert_record_in_leaf(PageBuf& leaf, const Record& rec) {
    // 叶页内部保持按 key 升序，插入时先移动较大的记录。
    int record_count = leaf.count();
    Record* records = leaf.records();
    int i = record_count;
    while (i > 0 && records[i - 1].key > rec.key) {
        records[i] = records[i - 1];
        i--;
    }
    records[i] = rec;
    leaf.count()++;
}

void BPTree::insert_into_parent(PageNo left_page_no, Key separator_key, PageNo right_page_no) {
    PageBuf left, right;
    read_page(left_page_no,  left);
    read_page(right_page_no, right);
    PageNo parent_page_no = left.parent();

    right.parent() = parent_page_no;
    write_page(right_page_no, right);

    if (parent_page_no == NULL_PAGE) {
        // 左右页原来没有父节点，说明分裂发生在根节点，需要创建新根。
        PageNo new_root_page_no = alloc_page();
        PageBuf root_page;
        memset(&root_page, 0, sizeof(root_page));
        root_page.type()          = PAGE_INTERNAL;
        root_page.count()         = 1;
        root_page.parent()        = NULL_PAGE;
        root_page.i_keys()[0]     = separator_key;
        root_page.i_children()[0] = left_page_no;
        root_page.i_children()[1] = right_page_no;
        write_page(new_root_page_no, root_page);

        left.parent()  = new_root_page_no;
        right.parent() = new_root_page_no;
        write_page(left_page_no,  left);
        write_page(right_page_no, right);

        meta_.root_pg() = new_root_page_no;
        meta_dirty_ = true;
        return;
    }

    PageBuf parent;
    read_page(parent_page_no, parent);
    int key_count = parent.count();
    Key* keys = parent.i_keys();
    PageNo* children = parent.i_children();

    int child_index = 0;
    while (child_index <= key_count && children[child_index] != left_page_no) {
        child_index++;
    }

    if (key_count < INTERNAL_MAX_KEYS) {
        // 父节点还有空位，直接插入分隔键和新的右子页号。
        for (int i = key_count; i > child_index; i--) {
            keys[i] = keys[i - 1];
        }
        for (int i = key_count + 1; i > child_index + 1; i--) {
            children[i] = children[i - 1];
        }
        keys[child_index] = separator_key;
        children[child_index + 1] = right_page_no;
        parent.count()++;
        write_page(parent_page_no, parent);

        right.parent() = parent_page_no;
        write_page(right_page_no, right);
    } else {
        // 父节点已满时继续分裂内部页，并把中间键上推。
        Key tmp_keys[INTERNAL_MAX_KEYS + 1];
        PageNo tmp_children[INTERNAL_MAX_KEYS + 2];
        memcpy(tmp_keys, keys, key_count * sizeof(Key));
        memcpy(tmp_children, children, (key_count + 1) * sizeof(PageNo));

        for (int i = key_count; i > child_index; i--) {
            tmp_keys[i] = tmp_keys[i - 1];
        }
        for (int i = key_count + 1; i > child_index + 1; i--) {
            tmp_children[i] = tmp_children[i - 1];
        }
        tmp_keys[child_index] = separator_key;
        tmp_children[child_index + 1] = right_page_no;

        int total_keys = key_count + 1;
        int left_key_count = (total_keys - 1) / 2;
        Key promoted_key = tmp_keys[left_key_count];
        int right_key_count = total_keys - left_key_count - 1;

        parent.count() = left_key_count;
        memcpy(parent.i_keys(), tmp_keys, left_key_count * sizeof(Key));
        memcpy(parent.i_children(), tmp_children, (left_key_count + 1) * sizeof(PageNo));
        write_page(parent_page_no, parent);

        PageNo new_internal_page_no = alloc_page();
        PageBuf new_internal;
        memset(&new_internal, 0, sizeof(new_internal));
        new_internal.type()   = PAGE_INTERNAL;
        new_internal.count()  = right_key_count;
        new_internal.parent() = parent.parent();
        memcpy(new_internal.i_keys(), tmp_keys + left_key_count + 1, right_key_count * sizeof(Key));
        memcpy(new_internal.i_children(),
               tmp_children + left_key_count + 1,
               (right_key_count + 1) * sizeof(PageNo));
        write_page(new_internal_page_no, new_internal);

        for (int i = 0; i <= right_key_count; i++) {
            PageBuf child;
            read_page(new_internal.i_children()[i], child);
            child.parent() = new_internal_page_no;
            write_page(new_internal.i_children()[i], child);
        }

        insert_into_parent(parent_page_no, promoted_key, new_internal_page_no);
    }
}

bool BPTree::insert(Key key, double lat, double lon, int32_t alt) {
    Record rec;
    rec.key  = key;
    rec.lat  = lat;
    rec.lon  = lon;
    rec.alt  = alt;
    rec._pad = 0;

    if (meta_.root_pg() == NULL_PAGE) {
        // 空树第一次插入时，直接创建一个叶页作为根页。
        PageNo page_no = alloc_page();
        PageBuf leaf;
        memset(&leaf, 0, sizeof(leaf));
        leaf.type()   = PAGE_LEAF;
        leaf.count()  = 0;
        leaf.parent() = NULL_PAGE;
        leaf.next()   = NULL_PAGE;
        insert_record_in_leaf(leaf, rec);
        write_page(page_no, leaf);
        meta_.root_pg() = page_no;
        meta_.rec_count()++;
        meta_dirty_ = true;
        flush_meta();
        return true;
    }

    int io_dummy;
    PageNo leaf_page_no = find_leaf(key, io_dummy);
    PageBuf leaf;
    read_page(leaf_page_no, leaf);

    if (leaf.count() < LEAF_MAX_KEYS) {
        // 目标叶页未满，直接在页内有序插入。
        insert_record_in_leaf(leaf, rec);
        write_page(leaf_page_no, leaf);
    } else {
        // 目标叶页已满，先合并为临时数组，再平均拆成左右两个叶页。
        Record tmp[LEAF_MAX_KEYS + 1];
        int record_count = leaf.count();
        const Record* records = leaf.records();
        int insert_pos = record_count;
        for (int i = 0; i < record_count; i++) {
            if (records[i].key > key) {
                insert_pos = i;
                break;
            }
        }
        for (int i = 0; i < insert_pos; i++) {
            tmp[i] = records[i];
        }
        tmp[insert_pos] = rec;
        for (int i = insert_pos; i < record_count; i++) {
            tmp[i + 1] = records[i];
        }

        int total_records = record_count + 1;
        int left_record_count = (total_records + 1) / 2;
        int right_record_count = total_records - left_record_count;

        PageNo new_page_no = alloc_page();
        PageBuf new_leaf;
        memset(&new_leaf, 0, sizeof(new_leaf));
        new_leaf.type()   = PAGE_LEAF;
        new_leaf.count()  = right_record_count;
        new_leaf.parent() = leaf.parent();
        new_leaf.next()   = leaf.next();

        leaf.count() = left_record_count;
        leaf.next()  = new_page_no;

        memcpy(leaf.records(), tmp, left_record_count * sizeof(Record));
        memcpy(new_leaf.records(), tmp + left_record_count, right_record_count * sizeof(Record));

        write_page(leaf_page_no, leaf);
        write_page(new_page_no, new_leaf);

        Key separator_key = leaf.records()[left_record_count - 1].key;
        // 使用左叶页最后一个 key 作为分隔键，相等时会从左侧开始查，避免漏掉重复时间戳。
        insert_into_parent(leaf_page_no, separator_key, new_page_no);
    }

    meta_.rec_count()++;
    meta_dirty_ = true;
    flush_meta();
    return true;
}
