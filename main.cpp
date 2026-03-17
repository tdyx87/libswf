#include <iostream>
#include <vector>
#include <zlib.h>
#include <stdexcept>
#include <fstream>
std::vector<unsigned char> decompressData(const std::vector<unsigned char>& compressedData) {
    if (compressedData.empty()) {
        return std::vector<unsigned char>();
    }

    // 初始化 zlib 流
    z_stream stream;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;

    // 初始化解压（使用 inflate）
    int ret = inflateInit(&stream);
    if (ret != Z_OK) {
        throw std::runtime_error("inflateInit 失败");
    }

    std::vector<unsigned char> decompressedData;
    const size_t CHUNK_SIZE = 16384;  // 16KB 块大小
    std::vector<unsigned char> outBuffer(CHUNK_SIZE);

    // 设置输入数据
    stream.avail_in = compressedData.size();
    stream.next_in = const_cast<Bytef*>(compressedData.data());

    // 解压循环
    do {
        stream.avail_out = CHUNK_SIZE;
        stream.next_out = outBuffer.data();

        ret = inflate(&stream, Z_NO_FLUSH);
        
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&stream);
            throw std::runtime_error("解压过程中发生错误");
        }

        // 将解压出的数据添加到结果中
        size_t have = CHUNK_SIZE - stream.avail_out;
        if (have > 0) {
            decompressedData.insert(decompressedData.end(), 
                                   outBuffer.begin(), 
                                   outBuffer.begin() + have);
        }
    } while (ret != Z_STREAM_END);

    // 清理
    inflateEnd(&stream);

    return decompressedData;
}

// 简化版本 - 如果知道解压后的大小
std::vector<unsigned char> decompressDataKnownSize(
    const std::vector<unsigned char>& compressedData, 
    size_t decompressedSize) {
    
    std::vector<unsigned char> decompressedData(decompressedSize);
    
    z_stream stream = {};
    inflateInit(&stream);
    
    stream.avail_in = compressedData.size();
    stream.next_in = const_cast<Bytef*>(compressedData.data());
    stream.avail_out = decompressedSize;
    stream.next_out = decompressedData.data();
    
    int ret = inflate(&stream, Z_FINISH);
    
    if (ret != Z_STREAM_END) {
        inflateEnd(&stream);
        throw std::runtime_error("解压失败");
    }
    
    inflateEnd(&stream);
    return decompressedData;
}


int main(int argc, char* argv[])
{
	if (argc < 2) {
		std::cerr << "请提供输入文件路径" << std::endl;
		return 1;
	}	
	std::ifstream inputFile(argv[1], std::ios::binary);
	if (!inputFile) {
		std::cerr << "无法打开文件" << std::endl;
		return 1;
	}
	try {
		inputFile.seekg(0, std::ios::end);
		size_t fileSize = inputFile.tellg();
		inputFile.seekg(0, std::ios::beg);
		std::vector<unsigned char> header(8);
		inputFile.read(reinterpret_cast<char*>(header.data()), 8);
		inputFile.seekg(8, std::ios::beg);
		std::vector<unsigned char> compressedData(fileSize - 8);
		inputFile.read(reinterpret_cast<char*>(compressedData.data()), fileSize - 8);
		std::vector<unsigned char> decompressedData = decompressData(compressedData);
		std::ofstream outputFile("decompressed_data.bin", std::ios::binary);
		if (!outputFile) {
			std::cerr << "无法创建输出文件" << std::endl;
			return 1;
		}
		header[0] = 'F'; // 修改头部标识为未压缩
		outputFile.write(reinterpret_cast<const char*>(header.data()), header.size());
		outputFile.write(reinterpret_cast<const char*>(decompressedData.data()), decompressedData.size());
		std::cout << "解压成功，输出文件已创建" << std::endl;
	} catch (const std::exception& e) {
		std::cerr << "解压失败: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}