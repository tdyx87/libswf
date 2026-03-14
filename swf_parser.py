#!/usr/bin/env python3
"""
SWF (Shockwave Flash) 文件解析器
用于解析 SWF 文件的头信息和标签内容
"""

import struct
import sys
import os
from typing import Dict, List, Any, Optional
from dataclasses import dataclass
from enum import Enum


class SWFTagType(Enum):
    """SWF 标签类型"""
    END = 0
    SHOW_FRAME = 1
    DEFINE_SHAPE = 2
    PLACE_OBJECT = 4
    REMOVE_OBJECT = 5
    DEFINE_BITS = 6
    DEFINE_BUTTON = 7
    JPEG_TABLES = 8
    SET_BACKGROUND_COLOR = 9
    DEFINE_FONT = 10
    DEFINE_TEXT = 11
    DO_ACTION = 12
    DEFINE_FONT_INFO = 13
    DEFINE_SOUND = 14
    START_SOUND = 15
    DEFINE_BUTTON_SOUND = 17
    SOUND_STREAM_HEAD = 18
    SOUND_STREAM_BLOCK = 19
    DEFINE_BITS_LOSSLESS = 20
    DEFINE_BITS_JPEG2 = 21
    DEFINE_SHAPE2 = 22
    DEFINE_BUTTON_CXFORM = 23
    PROTECT = 24
    PLACE_OBJECT2 = 26
    REMOVE_OBJECT2 = 28
    DEFINE_SHAPE3 = 32
    DEFINE_TEXT2 = 33
    DEFINE_BUTTON2 = 34
    DEFINE_BITS_JPEG3 = 35
    DEFINE_BITS_LOSSLESS2 = 36
    DEFINE_EDIT_TEXT = 37
    FRAME_LABEL = 38
    SOUND_STREAM_HEAD2 = 45
    DEFINE_MORPH_SHAPE = 46
    DEFINE_FONT2 = 48
    EXPORT_ASSETS = 56
    IMPORT_ASSETS = 57
    ENABLE_DEBUGGER = 58
    DO_INIT_ACTION = 59
    DEFINE_VIDEO_STREAM = 60
    VIDEO_FRAME = 61
    DEFINE_FONT_INFO2 = 62
    ENABLE_DEBUGGER2 = 64
    SCRIPT_LIMITS = 65
    SET_TAB_INDEX = 66
    FILE_ATTRIBUTES = 69
    PLACE_OBJECT3 = 70
    IMPORT_ASSETS2 = 71
    DEFINE_FONT_ALIGN_ZONES = 73
    CSM_TEXT_SETTINGS = 74
    DEFINE_FONT3 = 75
    SYMBOL_CLASS = 76
    METADATA = 77
    DEFINE_SCALING_GRID = 78
    DO_ABC = 82
    DEFINE_SCENE_AND_FRAME_LABEL_DATA = 86
    DEFINE_BINARY_DATA = 87
    DEFINE_FONT_NAME = 88
    START_MOTION2 = 91
    ACEND = 0xFFFF


@dataclass
class Rect:
    """矩形区域"""
    x_min: int
    x_max: int
    y_min: int
    y_max: int
    
    def __str__(self):
        return f"({self.x_min}, {self.y_min}, {self.x_max}, {self.y_max})"


@dataclass
class SWFHeader:
    """SWF 文件头"""
    signature: str
    version: int
    file_length: int
    frame_size: Rect
    frame_rate: float
    frame_count: int


@dataclass
class SWFTag:
    """SWF 标签"""
    tag_type: int
    tag_length: int
    tag_name: str
    data: bytes
    content: Optional[Dict[str, Any]] = None


