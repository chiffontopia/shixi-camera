/*
 * stb_impl.c — stb 单头库的实现单元（只在这里定义实现宏）
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          /* 只用内存接口，避免不必要的 stdio 依赖 */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* 字体：运行时栅格化（取代原预生成位图图集） */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
