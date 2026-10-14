# 数据包队列管理实验运行说明

本目录包含完整的本地实验代码。下发原始文件保存在上一级的`05-bufferbloat/`，本目录是可运行的完成版本。

## 1 环境

在Ubuntu/WSL内运行，需要管理员权限、Mininet、iperf2、iproute2、ethtool和matplotlib。使用系统Python，以便导入系统安装的Mininet。

```bash
sudo apt update
sudo apt install mininet iperf iproute2 iputils-ping ethtool python3-matplotlib
```

## 2 一次运行全部实验

进入本目录后执行：

```bash
sudo python3 run_experiments.py --duration 60 --results results/my-run
```

程序顺序执行队列上限20、队列上限100、Tail Drop、RED和CoDel五组实验，预计约6分钟，最后生成曲线和统计表。使用新的结果目录可保留旧数据；程序不会覆盖已经存在的实验结果。

```bash
python3 analyze_results.py --results results/my-run
cat results/my-run/summary.csv
```

`results/formal/`是已完成的60秒本地实测结果；`results/smoke-*`是6秒功能验证，不能作为正式对照数据。

## 3 单独运行

```bash
sudo python3 reproduce_bufferbloat.py --maxq 20 --duration 60 --output results/new-q20
sudo python3 reproduce_bufferbloat.py --maxq 100 --duration 60 --output results/new-q100
sudo python3 mitigate_bufferbloat.py --algo taildrop --duration 60 --output results/new-taildrop
sudo python3 mitigate_bufferbloat.py --algo red --duration 60 --output results/new-red
sudo python3 mitigate_bufferbloat.py --algo codel --duration 60 --output results/new-codel
```

单组数据应放在同一结果目录的不同子目录中，再将共同的父目录传给`analyze_results.py`。重复运行时使用新的目录名。

## 4 结果文件

| 文件 | 内容 |
| --- | --- |
| `metadata.json` | 实验类型、持续时间、共同时间原点、内核版本、算法 |
| `cwnd.txt` | 复现实验中的发送方TCP拥塞窗口，单位为TCP段 |
| `qlen.txt` | 复现实验中r1-eth1叶子队列的积压包数 |
| `rtt.txt` | h1到h2的ping结果，RTT单位为ms |
| `iperf.txt` | 客户端每0.5秒发送速率与汇总 |
| `iperf_server.txt` | 接收端实际接收吞吐率汇总 |
| `qdisc_before.txt`、`qdisc_after.txt` | 实验前后的队列类型、参数、发送与丢包统计 |
| `bandwidth.csv` | 缓解实验中的六阶段带宽变更时间 |
| `summary.csv` | 接收吞吐率、RTT均值/P95/最大值、队列统计和采样数量 |
| `stages.csv` | 动态带宽各阶段的RTT统计 |
| `reproduce.png` | cwnd、队列长度、RTT和发送速率对比图 |
| `mitigate.png` | 三种队列算法的RTT对比及带宽时间线，RTT纵轴为对数坐标 |

采样间隔是请求间隔，执行`ss`和`tc`还需要时间，因此实际采样周期不严格等于10ms。RTT统计只包含成功收到回复的ping样本，不代表丢包率。

## 5 实验与提交

本实验下发课件明确要求仅在SEP提交报告，不在OJ提交代码。上一级`bufferbloat-code.zip`是源码附件，解压后直接得到Python文件和本说明，不包含实验数据、截图、报告或缓存，也不需要Makefile。

报告中需要的证据可以这样获得：

```bash
uname -r
python3 --version
iperf --version
cat results/formal/red/qdisc_before.txt
cat results/formal/codel/qdisc_before.txt
cat results/formal/summary.csv
```

曲线直接使用本目录`results/formal/reproduce.png`与`results/formal/mitigate.png`；如重新测试，应同时替换报告中的曲线和数据表。

HTB的`quantum ... is big`是Mininet配置提示。本程序会检查实际带宽类和队列算法配置，若出现缺少qdisc、配置失败或空数据文件，必须处理后再统计，不能仅凭该提示判断成功或失败。
