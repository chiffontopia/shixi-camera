/*
 * pinyin_test.c — 拼音输入法逻辑自测（主机程序，不参与板上构建）
 *
 * 覆盖三件容易写错、错了又不容易发现的事：
 *   1) 「还能不能继续打字」的判定（"z" 能，"zz"/"xyz" 不能）
 *   2) 音节切分（"nihao" 应切出 "ni"，决定候选条显示谁）
 *   3) 候选顺序（第一个字就是空格/回车默认取的那个）与 e 块是否补上
 *
 * 用法：make pinyin-test
 */
#include <stdio.h>
#include <string.h>

#include "pinyin.h"

static int fails;

static void ck(const char *what, int got, int want)
{
    if (got != want) {
        printf("  FAIL %-36s got=%d want=%d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-36s = %d\n", what, got);
    }
}

static void ck_first_char(const char *py, const char *want)
{
    const char *hz = pinyin_lookup(py);
    char tag[64];
    snprintf(tag, sizeof(tag), "lookup(\"%s\") 首字是 %s", py, want);
    ck(tag, hz && strncmp(hz, want, strlen(want)) == 0, 1);
}

int main(void)
{
    printf("== 能否继续输入 ==\n");
    ck("acceptable(\"z\")", pinyin_acceptable("z"), 1);
    ck("acceptable(\"zz\") 应拒绝", pinyin_acceptable("zz"), 0);
    ck("acceptable(\"xyz\") 应拒绝", pinyin_acceptable("xyz"), 0);
    ck("acceptable(\"\")", pinyin_acceptable(""), 0);
    ck("acceptable(\"nih\") 前缀", pinyin_acceptable("nih"), 1);
    ck("acceptable(\"nihao\") 两音节", pinyin_acceptable("nihao"), 1);

    printf("\n== 音节切分 ==\n");
    ck("first_syllable(\"nihao\")", pinyin_first_syllable("nihao"), 2);
    ck("first_syllable(\"hao\")", pinyin_first_syllable("hao"), 3);
    ck("first_syllable(\"zhuang\")", pinyin_first_syllable("zhuang"), 6);
    ck("first_syllable(\"z\") 未成音节", pinyin_first_syllable("z"), 0);
    ck("first_syllable(\"ma\")", pinyin_first_syllable("ma"), 2);

    printf("\n== 候选 ==\n");
    ck_first_char("ni", "你");
    ck_first_char("hao", "好");
    ck_first_char("ma", "吗");
    ck_first_char("lv", "旅");          /* ü -> v 的输入约定 */
    ck_first_char("zhong", "中");
    ck("lookup(\"e\") 非空（原表缺整个 e 块）", pinyin_lookup("e") != NULL, 1);
    ck("lookup(\"er\") 非空（儿）", pinyin_lookup("er") != NULL, 1);
    ck("lookup(\"zzz\") 应为空", pinyin_lookup("zzz") == NULL, 1);

    printf("\n%s（失败 %d 项）\n", fails ? "**有失败**" : "全部通过", fails);
    return fails ? 1 : 0;
}
