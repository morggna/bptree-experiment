#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>

using PageNo = int64_t;
using Key = int64_t;
using Timestamp = int64_t;

// 每个索引页固定为 8 KB，页内容量按这个大小计算。
static const int    PAGE_SIZE         = 8192;
static const int    INTERNAL_MAX_KEYS = 510;
static const int    LEAF_MAX_KEYS     = 255;
static const PageNo NULL_PAGE         = -1LL;

enum : int32_t { PAGE_META = 0, PAGE_INTERNAL = 1, PAGE_LEAF = 2 };

// 一条轨迹点记录。key 使用 Unix 时间戳，lat/lon/alt 保存位置和海拔。
struct Record {
    Key     key;
    double  lat;
    double  lon;
    int32_t alt;
    int32_t _pad;
};
static_assert(sizeof(Record) == 32, "Record must be 32 bytes");

// PageBuf 表示文件中的一个 8 KB 页面，所有字段都按固定偏移解释。
struct PageBuf {

    alignas(8) char data[PAGE_SIZE];

    // 三类页面共用的页头字段。
    int32_t  type()   const { return *(const int32_t*)(data +  0); }
    int32_t& type()         { return *(      int32_t*)(data +  0); }
    int32_t  count()  const { return *(const int32_t*)(data +  4); }
    int32_t& count()        { return *(      int32_t*)(data +  4); }
    PageNo  parent() const { return *(const PageNo*)(data +  8); }
    PageNo& parent()       { return *(      PageNo*)(data +  8); }

    // 内部页：保存子页号数组和分隔键数组。
    PageNo*       i_children()       { return (      PageNo*)(data + 16);   }
    const PageNo* i_children() const { return (const PageNo*)(data + 16);   }
    Key*          i_keys()           { return (      Key*)(data + 4104); }
    const Key*    i_keys()     const { return (const Key*)(data + 4104); }

    // 叶页：保存后继叶页页号和轨迹记录数组。
    PageNo  next()   const { return *(const PageNo*)(data + 16); }
    PageNo& next()         { return *(      PageNo*)(data + 16); }
    Record*       records()       { return (      Record*)(data + 24); }
    const Record* records() const { return (const Record*)(data + 24); }

    // 元数据页：固定放在页 0，记录根页、已分配页数和记录总数。
    PageNo   root_pg()   const { return *(const PageNo*)(data +  8); }
    PageNo&  root_pg()         { return *(      PageNo*)(data +  8); }
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

    bool insert(Key key, double lat, double lon, int32_t alt);
    bool search(Key key, Record& out, int* io_count = nullptr);

    // 范围查询返回 [key_min, key_max] 内的记录，并统计读取了多少个逻辑页。
    int  range_query(Key key_min, Key key_max,
                     std::vector<Record>& results, int& io_count);

    int64_t total_records() const { return meta_.rec_count(); }
    int64_t total_pages()   const { return meta_.pg_count();  }

private:
    FILE*   fp_;
    PageBuf meta_;
    bool    meta_dirty_;

    void    flush_meta();
    PageNo  alloc_page();
    void    read_page (PageNo page_no, PageBuf& page);
    void    write_page(PageNo page_no, const PageBuf& page);

    PageNo  find_leaf(Key key, int& io_count);
    void    insert_record_in_leaf(PageBuf& leaf, const Record& rec);
    void    insert_into_parent(PageNo left_page_no, Key separator_key, PageNo right_page_no);
};
