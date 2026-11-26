# P1—P5学习与阶段汇报任务上下文

更新时间：2026年11月24日。

## 当前任务 第二阶段实验汇报（12页扩展版已交付）

用户最新要求：上一版每实验过简，参考P7补充各实验原理与实现，允许5—10分钟；末尾单独一页谢谢大家；直接修改PPTX。范围仍仅06路由器转发、08网络路由、09高效IP查找、10 NAT，无07、不含P11。未修改原报告、截图或代码，未重跑网络实验。

项目P12 第二次汇报/已从8页扩到12页，讲稿480秒（约8分钟）：封面10s、主线25s；P6/P8/P9/P10每项原理55s+验证40s；联系与心得60s；单独致谢5s。每项两页明确机制与实现，结果页保存原截图和真实证据；P6/P8调试归入各自结果页，P9讲短前缀继承，P10讲SNAT/DNAT完整双向转换、映射回收和校验和。保持tech顶部三章节导航、16:9、1280x720、微软雅黑。brief先更新后改slides，README、讲稿、素材来源同步12页。原来的8页版已由同路径更新，不另交付旧版。

文件：第二阶段实验总结_张晨轩.pptx与slides.pptx逐字节相同；slides.md、style.css、brief.md、讲稿.md、素材来源.md、README.md、public/原始截图副本、NOTICE.md及THIRD_PARTY_NOTICES.md、dist/网页。严格检查12页0错误0提醒；可编辑PPTX12/12页230个原生文字片段；最终构建成功。用PowerPoint COM只读渲染12页，检查新增3/4/5/6/7/9及11/12页；发现3/4页孤立句号后精简语句，重新严格检查、导出并重渲染这两页。pptx-preview/是实际PPTX截图，.slidev-check/slides/为网页截图与report.json。

数字与证据口径不变：P6三跳2/2、TTL61；P8原拓扑断r1-r2后40s观察点改经r3，不称最短收敛时间；P9五轮中位数16718/1374us比12.17，结构内存37.69→64MiB（+69.82%）；P10同SYN内10.21.0.2:60306→外159.226.39.43:12345。每轮100000查询。讲稿补齐机制与技术原因，不编造调试经历。

环境：原npm GitHub安装停住后已中止，复用本机OS/实例分析/实例分析3同版本依赖副本及锁文件，未写原项目；检查用npm run check -- --strict --browser msedge；导出用npm run export -- --executable-path 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'。Chromium下载已停止，采用Edge。原预览http://localhost:3036/，旧会话81951；停止后npm run dev -- --port 3036可重启。用户后续反馈按页修改，保持brief、讲稿、源稿和PPTX同步；修改源稿不等于PPTX自动更新。未公开上传。

## 当前任务 P11网络传输机制实验一

用户授权完成P11代码、本地测试、正式report.md（核心函数、截图占位）及OJ ZIP。OJ要求cc（C/C）、cp（C服务端/Python客户端）、pc（Python服务端/C客户端），无丢包两节点，server port/client IP port，保留tcp_apps.c线程签名，ZIP<5MB、根目录Makefile、不含Python拓扑。已读取29页课件。原包解压11-tcp_stack/保留，tcp_stack/可本地测试，原始Makefile未改；复用P6七个网络模块及utils.h。

实现：tcp_sock.c三表、绑定/连接/监听/accept/close/引用计数；tcp_in.c握手、正常关闭、合并FIN|ACK、同时关闭、重复FIN、RST；tcp_timer.c TIME_WAIT 2MSL回收（框架1MSL=1s/扫描100ms）；全局递归锁串行化应用/输入/定时器，等待前释放。初始化全部链表节点；main线程参数改静态存储，随机ISN播种；tcp.h改无符号差后有符号模序号比较和64位校验和伪首部累加。tcp.c收包持临时引用，tcp_finish多表清理持临时引用防最后摘表后访问。保留下发应用行为：client连接后1s关闭，server accept后5s关闭；仅连接管理，不宣称完整数据读写、丢包重传和拥塞控制。

本Agent实测：WSL Ubuntu-24.04原始Makefile编译成功。tcp_stack/lab_test.py创建两主机10.0.0.1/24与10.0.0.2/24，单向10ms，无随机丢包；仅用户栈节点屏蔽内核RST/ARP，参考Python端保留内核，两端关闭offload。最终test-results/verified：cc/cp/pc全部通过，每组7条TCP记录/2个FIN、握手与FIN的Seq/Ack、无RST、IP/TCP校验和、C stdout为空及进程存活、关闭状态、主动端TIME_WAIT到CLOSED均检查通过。原始tcp_stack_conn.py可由Python3执行。validation为初版记录，最终以verified为准。

