# 高级功能卡片实机反馈 / Advanced features card runtime feedback

本记录仅覆盖用户提供截图及明确反馈中的可观察问题，不代表完整 UI、解锁或多语言实机验收。

This record covers observable issues in the user's screenshots and explicit feedback; it does not establish complete UI, unlock or localization runtime acceptance.

| 尝试提交 / Try commit | 用户实机反馈 / User runtime feedback | 结论 / Result |
| --- | --- | --- |
| `d6014233863aff9108a9383f447f459678ac4ab5` | 展开区背景、边框和内边距与相邻卡片不一致；照片符号不符合图标美化语义 / Expanded background, border and padding differ from adjacent cards; the picture symbol does not match icon beautification | 视觉验收失败 / Visual acceptance failed |
| `f61057df097fadbbed8e5642cff13be334d7e554` | 高级功能已位于语言之后，快捷导航跳转卡片已移除；高级功能标题仍有额外左内边距，右侧信息图标被裁切 / Advanced features appears after Language and the Quick Navigation jump card is absent; extra left padding remains and the information icon is clipped | 两处布局仍未通过 / Two layout issues remain unaccepted |

证据为本对话用户附图 `codex-clipboard-97d0f823-8c54-4d82-bd14-a6484b272ca1.png` 与 `codex-clipboard-3debc6c1-5125-4516-b79e-01fdf1cb4920.png`。截图未包含运行二进制哈希，反馈关联对应候选的已交付结构；不声称完成全部状态、主题、键盘或尺寸验收。

Evidence is the user's attachments named above. The screenshots do not include a running-binary hash; feedback is associated with the delivered structures of the corresponding candidates. No complete state, theme, keyboard or size acceptance is claimed.

这些失败反馈不推翻对应的编译与定向测试通过记录，也不把那些记录升级为视觉通过。后续源码调整应以独立 `try` 提交保存，并继续等待原生设置窗口验证。

These failures do not negate the recorded compilation and targeted-test results, nor do those results establish visual acceptance. Subsequent source adjustments must be saved in a separate `try` commit and await native settings-window validation.
