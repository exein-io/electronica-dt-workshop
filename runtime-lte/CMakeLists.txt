cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(runtime_lte)

# Only used when runtime-zephyr is built from source (blob rebuild, issue #4).
zephyr_include_directories(compat)

target_sources(app PRIVATE src/main.c src/app_workload.c)
