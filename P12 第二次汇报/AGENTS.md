# 演示项目指引

这是用户的独立演示，brief.md 是沟通契约和逐页计划，源稿是 slides.md，图片放 public/。主题安装于 node_modules/slidev-theme-ucas，版式与叙事接口见其中的 .agents/skills/ucas-slidev/references/。

- 先完成 brief.md，核对用途、听众、目标结果、核心结论、叙事模式、风格和逐页计划；信息变化时先更新简报再改 slides.md。
- 叙事模式与视觉风格独立；论文阅读、组会进展和答辩分别使用 paper-review、progress-review、thesis-defense，其他结构见 references/narratives.md。
- 复用现有版式和组件，保持所选国科大风格与导航一致。每页一个主题，内容过多时精简或拆页，不通过缩小字号塞内容。
- 数字、图片、文献和结论必须有来源；讲稿写在页面末尾的 HTML 注释中。
- 修改后运行 npm run brief:check 和 npm run check，读取 .slidev-check/slides/report.json 并查看逐页截图。修正错误，逐项处理提醒，再重新运行。
- 规则不核对研究事实、全部动画步骤或 PPTX 导出，仍要人工核查。不要删检查、隐藏内容来获得通过。
- 运行 npm run build 核对构建。默认运行 npm run export 导出可编辑 PPTX，再运行 npm run verify:pptx 核对页数、原生文字与标题；用户需要 PDF 时运行 npm run export:pdf。源稿修改后重新导出并检查实际产物。
- 不自动将用户报告提交、推送或公开部署。保留 THIRD_PARTY_NOTICES.md。
