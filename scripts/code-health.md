# C++ 代码健康调查

从两处取候选。第一处是产品源码本身，包括平时少改的公共头、状态所有者、多处实现的同一规则和跨目标依赖。第二处是历代开发分支的共同修改。两组分别取样，再打开调用方核对修改原因；发布前的集中开发、目录迁移、协议常量和独立二进制的相似接线都可能放大指标。

## 建立可复查的基线

在仓库根目录的 PowerShell 7 中运行。每个基线使用新的任务证据目录。首次分析用 Python 标准库和 Git 固定源码、主线和各本地 `release/*` 提交。重放已有调查可向脚本传入 `--replay <上次的 inventory.json>`。

```powershell
$out = Join-Path $PWD '.codex-probes/health-baseline-example'
python scripts/code_health_inventory.py "$out/inventory"
$inventory = Get-Content -Raw "$out/inventory/inventory.json" | ConvertFrom-Json
Import-Csv "$out/inventory/files.csv" | Sort-Object { [int]$_.reachableIn } -Descending | Select-Object -First 15
Import-Csv "$out/inventory/cochange.csv" | Select-Object -First 15
```

- `files.csv` 记录当前路径的直接引用者、可反向到达的文件、直接引用数及开发提交数。`includes.csv` 可沿边检查实际依赖。范围包括 `src/`、`tools/snowwidget/` 和 `steam_bridge/src/` 的原生源码，排除生成的 acrylic 噪声数组。项目维护的上游改编代码须再按第三方声明甄别。新增源码根或生成文件时复核脚本内的范围。
- 包含图只解析双引号字面量，依次查同目录和所属源码根。它包含未启用的条件分支，遗漏宏、SDK、生成头、PCH、其他目标包含目录及链接边。`reachableIn` 是文件数。判断消费者时，再核对当前 CMake 源清单和已有 File API 的 Release 配置、目标源及依赖；记录与当前版本不匹配的旧回复。
- Git 取各开发分支可达提交的并集，剔除 `main` 可达提交和合并提交。计数单位是触及文件的提交。一次改动超过 20 个范围内原生文件时，仅保留频次，排除该次共改样本。`cochange.csv` 同时给出共改次数和两端各自的小提交数。复核重命名用 `git log --follow --no-merges --branches=release/* --not main -- <单个文件>`，再读代表性差异。早期只有发布压缩历史的时期缺少开发粒度。

在同一源码快照运行 [jscpd](https://github.com/kucherenko/jscpd) 和 [Lizard](https://github.com/terryyin/lizard)。以下命令使用任务私有工具目录；已有相同版本可直接使用。清单内工作树差异需要单独保存，与提交快照分别说明。

```powershell
uv venv --python 3.13 "$out/env"
uv pip install --python "$out/env/Scripts/python.exe" lizard==1.26.1
npm.cmd install --prefix "$out/js" --no-audit --no-fund --no-package-lock jscpd@5.4.3
$cpd = "$out/js/node_modules/.bin/jscpd.cmd"
$lizard = "$out/env/Scripts/lizard.exe"
$tree = New-Item -Type Directory "$out/tree" -ErrorAction Stop
git archive -o "$out/source.tar" $inventory.source -- src tools/snowwidget steam_bridge/src
tar -xf "$out/source.tar" -C $tree.FullName
$pattern = '**/*.{h,cpp,hpp,cc,c,cxx,inc}'
Push-Location $tree.FullName
try {
    & $cpd . --pattern $pattern --ignore '**/acrylic_noise_asset.h' --mode mild --min-lines 8 --min-tokens 80 --reporters json --output "$out/clones" --baseline "$out/fingerprints.json" --update-baseline --fail-on-empty
    if ($LASTEXITCODE) { throw 'jscpd failed' }
    & $lizard -l cpp . -x '*/acrylic_noise_asset.h' --csv -o "$out/functions.csv"
    if ($LASTEXITCODE) { throw 'Lizard failed' }
} finally { Pop-Location }
```

记录 jscpd 的 `sources`、`clones`、`duplicatedTokens`，保留报告、指纹与参数；阈值以下的短文件及工具不识别的扩展名会影响分母。Lizard CSV 的第 2、7、8、10、11 列分别给出函数 CCN、路径、名称及起止行。对分派器要读真实分支、状态写入和取消路径，宏和本地 lambda 会影响估算。

## 选择并验收一个单元

在任务记录为每项写下稳定的符号、调用方、状态的创建/写入/销毁者、目标和一项可观察的验收结果。标出适合先迁移的消费者、应保留的差异和退回边界。按职责问题的影响、证据可信度和能否单独验证安排批次，保留一项已审阅的合理重复作对照。

同一规则在多个消费者中执行时，先确认输入、默认值、异常和输出兼容，再迁移到实际所有者；调用方保留自己的业务策略。跨生命周期的服务可先在现有入口后引入窄接口，旧实现继续工作，每批迁移一类调用并比较结果；适合确实需要多批替换的路径。参考 [LLVM 的 JSON 输出实现](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Support/JSON.cpp)、[Fowler 的 Branch by Abstraction](https://martinfowler.com/bliki/BranchByAbstraction.html) 和 [Software Engineering at Google 的分批变更实践](https://abseil.io/resources/swe-book/html/ch22.html)。

## 后续改动的增量复查

1. 从已保存的 `inventory.json` 取基准 SHA。用 `git diff --name-status -M <基准> HEAD`、`git diff HEAD --name-status` 及 `git ls-files --others --exclude-standard` 取得新增、重命名、已提交和在编改动。按变更路径查询原始 CSV、反向包含与候选清单，历史增量使用 `git log <基准>..HEAD -- <路径>`。模块变更时只复读相关调用方。分支历史重写、源码根、包含关系变化或工具口径改变时，重建对应证据，保留原基线。
2. Lizard 可用 `-f <路径清单> --csv` 仅查涉及的源码。克隆是跨文件关系，应保留未改对照语料。将同口径的新快照放在新的目录，使用上面的 jscpd 参数，传入原 `--baseline` 并**去掉** `--update-baseline`，将报告输出到新目录；检查 `isNew`、两端符号和旧片段。移动、上下文或阈值边界可能改变指纹。单个文件的局部扫描不能发现与库内未改文件的新重复。
3. 一批只比较事先选定的规则定义数、依赖边、业务写入点或真实入口结果。将本批结论、限制和待办写入忽略的任务目录，并在本地提交正文记录关键指标及验收命令。现有手动 CI 入口可按需归档同样的报告；默认编译、CTest 与 CI 无需新增步骤。