tests/state_unit.c四组ASan/UBSan PASS：合并FIN|ACK与重复FIN/2MSL扫描和引用释放，同时关闭，RST/模序号回绕比较，监听关闭清理半连接子Socket。关闭alignment子检查（packed首部）；单元2MSL用20次扫描模拟，真实Mininet测试实际等待timer。日志test-results/unit.log。原始packed-pointer编译警告仍存在，不称零警告。

交付P11/tcp_stack/本地实现、tests/、report.md、evidence/空目录、逐图本地测试与截图指南.md、tcp-stack-submit.zip（36609字节36文件：15C+20头+Makefile；不含Python/测试/日志/二进制/嵌套ZIP）。ZIP逐项字节一致，独立解压test-results/submission-build后原始Makefile编译通过，SHA256=6d610db55b5e8e4e7f969486d126b8514155491978dfc46eb87b75886b8ed94b。报告三个完整核心函数与源码一致，图1—7占位，数值来自verified。没有上传OJ，不称OJ通过。下一步用户上传ZIP、重测截图并按真实截图更新报告。P1—P10及用户原有P2指南改动未触碰。没有改动Git状态（无提交/暂存）。

## 当前任务 P10网络地址转换

用户授权完成P10代码、本地测试、报告与OJ ZIP（<5MB，C/头文件/原始Makefile，不含Python拓扑，不上传OJ）。公开测试SNAT/exp1.conf、DNAT/exp2.conf、h1-n1(SNAT)-n2(DNAT)-h2自行配置串联NAT，约15s。已读P10课件21页，保留10-nat原始解压，nat/复制实现，复用P6基础7个C+utils.h，Makefile及原exp1/exp2未改。已实现方向判断、远端二元组桶+完整方向键、SYN建表、SNAT端口池/DNAT规则、校验和、FIN/ACK模序号、RST/60s超时回收、配置与资源清理。nat.h新增动态端口归还标记和ACK有效位；tcp.h伪首部改64位累加并回卷至16位，防32位溢出。P1—P9文件未修改。

本Agent实测完成：WSL Ubuntu-24.04，原始Makefile编译成功。nat/lab_test.py在真实Mininet隔离NAT内核代答/转发，交换机用P3用户态程序。SNAT与DNAT分别同时发起两请求，课程http_server.py完整页面与期望IP均一致；页面验证后同端口换静态服务器，下载262146字节二进制并与源文件逐字节比较。串联n1外192.0.2.1，n2外192.0.2.2，n2内10.22.0.254，h2=10.22.0.2，h1=10.21.0.1；自行配置chain-snat/dnat.conf。三拓扑共16/16检查通过，NAT stdout为空、NAT检查时存活。test-results/validation包含transcript、结果JSON、各HTTP文件/日志与8份PCAP。check_pcaps.py核验全部3666条捕获TCP记录IP/TCP校验和、配对SYN转换与TTL通过，见test-results/pcap-check.txt。记录有内外接口重复，不是3666个独立包。

tests/nat_unit.c合成报文测试4组PASS：SYN重传/正反向/多客户端和远端端口隔离/TTL，非法入站和端口耗尽，RST动态端口回收，模拟推进60s超时，FIN/ACK与序号回绕，DNAT回复源8001和服务端口保留。ASan/UBSan未报错，packed框架因非对齐访问关闭alignment子检查，需如实说明；超时不是实际等待60秒。单元日志test-results/unit.log。

报告report.md已写核心3个完整函数（方向、转换、结束判定）、拓扑、双向映射解释、页面/二进制/哈希及抓包结果、debug分析，图1—7占位。本地测试与截图指南.md含启动WSL、各模式交互/下载/抓包/状态机/OJ步骤；交互模式先自动验证后在8000切换为静态文件服务，不能继续期待根页显示动态IP。报告当前数据来自validation，若用户重测须更新临时源端口/SHA/捕获数。SNAT首次client端口46744/59746→外12345/12346；串联首次35704→12345。哈希完整见transcript，报告只取前16位。

交付nat/可本地测试，nat-submit.zip28758字节30项（10C+15头+原始Makefile+原exp1/exp2及chain两配置），不含Python/switch/二进制/参考程序。与源码逐项字节一致，解压test-results/submission-build后独立编译成功。SHA256=e00eaeef9dad76eec0ca57b75d9c63f6438d167e3c683fa74ece99399bb32e01。核心代码片段、围栏、下载哈希及记录数已核对。不曾上传OJ，不声称OJ通过。下一步用户上传ZIP、按指南截图补报告；改C/头/配置后需重新打包。

