#include "swf.h"
#include "stream.h"
SWF::SWF(const char *filename):fp(nullptr)
{
    fopen_s(&fp,filename,"rb");
}

SWF::~SWF()
{
    if(fp){
        fclose(fp);
    }
}

void SWF::parse()
{
    if(fp)
    {
        char signature[3];
        fread(signature,1,3,fp);
        if(signature[0] != 'F' && signature[0] != 'C')
        {
            printf("不是有效的SWF文件\n");
            return;
        }
        if(signature[1] != 'W' || signature[2] != 'S')
        {
            printf("不是有效的SWF文件\n");
            return;
        }

        fread(&version,1,1,fp);
        fread(&filelength,sizeof(filelength),1,fp);
        uint8_t *data = new uint8_t[filelength-8];
        fread(data,1,filelength-8,fp);
        SWFStream stream(data,filelength-8);
        RECT rect;
        stream.readRect(rect);
        float frameRate;
        stream.readFIXED8(frameRate);
        uint16_t frameCount;
        stream.readUI16(frameCount);
        while(1)
        {
            uint32_t startpos = stream.GetCurPos();
            uint16_t tagCode;
            uint32_t tagLength;
            stream.readTag(tagCode,tagLength,[&](const uint8_t *data,  uint32_t taglength, uint16_t tagcode){
                  std::shared_ptr<Tag> tag = CreateTag(data,taglength,tagcode);
                  if(tag){
                    tag->process();
                    vec_tags.emplace_back(tag);
                  }
                 
            });
            uint32_t endpos = stream.GetCurPos();
            printf("TagCode: %d, TagLength: %d\n",tagCode,tagLength);
            if(tagCode == 0)
            {
                break;
            }
        }
        
    }
}