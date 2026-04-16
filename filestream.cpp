#include "filestream.h"

FileStream::FileStream(const std::string& file) : fp(nullptr)
{
    fopen_s(&fp, file.c_str(), "rb");
}

int FileStream::read(
    uint32_t size, std::function<void(std::vector<uint8_t>, uint32_t)> callback)
{
    if (fp == nullptr) return -1;
    std::vector<uint8_t> buffer;
    buffer.resize(size);
    uint32_t realsize = fread(buffer.data(), 1, size, fp);
    if (realsize == 0) {
        return 0;
    }
    buffer.resize(realsize);
    if (callback) callback(buffer, realsize);
    return realsize;
}

FileStream::~FileStream()
{
    if (fp) {
        fclose(fp);
        fp = nullptr;
    }
}