## 当前任务 P9高效路由查找

用户授权读取P9并完成代码/本地测试/报告/OJ源码ZIP。OJ固定6文件：include/tree.h、include/util.h、main.c、Makefile、tree.c、util.c；ZIP<5MB，无自定义stdout。basic和advanced正确，优化时间至少快2倍且有未公开服务器绝对时限。已读课件12页，保留09-ip_lookup原始解压，复制ip_lookup实现，只改tree.c，所有原接口/main/Makefile不动。

实现：基本逐位node_t二叉树；优化16/8/8位多比特查表，8字节槽{port,child}，UINT32_MAX表示无子表，端口继承覆盖保持LPM。按前缀长度递增插入且同长度保留文件顺序，支持/0、/32、非8位边界、非规范网络地址、重复前缀、端口0、未命中-1。优化arena倍增保留容量，内存报告需记分配容量非仅已用槽。两版create会先释放旧结构。原main规定stdout输出保留，新tree.c只有错误stderr。

本Agent本地实测完成：WSL Ubuntu-24.04，原始gcc -Wall -g，无-O2。tests/run_checks.py三组独立线性LPM oracle（mixed、no-default、empty）各100000查询通过，两版重复构造，ASan/UBSan无报错。原始数据697882前缀、100000查询，五轮均basic_pass=1、advance_pass=1；basic_us=[17653,16134,16100,16058,15787]，advanced_us=[1621,1574,1317,1394,1826]。中位数16100/1574us，平均161.00/15.74ns，中位数之比10.23，最小逐轮比8.65。

2026年11月11日按用户当前截图更新报告：图3为用户新测basic_us=[16710,16718,18119,18379,16345]、advanced_us=[1266,1374,1319,1470,1400]，中位数16718/1374us，平均167.18/13.74ns，中位数之比12.17，最小加速比11.68。报告性能表、平均耗时、正文及结论已全量同步，不再使用上段历史10.23倍结果。图1单独运行18091/1311us且两版pass=1，已明确与图3不同轮；图2三组PASS、图4内存数据一致；oj.jpg整体及3项均Pass，已在正文确认OJ验收通过（依据用户截图，本Agent未上传）。此次只读图片并修改报告/上下文，没有重跑测试或改代码。

内存：sizeof(node_t)=24，1646585节点，39518040字节37.69MiB；优化8字节槽，使用7405824槽59246592字节，容量8388608槽实际分配67108864字节64MiB，比基本增加69.82%。报告按实际容量计数，排除malloc元数据/输入结果数组/构造临时路由，不称RSS或峰值。优化为空间换时间，不能声称空间减少或已测缓存命中率。

交付已完成：P9/ip_lookup/可测试实现及原数据，tests/validate.c和run_checks.py独立核验，report.md含两版完整查找与extend_slot源码、性能与内存表及用户图1—5，另有本地测试与截图指南.md和evidence目录。历史实测记录test-results/run-20261106-085148；当前报告新截图指向用户run-20261111-181840。仅tree.c改变，main/util/两个头文件/Makefile均与附件字节相同。ip-lookup-submit.zip4757字节，严格6文件无test数据和二进制，逐文件与实现一致；解压至test-results/submission-build后原始Makefile编译成功。SHA256=29d1619801200728c3abdc53e7ba5bb9bad163df422d9f6c09a90c1e7c6a685e。用户已提供OJ通过截图，当前报告已据此更新；下一步检查或导出报告。未修改P1—P8文件及用户P2指南改动。

## 当前任务 P8网络路由

用户明确要求按此前方式完成P8，授权代码实现、本地测试、源码ZIP和正式report.md（核心代码、截图占位），不上传OJ。已读P8课件25页和FAQ，保留08-mospf原始解压基线，在mospf/实现。OJ要求：simple_ping在40s后连通；原拓扑断开实际使用链路后40s连通；router+P3用户态switch拓扑30s连通。ZIP<5MB、原始Makefile、仅C和头文件，不含Python或switch、无stdout debug。P8 Makefile未改。

实现完成：继承P6数据面实现，按P8头文件补arpcache_sweep声明。mospf_daemon.c实现Hello/邻居15s老化、LSU30s/事件触发/序号回绕/TTL/数据库40s老化；mospf_database.c实现Dijkstra和动态路由更新，保留内核初始直连表项，只替换带标记的动态路由。rtable_lock保护读写，LPM返回线程局部副本防止解锁后释放。MOSPF_STATE_FILE仅本地显式启用时将邻居、LSA、路由写文件，不打印stdout。

