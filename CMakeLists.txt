cmake_minimum_required(VERSION 3.20)
project(bptree_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 优化编译
add_compile_options(-O2 -Wall -Wextra)

add_executable(bptree_demo
    bptree.cpp
    main.cpp
)

# 数据集路径（作为程序参数在 CLion Run Configuration 中设置）
# 默认数据集路径已硬编码在 main.cpp 中，无需额外配置