class SWFParser:
    """SWF 文件解析器"""
    
    def __init__(self, filepath: str):
        self.filepath = filepath
        self.header: Optional[SWFHeader] = None
        self.tags: List[SWFTag] = []
        self._file = None
        
    def open(self) -> bool:
        """打开并解析 SWF 文件"""
        try:
            self._file = open(self.filepath, 'rb')
            return self._parse()
        except Exception as e:
            print(f"Error opening file: {e}")
            return False
    
    def close(self):
        """关闭文件"""
        if self._file:
            self._file.close()
            self._file = None
    
    def _read_bytes(self, n: int) -> bytes:
        """读取 n 字节"""
        return self._file.read(n)
    
    def _read_ui8(self) -> int:
        """读取无符号 8 位整数"""
        return struct.unpack('B', self._file.read(1))[0]
    
    def _read_ui16(self) -> int:
        """读取无符号 16 位整数"""
        return struct.unpack('<H', self._file.read(2))[0]
    
    def _read_ui32(self) -> int:
        """读取无符号 32 位整数"""
        return struct.unpack('<I', self._file.read(4))[0]
    
    def _read_fixed8(self) -> float:
        """读取定点数 8.8"""
        value = self._read_ui16()
        return value / 256.0
    
    def _read_fixed16(self) -> float:
        """读取定点数 16.16"""
        value = self._read_ui32()
        return value / 65536.0
    
    def _read_rect(self) -> Rect:
        """读取矩形结构"""
        # 读取第一个字节获取位长度
        first_byte = self._read_ui8()
        n_bits = (first_byte >> 3) & 0x0F
        
        # 计算剩余位数
        remaining_bits = (n_bits * 4 + 5) - 8
        buffer = first_byte & 0x07
        
        # 读取足够的字节来获取所有位
        bytes_to_read = (remaining_bits + 7) // 8
        if bytes_to_read > 0:
            buffer = (buffer << (bytes_to_read * 8)) | int.from_bytes(
                self._file.read(bytes_to_read), 'big')
        
        # 提取坐标值
        x_min = self._read_bits(buffer, n_bits, True)
        x_max = self._read_bits(buffer, n_bits, True)
        y_min = self._read_bits(buffer, n_bits, True)
        y_max = self._read_bits(buffer, n_bits, True)
        
        return Rect(x_min, x_max, y_min, y_max)
    
    def _read_bits(self, buffer: int, n_bits: int, signed: bool = False) -> int:
        """从位缓冲区读取位"""
        mask = (1 << n_bits) - 1
        value = buffer & mask
        if signed and value >= (1 << (n_bits - 1)):
            value -= (1 << n_bits)
        return value
    
    def _parse(self) -> bool:
        """解析 SWF 文件"""
        # 读取签名 (3 字节)
        signature = self._file.read(3).decode('ascii')
        if signature not in ['FWS', 'CWS', 'ZWS']:
            print(f"Invalid SWF signature: {signature}")
            return False
        
        # 读取版本
        version = self._read_ui8()
        
        # 读取文件长度
        file_length = self._read_ui32()
        
        # 读取帧大小
        frame_size = self._read_rect()
        
        # 读取帧率
        frame_rate = self._read_fixed8()
        
        # 读取帧数
        frame_count = self._read_ui16()
        
        self.header = SWFHeader(
            signature=signature,
            version=version,
            file_length=file_length,
            frame_size=frame_size,
            frame_rate=frame_rate,
            frame_count=frame_count
        )
        
        # 解析标签
        self._parse_tags()
        
        return True
    
    def _parse_tags(self):
        """解析所有标签"""
        while True:
            # 读取标签头
            header = self._read_ui16()
            tag_type = (header >> 6) & 0x3FF
            tag_length = header & 0x3F
            
            # 如果标签长度是 0x3F (63)，则需要额外读取 32 位
            if tag_length == 0x3F:
                tag_length = self._read_ui32()
            
            # 获取标签名称
            tag_name = self._get_tag_name(tag_type)
            
            # 读取标签数据
            data = self._file.read(tag_length) if tag_length > 0 else b''
            
            # 创建标签对象
            tag = SWFTag(
                tag_type=tag_type,
                tag_length=tag_length,
                tag_name=tag_name,
                data=data
            )
            
            # 解析特定标签内容
            tag.content = self._parse_tag_content(tag)
            
            self.tags.append(tag)
            
            # 遇到 End 标签则停止
            if tag_type == 0:
                break
    
    def _parse_tag_content(self, tag: SWFTag) -> Optional[Dict[str, Any]]:
        """解析特定标签的内容"""
        content = {}
        
        if tag.tag_type == 9:  # SET_BACKGROUND_COLOR
            if len(tag.data) >= 3:
                r, g, b = tag.data[0], tag.data[1], tag.data[2]
                content['background_color'] = f"#{r:02X}{g:02X}{b:02X}"
        
        elif tag.tag_type == 39:  # DEFINE_FONT2
            if len(tag.data) >= 2:
                font_id = struct.unpack('<H', tag.data[0:2])[0]
                content['font_id'] = font_id
                content['font_name'] = "Unknown"  # 需要进一步解析
        
        elif tag.tag_type == 75:  # DEFINE_FONT3
            if len(tag.data) >= 2:
                font_id = struct.unpack('<H', tag.data[0:2])[0]
                content['font_id'] = font_id
        
        elif tag.tag_type == 37:  # DEFINE_EDIT_TEXT
            if len(tag.data) >= 2:
                text_id = struct.unpack('<H', tag.data[0:2])[0]
                content['text_id'] = text_id
        
        elif tag.tag_type == 38:  # FRAME_LABEL
            content['label'] = tag.data.decode('utf-8', errors='ignore').rstrip('\x00')
        
        elif tag.tag_type == 77:  # METADATA
            content['metadata'] = tag.data.decode('utf-8', errors='ignore').rstrip('\x00')
        
        elif tag.tag_type == 1:  # SHOW_FRAME
            content['description'] = "渲染当前帧"
        
        elif tag.tag_type == 69:  # FILE_ATTRIBUTES
            if len(tag.data) >= 4:
                flags = struct.unpack('<I', tag.data[0:4])[0]
                content['has_metadata'] = bool(flags & 0x10)
                content['has_swf_action'] = bool(flags & 0x08)
        
        return content if content else None
    
    def _get_tag_name(self, tag_type: int) -> str:
        """获取标签名称"""
        try:
            return SWFTagType(tag_type).name
        except ValueError:
            return f"Unknown_{tag_type}"
    
    def get_summary(self) -> str:
        """获取解析摘要"""
        if not self.header:
            return "未解析文件"
        
        lines = []
        lines.append("=" * 50)
        lines.append("SWF 文件解析结果")
        lines.append("=" * 50)
        lines.append(f"文件路径: {self.filepath}")
        lines.append(f"签名: {self.header.signature}")
        lines.append(f"版本: {self.header.version}")
        lines.append(f"文件大小: {self.header.file_length} bytes")
        lines.append(f"帧尺寸: {self.header.frame_size}")
        lines.append(f"帧率: {self.header.frame_rate} fps")
        lines.append(f"帧数: {self.header.frame_count}")
        lines.append("-" * 50)
        lines.append(f"标签数量: {len(self.tags)}")
        
        # 统计标签类型
        tag_counts = {}
        for tag in self.tags:
            tag_counts[tag.tag_name] = tag_counts.get(tag.tag_name, 0) + 1
        
        lines.append("标签统计:")
        for name, count in sorted(tag_counts.items()):
            lines.append(f"  - {name}: {count}")
        
        return "\n".join(lines)
    
    def get_tag_details(self) -> List[Dict[str, Any]]:
        """获取所有标签的详细信息"""
        results = []
        for i, tag in enumerate(self.tags):
            info = {
                'index': i,
                'type': tag.tag_type,
                'name': tag.tag_name,
                'length': tag.tag_length
            }
            if tag.content:
                info.update(tag.content)
            results.append(info)
        return results