本Agent最终实测：mospf/lab_test.py在真实Mininet中建立课件拓扑，40s后初始双向ping成功；按实际路径断r1—r2，同一拓扑等待40s后双向ping成功、路径改经r3，16/16检查通过。初始路径10.0.1.1→10.0.2.2→10.0.4.4→10.0.6.22；断链后10.0.1.1→10.0.3.3→10.0.5.4→10.0.6.22。switch模式让r1/r2/r3经P3用户态switch共享10.0.2/24；30s双向ping成功，r1同接口两个邻居，9/9检查通过。两种拓扑应答TTL61、ping2/2、无丢包；只是观察点，不是测得最短收敛时间，也不声称故障切换零丢包。脚本只给host配默认网关，router无远端静态路由，关闭内核转发/代答/网卡卸载。环境WSL Ubuntu-24.04，原始Makefile。

最终证据：P8/test-results/simple-verified和switch-verified，transcript.txt、results.json、初始/断链后路由和数据库快照、mospf.pcap。check_evidence.py只读核验四路由器同源LSA通告集合一致，Hello组播/间隔、protocol90、版本、长度、字节序、IP与mOSPF校验和通过，switch共享网段LSU包含多邻居。所有router stdout为空、结束前进程存活。旧simple/switch目录保留调试记录，不作为最终依据。

实际调试：初版switch30s时ping已通但部分数据库缺r3 LSU，因邻居互相发现有先后，早期LSU会被未知发送者检查丢弃。邻居新增/地址变化后立即发布，再设置1秒延迟补发，重测解决。报告6.1据此写真实debug过程。

交付：P8/mospf/本地实现（含lab_test.py、check_evidence.py、Lua），P8/report.md（2个完整核心函数及Dijkstra源码片段，图1—8占位）、本地测试与截图指南.md、evidence目录、mospf-submit.zip。ZIP30117字节，29项（12C+16头文件+原始Makefile），不含Python、switch、参考程序或编译产物；逐项字节与mospf实现一致，原始Makefile哈希一致，解压至test-results/submission-build独立编译成功。ZIP SHA256=5b87fa7e05527edf8d67a98c273080eebb6c5736eff72364c2066707d8ddb30f。源码片段逐字核对、围栏完整。尚未上传OJ，不声称OJ通过。下一步由用户上传ZIP、按指南截图补报告。P1—P7代码报告均未修改，P2指南原有修改保留。后续更改C/头文件需重打包。

## 当前任务 P6路由器转发

用户要求完成P6，已提供OJ的7项验收：本机ping、h1到h2/h3、未知主机/网络、三跳ping及traceroute。提交ZIP不超过5MB，仅C/头文件和原始Makefile，不含Python拓扑或二进制，不输出自定义stdout调试。已交付P6下可本地测试的router目录、router-submit.zip及附3个完整核心函数源码的report.md；截图留图1—图6占位，另有本地测试与截图指南.md。

已读取P6课件21页和FAQ，解压原附件到P6/06-router作只读基线。在P6/router中实现最长前缀匹配、IPv4本机交付/转发、ARP收发、32项缓存/15秒老化/5次请求、ICMP回显及3/0、3/1、11/0等错误反馈。框架`iface_send_packet`会释放packet，发送或入队后由相应函数接管；ARP失败队列锁内摘除、锁外发送ICMP，避免重入死锁。Makefile与原附件SHA256相同，保留已有代码注释，新增逻辑有中文注释。用户P2操作指南有未提交改动，必须保留。

本Agent实测：WSL发行版Ubuntu-24.04，内核6.6.87.2、Python3.12.3、Mininet2.3.0、GCC13.3.0。已补装arptables、iptables、traceroute、ethtool等本地依赖。WSL调用需sandbox升级，root只用于隔离Mininet拓扑。新lab_topology.py自动配置单路由器/3台路由器链式拓扑、静态路由及命名空间内核禁用规则；勿再手动启动第二份router。当前WSL不支持arptables FORWARD链，拓扑无网桥，使用ARP OUTPUT过滤和ip_forward=0即可。所有接口必须关闭tx/rx校验和卸载，不能只关TSO/GSO/GRO，否则原始套接字转发UDP校验和不完整，普通traceroute无法获得终点反馈。

