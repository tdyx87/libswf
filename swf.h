#pragma once
#include <stdio.h>
#include <stdint.h>
#include "tag.h"
class SWF
{
public:
    SWF(const char *filename);
    ~SWF();
    void parse();

public:
    FILE *fp;
    uint8_t version;
    unsigned int filelength;
    std::vector<std::shared_ptr<Tag>> vec_tags;
};