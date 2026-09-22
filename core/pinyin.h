/*
 * pinyin.h — 拼音输入法查表（音节 -> 候选字）
 *
 * 候选表由 tools/make_pinyin_dict.py 生成（core/pinyin_dict.h，410 音节 / 26731 字），
 * 覆盖字体能画出的全部 CJK 范围，且候选按常用度排序（第一个字直接回车/空格可取）。
 */
#ifndef SHIXI_PINYIN_H
#define SHIXI_PINYIN_H

/* 精确查找一个完整音节，返回候选字串（UTF-8，常用度降序）；没有返回 NULL */
const char *pinyin_lookup(const char *py);

/* 这一串字母是否还能继续输入：整串能拆成若干音节，或末尾是某个音节的前缀 */
int pinyin_acceptable(const char *py);

/* 开头一个完整音节的字节长度（最长 6，如 zhuang）；0 表示开头不是完整音节 */
int pinyin_first_syllable(const char *py);

#endif /* SHIXI_PINYIN_H */
