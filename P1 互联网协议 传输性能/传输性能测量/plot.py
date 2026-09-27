import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

INPUT_FILE = "results_geo.csv"
OUTPUT_FILE = "cdf_inflation.png"

# 是否显示 PPT 里的 Router-path
# 你的实验数据里没有 router-path，因此默认不画
DRAW_ROUTER_PATH = False

# 如果你后续真的有 router_path_ms 这一列，可以设为 True


def prepare_ratio(df, metric, c_col="c_latency_ms"):
    """
    计算 metric / c_latency_ms，并清理无效值。
    """
    data = df[[metric, c_col]].dropna().copy()

    # 去除 <= 0，避免对数坐标出错
    data = data[
        (data[metric] > 0) &
        (data[c_col] > 0)
    ]

    ratio = data[metric] / data[c_col]

    # 去除无穷、NaN
    ratio = ratio.replace([np.inf, -np.inf], np.nan).dropna()

    return ratio.values


def ecdf(values):
    """
    计算经验累计分布函数 ECDF。
    """
    x = np.sort(values)
    y = np.arange(1, len(x) + 1) / len(x)
    return x, y


def add_curve(ax, values, label, linewidth=2.5):
    """
    添加一条 CDF 曲线。
    """
    if len(values) == 0:
        print(f"Warning: no valid data for {label}")
        return None

    x, y = ecdf(values)

    line, = ax.plot(
        x,
        y,
        linewidth=linewidth,
        label=label
    )

    return line


def main():
    df = pd.read_csv(INPUT_FILE)

    required = [
        "ping_ms",
        "dns_ms",
        "tcp_handshake_ms",
        "tcp_transfer_ms",
        "total_ms",
        "c_latency_ms",
    ]

    missing = [c for c in required if c not in df.columns]

    if missing:
        raise ValueError(
            f"Missing columns in {INPUT_FILE}: {missing}"
        )

    ping_ratio = prepare_ratio(df, "ping_ms")
    dns_ratio = prepare_ratio(df, "dns_ms")
    handshake_ratio = prepare_ratio(df, "tcp_handshake_ms")
    transfer_ratio = prepare_ratio(df, "tcp_transfer_ms")
    total_ratio = prepare_ratio(df, "total_ms")

    print("Valid samples:")
    print("Min Ping:", len(ping_ratio))
    print("DNS:", len(dns_ratio))
    print("TCP handshake:", len(handshake_ratio))
    print("TCP transfer:", len(transfer_ratio))
    print("Total time:", len(total_ratio))

    # 可选：如果以后有 router_path_ms
    router_ratio = None

    if DRAW_ROUTER_PATH:
        if "router_path_ms" in df.columns:
            router_ratio = prepare_ratio(df, "router_path_ms")
        else:
            print(
                "Warning: DRAW_ROUTER_PATH=True, "
                "but router_path_ms not found."
            )

    fig, ax = plt.subplots(figsize=(9, 6))

    lines = []

    if router_ratio is not None:
        line = add_curve(
            ax,
            router_ratio,
            "Router-path"
        )
        if line:
            lines.append(line)

    for values, label in [
        (ping_ratio, "Ping"),
        (handshake_ratio, "TCP handshake"),
        (dns_ratio, "DNS"),
        (transfer_ratio, "TCP transfer"),
        (total_ratio, "Total time"),
    ]:
        line = add_curve(
            ax,
            values,
            label
        )

        if line:
            lines.append(line)

    # PPT 横轴是 logarithmic
    ax.set_xscale("log")

    # 尽量贴近 PPT 范围
    ax.set_xlim(0.1, 1000)
    ax.set_ylim(0, 1.02)

    ax.set_xlabel(
        "Inflation over c-latency",
        fontsize=16
    )

    ax.set_ylabel(
        "CDF",
        fontsize=16,
        rotation=0,
        labelpad=30
    )

    # 网格
    ax.grid(
        True,
        which="major",
        linestyle=":",
        linewidth=1,
        alpha=0.6
    )

    # y 轴
    ax.set_yticks(
        np.arange(0, 1.01, 0.2)
    )

    # x 轴主要刻度
    ax.set_xticks(
        [1, 10, 100, 1000]
    )

    ax.get_xaxis().set_major_formatter(
        plt.ScalarFormatter()
    )

    ax.tick_params(
        axis="both",
        labelsize=12
    )

    # 顶部 legend，接近 PPT
    ax.legend(
        loc="lower left",
        bbox_to_anchor=(0.0, 1.02),
        ncol=3,
        frameon=False,
        fontsize=12
    )

    # 计算 Total time 的中位 inflation
    if len(total_ratio) > 0:
        median_total = np.median(total_ratio)

        # 找 total CDF 上接近 y=0.5 的位置
        x_total, y_total = ecdf(total_ratio)

        idx = np.argmin(
            np.abs(y_total - 0.5)
        )

        x50 = x_total[idx]
        y50 = y_total[idx]

        ax.scatter(
            [x50],
            [y50],
            s=60,
            zorder=5
        )

        ax.text(
            x50 * 1.25,
            y50,
            f"{median_total:.1f}x",
            fontsize=16,
            fontweight="bold",
            va="center"
        )

    plt.tight_layout()

    plt.savefig(
        OUTPUT_FILE,
        dpi=300,
        bbox_inches="tight"
    )

    print(f"Saved figure to {OUTPUT_FILE}")

    plt.show()


if __name__ == "__main__":
    main()