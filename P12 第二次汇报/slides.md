---
theme: ucas
layout: cover
title: 第二阶段实验总结
lang: zh-CN
presenter: 张晨轩 · 2024K8009929011
school: 中国科学院大学 · 计算机网络研讨课
subtitle: 路由器转发 · 网络路由 · 高效IP查找 · NAT
date: 2026年11月
canvasWidth: 1280
aspectRatio: 16/9
fonts:
  sans: Microsoft YaHei
  mono: Consolas
themeConfig:
  style: tech
  navigation: top
  sections: ['实验主线', '实现与证据', '问题与心得']
  sectionIndex: 1
  footer: 计算机网络研讨课 · 第二阶段实验总结
  showPageNumber: true
---

# 从路由转发到地址转换

<!-- 10秒。同学们好，我汇报的是第二阶段四个实验：路由器转发、网络路由、高效IP路由查找和NAT。我想用一次跨网络通信，把这四个实验的分工和联系串起来。 -->
---
sectionIndex: 1
---

# 四个实验，围绕同一次跨网通信

<div class="route-strip"><span>主机</span><b>→</b><span>路由器</span><b>→</b><span>后续网络 / NAT</span><b>→</b><span>服务器</span></div>

<div class="quad">
<div><label>06 · 路由器转发</label><h3>这一跳怎么走？</h3><p>查路由、解析下一跳MAC、重写帧头</p></div>
<div><label>08 · 网络路由</label><h3>路由表从哪里来？</h3><p>Hello发现邻居，LSU交换拓扑</p></div>
<div><label>09 · 高效IP查找</label><h3>每包怎样查得更快？</h3><p>保持最长匹配，减少查找层数</p></div>
<div><label>10 · 网络地址转换</label><h3>地址改变后如何返回？</h3><p>改写IP/端口，维护双向映射</p></div>
</div>



<!-- 25秒。这四个实验不是四套独立功能。报文经过路由器时，需要知道下一跳；下一跳来自路由表，而路由协议负责产生这张表。表很大时，还要考虑每个包怎么查得更快。如果中间经过NAT，还要处理地址端口改变后的返回路径。这里的编号沿用课程材料，没有额外的07实验。 -->
---
sectionIndex: 2
---

# 06 路由器转发：一跳一跳完成IP交付

<div class="principle-cols">
<div class="principle-card"><label>核心规则</label><h3>最长前缀匹配，确定下一跳</h3><ul><li>多条路由同时匹配时，选择最长前缀。</li><li>直连取目的主机；非直连取路由网关。</li><li>ARP只查询当前链路上的下一跳MAC。</li></ul></div>
<div class="principle-card"><label>实现流程</label><h3>收包后先区分本机与转发</h3><ul><li>本机ICMP请求：生成回显应答。</li><li>转发：检查TTL与路由，再更新TTL、校验和与MAC头。</li><li>ARP未命中先入队，收到应答后再发送。</li></ul></div>
</div>
<div class="lesson"><b>异常也需要返回路径：</b>无路由返回“网络不可达”；ARP失败返回“主机不可达”；TTL耗尽返回超时。</div>
<p class="reference">依据：P6报告2—3章。普通IP转发保留源/目的IP，Ethernet头按下一跳重写。</p>

<!-- 55秒。路由器不需要提前知道从源到目的的完整路径，只需要确定当前这一跳。目的IP可能同时命中默认路由、网络路由和主机路由，选择前缀最长的一条。如果目的主机就在直连网段，下一跳就是它；否则下一跳是路由表里的网关。然后才用ARP把下一跳IP解析成MAC，ARP不会到远端网络去找服务器。实现时先区分发给本机的报文和需要转发的报文。转发要检查TTL、更新IP校验和，并填写新的MAC头。暂时不知道MAC时把报文放入等待队列。另一个重要部分是错误反馈：无路由、ARP失败和TTL耗尽应该返回不同的ICMP信息。 -->
---
sectionIndex: 2
---

# 06 验证与调试：连通之外还要检查反馈