最终运行记录P6/test-results/verified/results.json和transcript.txt：17/17通过，涵盖7类公开功能、双向三跳ping、入口地址、两种traceroute、TTL、ARP老化、失败后通信、进程存活及stdout为空。PCAP只读核验通过IPv4/ICMP/UDP校验和、回显标识/序号/数据和ICMP引用；当前报告中使用的图4(a)截图显示未知主机ARP共5次，第一个相邻间隔约0.470秒，后面三个约1秒。独立LPM检查覆盖默认、/8、/16、/24、/32和空表。未上传OJ，不能声称OJ通过。旧validation*目录是调试记录，不作为最终报告依据。

已修复实际调试问题：ICMP指针先于IP头初始化计算，ihl为0导致覆盖IP头；改为初始化后计算IP_DATA。抓包收尾需--immediate-mode并留1秒采集，避免丢失最后一组快速traceroute的证据。tests/inspect_pcaps.py不强制Linux主机3/3错误只引用28字节，因内核可引用更多；仅对本程序3/0、3/1、11/0检查精确引用长度。

提交包21102字节，23项（9个C、13个头文件和Makefile），无Python、tests、二进制、静态库或嵌套ZIP。解压到test-results/submission-build后原始Makefile独立编译成功，打包内容逐项哈希与router源码一致。ZIP SHA256=f68a7c20bff84efe846ef6c3f07ca839b476100d67a8a1b4014807c8fcb4e93e。当前报告已放入evidence/2.jpg至6.jpg和oj.jpg；核对后仅修正图4(a)ARP首个间隔的正文描述。后续若重新截图，保持图、TTL、数据和结论一致。P1—P5原报告/课件/代码未修改。

## 1 目标与约束

依据P1—P5实际课件、代码、报告，以及只读的CN理论课件，编写面向计网初学者的学习讲解。2026年10月20日用户进一步要求新增Markdown，写出约10分钟阶段汇报每页PPT的文字及配图来源。当前只编写内容稿，不创建PPT，不包含后续路由器转发实验。保留所有既有报告、代码、截图和课件。中文与英文、数字、行内代码相邻不额外加空格，编号标题保留编号后的空格。

用户最新约定：之后的计网实验报告附核心函数源代码，并解释作用与实现理由，不以运行命令替代实现代码，不因此补改既有报告。汇报中实验联系应连贯解释因果与相互依赖，不能仅分别罗列各实验任务。

## 2 当前状态

已完成`P1-P5实验原理与实践详解.md`，约3.3万字符。讲解涵盖基础概念、五次实验的原理和实现、现有数据解读、分层调试、实验与理论联系、12道带答案自测题及来源链接。已提取五份实验课件，读取五份报告，并核对P2关键函数、P3广播与MAC表、P4算法和接收入口、P5实验拓扑与监测配置。理论依据以CN第1章性能和分层、第2章可靠传输和以太网、第3章交换网络为主。

已新增`P1-P5阶段总结PPT逐页内容.md`，12页主汇报预计600秒，另附多拓扑验证和Incast原理2页备份。每页含上屏文字、已有图片及报告章节来源、讲述提示与必要范围限定。已查看P1封装/CDF、P2文件一致性、P3正向吞吐率/环路、P4四节点角色、P5两张曲线等关键素材。2026年10月21日按用户反馈重写第2、12页的实验联系，并新增第17节，以同一次网页访问串起问题递进，另给约一段的口头总结。主汇报页数与时长不变。

## 3 工作文件与下一步

- 已完成输出：`P1-P5实验原理与实践详解.md`。
- 已完成输出：`P1-P5阶段总结PPT逐页内容.md`。
- 原始材料：各P目录课件、报告和实现文件；CN只读。
- 已确认并在文中明确：P4只计算STP角色，未整合P3普通数据转发；固定拓扑收敛不等于验证动态重构；STP不等同于任意加权图的全局最小生成树。
- 已说明P1的curl剩余计时不是纯文件传输时间，ping实际读取平均值；P3教学Hub使用全双工虚拟链路，不能用碰撞解释所有数据；P5实验结果不代表所有参数和所有时刻下的算法优劣。
- 汇报稿保留P3反向Switch未提升与P5CoDel最大RTT高于RED的限制，不编造失败经历或新实验。
- 后续可按用户要求修改页数、详略或制作PPT；当前没有PPTX产物。

## 4 验证状态

学习讲解已检查来源链接、代码围栏和标题格式。逐页稿45处文件链接全部存在，12个主页面时长合计600秒，无中文与英文数字间额外空格；P3、P5数字与当前报告及summary.csv一致。P2文件一致性截图没有展示200状态行，稿中仅将其作为长度、哈希和逐字节比较证据。未修改任何原有报告、代码、图片或CN课件，也未执行实验重测。当前git状态仅新增学习讲解、逐页稿与本上下文文件。
