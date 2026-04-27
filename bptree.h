#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>

static const int     PAGE_SIZE         = 8192;
static const int     INTERNAL_MAX_KEYS = 510;
static const int     LEAF_MAX_KEYS     = 255;
static const int     INTERNAL_MIN_KEYS = 255;   // ceil(510/2)
static const int     LEAF_MIN_KEYS     = 128;   // ceil(255/2)
static const int64_t NULL_PG           = -1LL;

enum : int32_t { PAGE_META = 0, PAGE_INTERNAL = 1, PAGE_LEAF = 2 };

struct Record {
    int64_t key;   // Unix 时间戳（秒）
    double  lat;
    double  lon;
    int32_t alt;
    int32_t _pad;
};
static_assert(sizeof(Record) == 32, "Record must be 32 bytes");

// 页内存布局：
//   公共头部(16B): [0]type [4]key_count [8]parent_pgno
//   内部节点: [16]children[511] [4104]keys[510]
//   叶节点:   [16]next_pgno     [24]records[255]
//   元数据页: [8]root_pgno      [16]page_count  [24]record_count
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

    // 元数据页复用公共头部偏移
    int64_t  root_pg()   const { return *(const int64_t*)(data +  8); }
    int64_t& root_pg()         { return *(      int64_t*)(data +  8); }
    int64_t  pg_count()  const { return *(const int64_t*)(data + 16); }
    int64_t& pg_count()        { return *(      int64_t*)(data + 16); }
    int64_t  rec_count() const { return *(const int64_t*)(data + 24); }
    int64_t& rec_count()       { return *(      int64_t*)(data + 24); }
};
static_assert(sizeof(PageBuf) == PAGE_SIZE, "PageBuf size mismatch");

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