<div class="split">
<div>
<h3>验证范围</h3><ul><li>单路由器与三跳双向ping</li><li>未知网络、未知主机与TTL超时</li><li>traceroute检查实际路径</li></ul><p>三跳结果：<strong>2/2、0%丢包</strong>；TTL从64变为61。</p></div>
<figure><img src="/p6-ping.jpg" alt="P6三跳拓扑双向ping结果"/><figcaption>来源：P6报告5.4，evidence/5.jpg</figcaption></figure>
</div>

<div class="lesson"><b>调试：</b>能转发却无本机ICMP反馈。IP_DATA依赖ihl；应先初始化IP头，再计算ICMP位置。</div>

<!-- 40秒。验证时我先检查单路由器和三跳拓扑能否通信，再检查TTL、traceroute及错误反馈。右图双向各发两个包都得到回复，TTL为61。一个真实调试问题是，最初能经过路由器转发，却没有本机ICMP和错误反馈。既然普通转发可用，就把范围缩到新生成的ICMP包，发现IP头还没初始化就计算了IP_DATA，此时ihl为0，ICMP覆盖了IP头。调整为先初始化IP头再计算负载位置后恢复正常。 -->
---
sectionIndex: 2
---

# 08 网络路由：从邻居信息生成转发表

<div class="three-stages">
<div><label>1 · Hello</label><h3>发现邻居</h3><p>每5秒发送Hello，刷新邻居存活时间。</p><p>约15秒未收到时删除邻居，触发更新。</p></div>
<div><label>2 · LSU</label><h3>交换链路状态</h3><p>通告网络、掩码与邻居RID，向其他邻居泛洪。</p><p>用发布者和序号过滤重复或过期消息。</p></div>
<div><label>3 · Dijkstra</label><h3>计算下一跳</h3><p>以路由器为顶点，本实验每条链路代价为1。</p><p>沿最短路回溯第一跳，生成目的网络的路由。</p></div>
</div>
<div class="lesson"><b>拓扑变化 → 更新数据库 → 重算路由。</b>保留初始直连路由，只替换动态生成的表项。</div>
<p class="reference">依据：P8报告2—3章。共享网段可能有多个邻居，因此邻居表按接口维护列表。</p>

<!-- 55秒。P6解决的是有了路由之后怎么转发，P8解决这张路由表怎么获得。简化的mOSPF分三步。首先通过Hello发现本地邻居，本实验每5秒发一次，约15秒未收到就判定邻居失效。同一接口通过交换机可能连接多台路由器，所以必须保存邻居列表。第二步用LSU传播网络、掩码和邻居关系，收到新序号才更新并继续泛洪，避免旧信息反复传播。第三步在拓扑图上运行Dijkstra，本实验链路代价都为1，再回溯得到第一跳，转换成下一跳IP和出口。发生断链时需要重算，但不能把直接连接主机和邻居的路由也一起删掉，因此给动态表项独立标记。 -->
---
sectionIndex: 2
---

# 08 验证与调试：断链后切换备用路径

<div class="split mospf">
<div>

<h3>在原拓扑中断开r1—r2</h3>
<div class="path">断链前：<b>r1 → r2 → r4</b></div>
<div class="path active">断链后：<b>r1 → r3 → r4</b></div>
<p>断链后等待40秒，双向ping仍为 <strong>2/2</strong>，traceroute改走备用路径。</p>
<p class="small">40秒是观察点，不是测得的最短收敛时间。</p><div class="lesson"><b>调试：</b>ping已通但部分LSU缺失；邻居发现存在先后。新增邻居后约1秒补发，再逐节点比对数据库。</div>
</div>
<figure><img src="/p8-down.jpg" alt="原拓扑断链后双向ping及traceroute"/><figcaption>来源：P8报告5.3，evidence/4.jpg</figcaption></figure>
</div>

<!-- 40秒。在原拓扑断开实际使用的r1到r2链路，等待40秒后，双向ping仍是两发两收，traceroute改经r3到r4。这个实验还遇到过通信已经成功、数据库却缺少LSU的情况。原因是发送方先发现了邻居，但接收方还没有确认发送方，早到的LSU被忽略。新增邻居后先立即通告，再约1秒补发，并逐节点比较数据库后，状态恢复一致。这也说明一条路径可用，不能证明全部内部状态都正确。 -->
---
sectionIndex: 2
---

# 09 高效IP查找：保持最长匹配，减少层数

