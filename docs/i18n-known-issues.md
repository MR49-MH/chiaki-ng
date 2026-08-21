# i18n 已知问题（zh_CN 翻译遗留）

> 记录于 2026-08-21，commit 082c0538（首次中文翻译）之后用户实测反馈。
> 状态：待修复。修复时重跑 scripts/i18n/ 流水线即可。

## 1. 设置页「视频」标签布局错位

- 现象：切到简体中文后，视频标签页的 标签/控件/默认值提示 三列对不齐
- 原因分析：SettingsDialog.qml 的 GridLayout 行内，控件多用 `Layout.preferredWidth: 400`
  固定宽度，而提示列 Label 跟随文本隐式宽度；中文文案长度与英文差异大，
  导致第三列（默认值提示）与控件的视觉对齐被破坏；部分长标签可能换行挤压行高
- 修复方向：
  - 给三列GridLayout显式列宽或让提示列 `Layout.fillWidth` + 左对齐统一基线
  - 或改用 `ColumnLayout + RowLayout` 每行固定 [label 宽度][控件弹性][提示固定]
  - 只需调 SettingsDialog.qml 结构，翻译文件无需改动

## 2. 部分界面仍是英文

- 排查方向（按可能性排序）：
  1. QML 中**未包 qsTr() 的硬编码英文**（上游遗留，如 DialogView.qml 全文 0 个 qsTr）
     —— 用 `grep -P 'text:\s*"' gui/src/qml/*.qml'` 找出后补 qsTr 并重跑流水线
  2. C++ 侧 tr()/原生字符串（chiaki 库错误消息、QMessageBox 等）——本次只翻了 QML
  3. 非字面量 qsTr（数字格式化 toFixed 等）——属正常不需翻译
- 修复流程：补 qsTr → `python scripts/i18n/gen_ts.py` → 补译新增条目到 trans_*.json
  → `python scripts/i18n/merge_ts.py` → 提交重建
