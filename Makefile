CXX      = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra
TARGET   = bptree_demo
SRCS     = bptree.cpp main.cpp
OBJS     = $(SRCS:.cpp=.o)
PYTHON   ?= python3

# 数据集目录（可在命令行覆盖：make run DATA="路径"）
DATA ?=

# MSYS2 中 TEMP 需要指向可写目录
export TEMP := /tmp
export TMP  := /tmp

.PHONY: all clean run viz

all: $(TARGET).exe

$(TARGET).exe: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp bptree.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

run: all
	./$(TARGET).exe $(DATA)

viz:
	$(PYTHON) visualize.py

clean:
	rm -f $(OBJS) $(TARGET).exe geolife.idx query_results.csv query_summary.csv bptree_performance.png