<div class="principle-cols">
<div class="principle-card"><label>基本二叉前缀树</label><h3>从最高位开始逐位走</h3><ul><li>0走左分支，1走右分支，前缀末端保存端口。</li><li>查找时记录最近的有效前缀；遇到空分支后返回该结果。</li><li>不会因更长路径不存在而丢失较短的匹配。</li></ul></div>
<div class="principle-card"><label>优化16 / 8 / 8位树</label><h3>用更多地址位直接索引</h3><ul><li>先取高16位，必要时再查两个8位子表。</li><li>新子表继承父槽端口，再由更长前缀覆盖。</li><li>最多访问3层；前缀不必正好落在字节边界。</li></ul></div>
</div>
<div class="lesson"><b>最长匹配示例：</b>128.0.0.0/1 → 端口3；128.0.0.0/2 → 端口4。查询128.0.0.0时应返回4。</div>
<p class="reference">依据：P9报告2章、5.1。主要挑战：子表创建后仍须保留原有短前缀结果。</p>

<!-- 55秒。P9的基本版把前缀变成二叉树，从IP最高位开始，0走左边，1走右边。查找时不能只看最后到达的节点，而要一直保存最近命中的前缀。下面的例子里同一个IP命中斜杠1和斜杠2，应返回更长的斜杠2对应端口4。优化版把逐位查找改成16、8、8位三层索引，一次使用多个地址位，最多读三层。但层数减少不能改变最长匹配语义。关键是新建子表时把父槽的结果复制进去，让更长的前缀再覆盖对应范围。这样进入子表却没有命中更长前缀时，仍能得到原来的较短匹配。 -->
---
sectionIndex: 2
---

# 09 性能结果：加速与空间开销一起比较

<div class="lookup-top">
<div><label>基本二叉前缀树</label><h3>逐位查找</h3><p>沿路径保留最近一次命中</p><strong>16718 μs</strong><p>结构内存37.69 MiB</p></div>
<div><label>优化多比特树</label><h3>16 / 8 / 8位查表</h3><p>最多读取3层，继承短前缀结果</p><strong>1374 μs</strong><p>结构内存64.00 MiB（+69.82%）</p></div>
<div class="ratio"><strong>12.17×</strong><p>五轮批量耗时<br>中位数之比</p></div>
</div>

<figure class="wide-proof"><img src="/p9-runs.jpg" alt="同一转发表五轮查找性能原始记录"/><figcaption>来源：P9报告4.3—4.4；图为evidence/3.jpg。每轮100000次查询，同一编译配置。</figcaption></figure>



<!-- 40秒。路由表正确之后，P9关注逐包查找的代价。基本版逐位走二叉树，始终记住最近命中的前缀；优化版分成16、8、8位三层表，并把短前缀结果传播到子表，避免创建子表后丢掉原来的匹配。同一数据和编译配置下，五轮批量耗时中位数从16718微秒降到1374微秒，比值约12.17。代价是结构内存从37.69增到64MiB。这是空间换时间，不是内存也变小了，更不能把这一次加速比当成所有机器的固定结果。 -->
---
sectionIndex: 2
---

# 10 NAT：地址改写与连接映射

<div class="principle-cols">
<div class="principle-card"><label>SNAT · 内网访问外网</label><h3>改写源IP与源端口</h3><p>多个客户端共享NAT外部IP，用分配的端口区分连接。</p><div class="mini-flow">去程：内网源地址 → 外部源地址<br>回程：外部目的地址 → 内网目的地址</div></div>
<div class="principle-card"><label>DNAT · 外网访问内网服务</label><h3>改写目的IP与目的端口</h3><p>按外部服务端口匹配静态规则，转到对应内网服务器。</p><div class="mini-flow">去程：外部目的地址 → 内网服务地址<br>回程：内网源地址 → 外部服务地址</div></div>
</div>
<div class="lesson"><b>映射表：</b>保存内部、外部和远端IP/端口；先查映射再改包，改写后更新IP与TCP校验和。</div>
<p class="reference">依据：P10报告2章及6章。FIN/ACK、RST或空闲超时触发回收；静态DNAT规则端口继续保留。</p>

