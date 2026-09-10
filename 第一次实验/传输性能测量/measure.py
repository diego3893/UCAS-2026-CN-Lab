import csv
import re
import socket
import subprocess
import time

INPUT_FILE = "sites.txt"
OUTPUT_FILE = "results.csv"

PING_COUNT = 3
TIMEOUT = 10


def resolve_ip(domain):
    try:
        return socket.gethostbyname(domain)
    except Exception:
        return ""


def measure_ping(domain):
    try:
        result = subprocess.run(
            ["ping", "-c", str(PING_COUNT), "-W", "2", domain],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=10,
        )

        m = re.search(
            r"rtt min/avg/max/mdev = [\d.]+/([\d.]+)/",
            result.stdout
        )

        if m:
            return float(m.group(1))

    except Exception:
        pass

    return None


def measure_curl(domain):
    fmt = (
        "%{time_namelookup},"
        "%{time_connect},"
        "%{time_total}"
    )

    urls = [
        f"https://{domain}",
        f"http://{domain}",
    ]

    for url in urls:
        try:
            result = subprocess.run(
                [
                    "curl",
                    "--noproxy", "*",
                    "-L",
                    "-o", "/dev/null",
                    "-s",
                    "--connect-timeout", str(TIMEOUT),
                    "--max-time", "20",
                    "-w", fmt,
                    url,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=25,
            )

            if result.returncode != 0:
                continue

            values = result.stdout.strip().split(",")

            if len(values) != 3:
                continue

            dns = float(values[0])
            connect = float(values[1])
            total = float(values[2])

            if total <= 0:
                continue

            dns_ms = dns * 1000
            tcp_handshake_ms = max(0, (connect - dns) * 1000)
            tcp_transfer_ms = max(0, (total - connect) * 1000)
            total_ms = total * 1000

            return (
                dns_ms,
                tcp_handshake_ms,
                tcp_transfer_ms,
                total_ms,
                url,
            )

        except Exception:
            continue

    return None


def main():
    with open(INPUT_FILE, "r", encoding="utf-8") as f:
        domains = [
            line.strip()
            for line in f
            if line.strip()
        ]

    with open(
        OUTPUT_FILE,
        "w",
        newline="",
        encoding="utf-8"
    ) as f:

        writer = csv.writer(f)

        writer.writerow([
            "rank",
            "domain",
            "ip",
            "ping_ms",
            "dns_ms",
            "tcp_handshake_ms",
            "tcp_transfer_ms",
            "total_ms",
            "url",
        ])

        for rank, domain in enumerate(domains, 1):
            print(f"[{rank}/{len(domains)}] {domain}")

            ip = resolve_ip(domain)
            ping_ms = measure_ping(domain)
            curl_result = measure_curl(domain)

            if curl_result:
                (
                    dns_ms,
                    handshake_ms,
                    transfer_ms,
                    total_ms,
                    url,
                ) = curl_result
            else:
                dns_ms = None
                handshake_ms = None
                transfer_ms = None
                total_ms = None
                url = ""

            writer.writerow([
                rank,
                domain,
                ip,
                ping_ms,
                dns_ms,
                handshake_ms,
                transfer_ms,
                total_ms,
                url,
            ])

            f.flush()

            time.sleep(0.1)


if __name__ == "__main__":
    main()
