#pragma once
#include <string>
#include <stdio.h>
#include <functional>
#include <vector>
class FileStream {
public:
    explicit FileStream(const std::string& file);
    int read(uint32_t size,
             std::function<void(std::vector<uint8_t>, uint32_t)> callback);
    ~FileStream();

private:
    FILE* fp;
};