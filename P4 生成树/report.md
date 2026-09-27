# 生成树机制实验报告

张晨轩2024K8009929011

## 1 实验题目

实现简化的生成树协议，在存在环路的二层网络中选举根节点、确定端口角色，并通过阻塞冗余端口生成无环的生成树。

## 2 实验目的

1. 理解二层网络产生环路和广播风暴的原因。
2. 理解生成树协议中根节点、根端口、指定端口和备用端口的含义。
3. 掌握Config报文的优先级比较、根路径开销计算和端口角色更新过程。
4. 在四节点环形拓扑和自定义七节点拓扑中验证生成树的收敛结果。

## 3 实验原理

### 3.1 生成树的作用

交换节点之间存在环路时，同一个二层帧可能被不断复制和转发。生成树协议通过选出一个根节点，并将部分冗余端口置为备用状态，使保留下来的链路能够连接全部节点，同时不再包含环路。

### 3.2 节点ID和端口ID

节点ID由16位优先级和48位MAC地址组成。本实验中节点优先级均为32768，因此MAC地址较小的节点ID也较小。端口ID由8位优先级和8位端口编号组成，默认端口优先级为128。

### 3.3 Config报文及优先级

每个端口保存当前已知的最优Config信息，包括根节点ID、到根节点的路径开销、发送节点ID和发送端口ID。比较两个Config时依次比较以下字段，数值更小者优先：

1. 根节点ID。
2. 根路径开销。
3. 发送节点ID。
4. 发送端口ID。

当多个候选根端口的上述信息完全相同时，再以本地端口ID作为最终判断依据。收到报文时，报文中的多字节字段为网络字节序，必须转换为主机字节序后才能与端口中保存的字段比较。

### 3.4 端口角色

- 根节点：节点ID最小的节点，所有端口均为指定端口。
- 根端口：非根节点上通向根节点的最优端口，每个非根节点只有一个。
- 指定端口：一个链路段上能够通告最优Config的端口。
- 备用端口：既不是根端口也不是指定端口，不参与生成树的数据转发。

节点通过根端口到达根节点，其路径开销为根端口接收到的路径开销与该端口自身开销之和：

```text
root_path_cost = root_port->designated_cost + root_port->path_cost
```

本实验中所有端口的路径开销均为1。

## 4 程序实现

### 4.1 Config优先级比较

在`stp.c`中实现统一的Config比较函数，按照根节点ID、根路径开销、发送节点ID和发送端口ID的顺序进行字典序比较。比较结果小于0表示第一个Config优先级更高。

### 4.2 根端口选择

程序从所有非指定端口中选择优先级最高的端口作为根端口。比较候选根端口时，需要把端口的链路开销加入收到的根路径开销；如果两个候选端口仍然完全相同，再比较本地端口ID。

选出根端口后，程序更新节点认可的根节点、根端口和根路径开销。节点一旦发现优先级更高的根节点，便停止自身的Hello定时发送。

### 4.3 其他端口更新

节点状态改变后，程序为每个端口构造本节点能够发送的Config，并与该端口原来保存的Config比较。如果本节点的Config更优，就把该端口更新为指定端口。完成更新后，仅从指定端口发送新的Config，使更优信息继续向其他节点传播。

### 4.4 Config报文处理

端口收到Config报文后，程序先将报文字段转换为主机字节序，再与端口当前保存的信息比较：

- 收到的Config更优：保存收到的信息，重新选择根端口，更新其他端口并从指定端口发送新Config。
- 收到的Config较差或相同：如果当前端口是指定端口，则回复本端口的更优Config。

报文处理线程和定时器线程都可能访问生成树状态，因此原有程序使用同一把互斥锁保护状态更新。

## 5 实验流程

### 5.1 编译

在`stp/`目录中使用原始Makefile编译：

```bash
make clean
make
```

> 截图占位：原始Makefile编译成功且没有警告，建议保存为`evidence/01-build.png`。

### 5.2 四节点环形拓扑测试

启动课程提供的四节点拓扑：

```bash
sudo mn -c
sudo python3 four_node_ring.py
```

在Mininet中分别启动四个STP进程：

```text
mininet> b1 ./stp > b1-output.txt 2>&1 &
mininet> b2 ./stp > b2-output.txt 2>&1 &
mininet> b3 ./stp > b3-output.txt 2>&1 &
mininet> b4 ./stp > b4-output.txt 2>&1 &
mininet> sh sleep 30
mininet> sh pkill -SIGTERM stp
mininet> sh sleep 2
mininet> exit
```

程序收到`SIGTERM`后输出最终状态。返回普通终端后汇总结果：

```bash
./dump_output.sh 4
```

> 截图占位：四个节点的根节点、路径开销和端口角色，建议保存为`evidence/02-four-node-output.png`。

### 5.3 Config报文抓包

可以在STP进程运行时使用Wireshark观察目的MAC地址为`01:80:c2:00:00:01`的Config报文，或者在Mininet中执行：

```text
mininet> b1 tcpdump -nn -e -vv -i b1-eth0 ether dst 01:80:c2:00:00:01 -c 5
```

重点检查根节点ID、根路径开销、发送节点ID和端口ID，确认不同节点传播的是当前最优Config。

> 截图占位：Wireshark或tcpdump中的Config报文，建议保存为`evidence/03-config-packet.png`。