<!-- 55秒。NAT增加的是地址空间之间的转换。SNAT用于内网主动访问外网，把源IP和端口换成NAT的外部地址和分配端口；回复回来后再恢复内部目的地址。DNAT则用于从外部访问内网服务，按外部服务端口把目的地址改成内部服务器，回复时恢复对外呈现的源地址。为了完成双向转换，不能只记一个IP，映射中还要保存端口和远端信息，区分同一服务器上的多个连接。改IP也要更新TCP校验和，因为TCP伪首部包含IP地址。连接结束后释放动态映射，但静态DNAT服务端口要继续保留，不能当成临时端口一并归还。 -->
---
sectionIndex: 2
---

# 10 验证：同一报文的地址与端口变化

<div class="nat-flow"><span>内网客户端<br><b>10.21.0.2:60306</b></span><i>SNAT →</i><span>NAT外部地址<br><b>159.226.39.43:12345</b></span><i>→</i><span>服务器<br><b>159.226.39.123:8000</b></span></div>
<div class="nat-proof">
<figure><img src="/nat-in.jpg" alt="SNAT内部接口的SYN，源地址10.21.0.2端口60306"/><figcaption>转换前：P10报告5.2，3-eth0-SYN.jpg</figcaption></figure>
<figure><img src="/nat-out.jpg" alt="同一SYN经SNAT后源地址和端口改变"/><figcaption>转换后：P10报告5.2，3-eth1-SYN.jpg</figcaption></figure>
</div>
<div class="nat-notes"><p><b>DNAT：</b>映射内网服务；改IP也更新TCP校验和。</p><p><b>验证：</b>三种配置均成功下载，文件逐字节一致。</p></div>


<!-- 40秒。NAT复用前面的路由和ARP，但增加了地址转换。图中同一条SYN从内网接口的10.21.0.2端口60306，变成外网接口的159.226.39.43端口12345，目的服务器不变。回包必须通过映射恢复到原客户端。DNAT则把外部服务端口映射到内部服务器，本实验还验证了两台NAT串联。验证时不仅看连接建立，还下载二进制文件并逐字节比较。另外，即使没有改TCP数据，改IP也需要更新TCP校验和，因为TCP包含IP伪首部。截图保留原始证据，口头只强调地址和端口两处变化。 -->
---
sectionIndex: 3
---

# 把四个实验放回同一套网络分工

<table class="theory"><thead><tr><th>理论课中的概念</th><th>在实验中的具体作用</th></tr></thead><tbody>
<tr><td>控制平面与链路状态</td><td>P8交换拓扑信息，产生并更新路由表</td></tr>
<tr><td>转发平面与最长前缀匹配</td><td>P6使用路由；P9在不改变语义的前提下加速查找</td></tr>
<tr><td>分层封装与逐跳交付</td><td>IP决定跨网目的，ARP解析下一跳，MAC头逐跳改写</td></tr>
<tr><td>地址转换与有状态映射</td><td>P10改写IP/端口，并保存回包转换所需信息</td></tr>
</tbody></table>
<div class="closing-line">我的收获：把“通信成功”拆成可逐项检查的条件。</div>
<p class="closing-sub">心得：同时检查通信结果、内部状态和报文格式；性能比较保持相同条件。</p>

<!-- 60秒。最后把四个实验联系起来看。P8属于控制平面，通过分布式交换拓扑信息产生路由表；P6使用这些表项执行转发，P9优化其中每包都要进行的查找。选出下一跳以后，ARP和MAC负责当前链路交付。NAT则增加地址和端口转换以及返回映射，并不能替代路由选择。对我来说，重要的变化是把通信成功拆成了可检查的条件：路由来源对不对，最长匹配有没有保留，报文头和校验和是否正确，回包能否找到原客户端。P8提醒我，ping通不等于内部状态完整；P9提醒我，优化也有空间代价。因此后续验证要结合抓包、状态快照和重复测量，而不是只看一条命令有没有成功。 -->
---
layout: end
sectionIndex: 3
presenter: 张晨轩 · 2024K8009929011
subtitle: 计算机网络研讨课 · 第二阶段实验总结
---

# 谢谢大家！

<!-- 5秒。我的汇报到这里，谢谢大家，欢迎批评指正。 -->

