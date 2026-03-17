@echo off
call conan\conan_install_all.cmd
cmake --preset conan-default
cmake --build --preset conan-debug