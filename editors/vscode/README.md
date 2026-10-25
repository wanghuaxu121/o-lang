# O 语言 · VSCode 语法高亮

只有 52 个 token 的小语言，语法文件也很小（三个 JSON）。

## 安装

**方式一（推荐）**：把整个 `editors/vscode` 目录复制到

```
~/.vscode/extensions/olang-0.1.0/
```

重启 VSCode 即可，`.o` 文件自动用 O 语法高亮。

**方式二**：`code --install-extension` 需要先打包（`npx vsce package`），
本项目不依赖 node 工具链，所以没走这条。

## 覆盖范围

- `//` 与 `/* */` 注释
- `"…"` 字符串（含 `\"` 转义）
- `#{...}` 导库、`#mem N`
- `${}` `&{}` `?{}` `^{}` 四种 sigil 与 `!`
- `int/double/float/bool/char/str/struct/fn`、`true/false`
- 数字（整数 / 浮点）、运算符（`== != <= >= && || | ~` 等）

## 已知不做的

- 语义高亮（需要语言服务器，v1.1 不做）
- 括号配对之外的缩进规则（O 的块必须显式 `{}`，不需要）