def analyze_swf(filepath: str, verbose: bool = False) -> Dict[str, Any]:
    """
    分析 SWF 文件
    
    Args:
        filepath: SWF 文件路径
        verbose: 是否显示详细信息
    
    Returns:
        包含解析结果的字典
    """
    parser = SWFParser(filepath)
    
    if not parser.open():
        return {'error': 'Failed to parse SWF file'}
    
    try:
        result = {
            'header': {
                'signature': parser.header.signature,
                'version': parser.header.version,
                'file_length': parser.header.file_length,
                'frame_size': str(parser.header.frame_size),
                'frame_rate': parser.header.frame_rate,
                'frame_count': parser.header.frame_count
            },
            'tag_count': len(parser.tags),
            'tags': parser.get_tag_details() if verbose else None
        }
        
        if verbose:
            print(parser.get_summary())
        
        return result
        
    finally:
        parser.close()


def extract_images(swf_path: str, output_dir: str) -> List[str]:
    """
    从 SWF 文件中提取图像
    
    Args:
        swf_path: SWF 文件路径
        output_dir: 输出目录
    
    Returns:
        提取的图像文件路径列表
    """
    import zlib
    
    parser = SWFParser(swf_path)
    if not parser.open():
        return []
    
    extracted = []
    
    try:
        os.makedirs(output_dir, exist_ok=True)
        
        for tag in parser.tags:
            # DEFINE_BITS (6), DEFINE_BITS_JPEG2 (21), DEFINE_BITS_JPEG3 (35)
            if tag.tag_type in [6, 21, 35]:
                try:
                    # 跳过前 4 字节 (Character ID)
                    if len(tag.data) > 4:
                        image_data = tag.data[4:]
                        
                        # 检查 JPEG 头
                        if image_data[:2] == b'\xFF\xD8':
                            filename = f"image_{tag.tag_type}_{len(extracted)}.jpg"
                        elif image_data[:4] == b'CWS' or image_data[:4] == b'FWS':
                            # 可能是压缩的
                            continue
                        else:
                            filename = f"image_{tag.tag_type}_{len(extracted)}.png"
                        
                        filepath = os.path.join(output_dir, filename)
                        with open(filepath, 'wb') as f:
                            f.write(image_data)
                        extracted.append(filepath)
                        
                except Exception as e:
                    print(f"Error extracting image: {e}")
    
    finally:
        parser.close()
    
    return extracted


def main():
    """主函数"""
    if len(sys.argv) < 2:
        print("Usage: python swf_parser.py <swf_file> [--extract-images]")
        print("       python swf_parser.py <swf_file> --verbose")
        sys.exit(1)
    
    filepath = sys.argv[1]
    
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        sys.exit(1)
    
    verbose = '--verbose' in sys.argv
    extract = '--extract-images' in sys.argv
    
    if extract:
        output_dir = filepath + '_images'
        print(f"Extracting images to: {output_dir}")
        images = extract_images(filepath, output_dir)
        print(f"Extracted {len(images)} images")
        for img in images:
            print(f"  - {img}")
    else:
        result = analyze_swf(filepath, verbose=verbose)
        if 'error' in result:
            print(f"Error: {result['error']}")
            sys.exit(1)
        
        print(f"\nSWF File: {filepath}")
        print(f"Version: {result['header']['version']}")
        print(f"Size: {result['header']['file_length']} bytes")
        print(f"Frame Rate: {result['header']['frame_rate']} fps")
        print(f"Frame Count: {result['header']['frame_count']}")
        print(f"Tags: {result['tag_count']}")


if __name__ == '__main__':
    main()
