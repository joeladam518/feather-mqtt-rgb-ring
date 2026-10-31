# The top level build file. These three lines are the standard start of an ESP-IDF project.
# The code itself is listed in main/CMakeLists.txt.
cmake_minimum_required(VERSION 3.22)

# Pulls in ESP-IDF's build system. IDF_PATH gets set by running export.sh.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt-rgb-dino)