### 5.4 七节点拓扑测试

自定义拓扑由7个节点和9条链路组成：

```text
b1--b2    b1--b3
b2--b4    b3--b4
b2--b5    b3--b6
b4--b7    b5--b7    b6--b7
```

启动拓扑：

```bash
sudo mn -c
sudo python3 seven_node_topo.py
```

在Mininet中启动7个STP进程，等待35秒后输出状态：

```text
mininet> b1 ./stp > b1-output.txt 2>&1 &
mininet> b2 ./stp > b2-output.txt 2>&1 &
mininet> b3 ./stp > b3-output.txt 2>&1 &
mininet> b4 ./stp > b4-output.txt 2>&1 &
mininet> b5 ./stp > b5-output.txt 2>&1 &
mininet> b6 ./stp > b6-output.txt 2>&1 &
mininet> b7 ./stp > b7-output.txt 2>&1 &
mininet> sh sleep 35
mininet> sh pkill -SIGTERM stp
mininet> sh sleep 2
mininet> exit
```

```bash
./dump_output.sh 7
```

> 截图占位：七节点拓扑最终状态，画面中应包含b4和b7的ALTERNATE端口，建议保存为`evidence/04-seven-node-output.png`。

### 5.5 OJ测试

将`stp/`中的代码直接压缩为zip文件，保证解压后根目录中能够看到`Makefile`，并使用原始Makefile完成编译。

> 截图占位：OJ全部测试通过，建议保存为`evidence/05-oj.png`。

## 6 实验结果

### 6.1 四节点环形拓扑

四节点拓扑收敛后的结果如下：

| 节点 | 根路径开销 | 端口1 | 端口2 |
| --- | ---: | --- | --- |
| b1 | 0 | DESIGNATED | DESIGNATED |
| b2 | 1 | ROOT | DESIGNATED |
| b3 | 1 | ROOT | DESIGNATED |
| b4 | 2 | ROOT | ALTERNATE |

b1的节点ID最小，因此成为根节点。b2和b3通过各自连接b1的端口到达根节点。b4通过b2和b3到达根节点的路径开销相同，继续比较上游节点ID后选择b2，因此连接b2的端口为根端口，连接b3的端口为备用端口。阻塞该备用端口后，原来的环形拓扑变为无环连通拓扑。

### 6.2 七节点拓扑

七节点拓扑收敛后的主要结果如下：

| 节点 | 根路径开销 | 端口角色 |
| --- | ---: | --- |
| b1 | 0 | DESIGNATED、DESIGNATED |
| b2 | 1 | ROOT、DESIGNATED、DESIGNATED |
| b3 | 1 | ROOT、DESIGNATED、DESIGNATED |
| b4 | 2 | ROOT、ALTERNATE、DESIGNATED |
| b5 | 2 | ROOT、DESIGNATED |
| b6 | 2 | ROOT、DESIGNATED |
| b7 | 3 | ROOT、ALTERNATE、ALTERNATE |

最终由b1担任根节点。b4阻塞连接b3的冗余端口，b7阻塞连接b5和b6的两个冗余端口，共产生3个ALTERNATE端口。其余6条有效链路连接全部7个节点，链路数量为节点数减1，形成了一棵无环生成树。

## 7 实验结果分析与调试过程

### 7.1 网络字节序

最初需要特别区分报文中的Config字段和端口结构中保存的Config字段。报文中的`root_id`、`root_path_cost`、`switch_id`和`port_id`均为网络字节序，而端口结构中的字段为主机字节序。如果直接比较，根节点选举会受到字节排列影响。处理报文时统一使用`ntohll`、`ntohl`和`ntohs`转换后，比较结果才与节点ID大小一致。

### 7.2 路径开销的使用位置

收到Config报文并与当前端口Config比较时，应直接比较报文中的`root_path_cost`和端口保存的`designated_cost`，不能提前加入本端口开销。只有在多个端口之间选择根端口、计算本节点到根节点的路径开销时，才加入`path_cost`。如果在接收阶段提前加1，会混淆链路对端通告的开销和本节点实际到根节点的开销。

### 7.3 端口状态联动更新

端口接收到更优Config后，不能只修改该端口。根端口变化会改变本节点发送的Config，因此还要重新检查其他端口是否应成为指定端口，并从所有指定端口立即传播新结果。四节点测试中，b4两条路径开销相同，最终依靠上游节点ID选出连接b2的根端口，说明比较顺序和端口联动更新均正确。

### 7.4 输出与OJ要求

调试阶段主要使用程序在`SIGTERM`时输出的状态检查收敛结果。正式代码删除了未实现提示，不在正常运行过程中打印自定义调试信息，避免大量输出影响OJ评测。四节点和七节点测试均使用原始Makefile编译，并在等待生成树稳定后统一输出结果。

## 8 实验结论

本实验实现了简化的生成树协议。程序能够比较Config优先级、选举唯一根节点、选择非根节点的根端口，并确定各链路上的指定端口和备用端口。四节点环形拓扑最终阻塞1个冗余端口，七节点拓扑最终阻塞3个冗余端口，两种拓扑都在保持全部节点连通的同时消除了二层环路。通过实现和调试，我进一步理解了分布式节点如何只依靠邻居交换的Config信息逐步收敛到一致的生成树。
