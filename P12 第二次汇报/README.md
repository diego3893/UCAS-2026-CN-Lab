# 第二阶段实验总结

12页，约8分钟（可按5—10分钟安排），UCAS主题tech风格、顶部章节导航。范围仅为P6路由器转发、P8网络路由、P9高效IP路由查找和P10网络地址转换。

## 1 使用文件

- slides.pptx：可编辑PowerPoint，正文为原生文字，截图为图片。
- slides.md：Slidev源稿，每页末尾的HTML注释是讲稿。
- 讲稿.md：独立逐页讲稿，建议用时合计480秒。
- 素材来源.md：原报告章节、原截图文件及性能统计口径。
- brief.md：12页内容与视觉方案。
- public/：实验截图原样副本；原报告及图片未修改。
- pptx-preview/：用PowerPoint实际渲染的逐页图片。
- .slidev-check/slides/：网页逐页检查记录与截图。

## 2 网页预览与修改

当前本地预览：http://localhost:3036/ 。演讲者模式：http://localhost:3036/presenter/ 。服务停止后，在本目录执行：

```powershell
npm run dev -- --port 3036
```

修改slides.md后网页自动更新。网页版本与PowerPoint为不同产物；修改源稿后需要重新导出PPTX。PowerPoint中的文字可以直接编辑，但不会反向更新slides.md。

## 3 检查与导出

当前依赖已就绪。换机器时使用Node.js 22.18及以上，再运行npm install。通常安装Chromium后执行：

```powershell
npx playwright install chromium
npm run brief:check
npm run check -- --strict
npm run build
npm run export
npm run verify:pptx
```

本机使用已安装的Edge完成检查和导出，对应命令为：

```powershell
npm run check -- --strict --browser msedge
npm run export -- --executable-path 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
```

网页/export页面的PPTX按钮会生成整页图片，需保留可编辑文字时使用上面的命令行导出。dist/为构建的网页，需通过HTTP服务访问。

## 4 素材与兼容性

使用本机微软雅黑，字体不嵌入PPTX。报告图片完整保留，NAT细节由上方原生文字明确标出；投影时无需逐字念Wireshark首部。封面日期仅写2026年11月，实际汇报日可自行修改。

本项目使用非官方社区主题slidev-theme-ucas；保留NOTICE.md与THIRD_PARTY_NOTICES.md，不代表学校官方授权。材料只在本地使用，未上传公开网站。
