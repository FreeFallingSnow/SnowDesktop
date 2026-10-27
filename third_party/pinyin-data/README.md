# pinyin-data

汉字拼音数据，用于 SnowDesktop 本地名称搜索的全拼与首字母匹配。

## 来源

- 上游：[mozillazg/pinyin-data](https://github.com/mozillazg/pinyin-data)。
- 本地记录：随仓库保存的 `kMandarin.txt`；当前未记录其上游提交或版本号。
- 版权：Copyright (c) 2016 mozillazg。
- 许可证：MIT，完整声明见 [LICENSE](LICENSE)。

## 本地文件与使用方式

- [kMandarin.txt](kMandarin.txt)：上游来源数据。
- [pinyin_full_table.cpp](pinyin_full_table.cpp) 与 [pinyin_full_table.h](pinyin_full_table.h)：从数据生成的紧凑 C++ 查询表。
- [name_pinyin.h](../../src/navigation/name_pinyin.h) 与 [search_match.cpp](../../src/navigation/search_match.cpp)：在名称查询中使用上述表。

发行包随附 `licenses/PinyinData-LICENSE.txt`。
