# The top level build file. These three lines are the standard start of an ESP-IDF project.
# The code itself is listed in src/CMakeLists.txt.
cmake_minimum_required(VERSION 3.22)

# ESP-IDF looks for the app's code in a folder called main/. Ours is in src/, so it has to be added here.
set(EXTRA_COMPONENT_DIRS src)

# Pulls in ESP-IDF's build system. IDF_PATH gets set by running export.sh.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt-rgb-dino)
