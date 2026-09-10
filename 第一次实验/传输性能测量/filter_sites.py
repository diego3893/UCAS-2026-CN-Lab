import csv
import socket
import subprocess
import time

INPUT_FILE = "tranco_Q2Q44.csv"
OUTPUT_FILE = "sites.txt"

TARGET_COUNT = 1000

# 每个网站最多允许测试多少秒
SITE_TIMEOUT = 8

# 单次 TCP 连接超时
CONNECT_TIMEOUT = 3


def can_resolve(domain):
    """检查域名是否能解析出 IPv4"""
    try:
        socket.setdefaulttimeout(SITE_TIMEOUT)
        socket.gethostbyname(domain)
        return True
    except Exception:
        return False


def can_access(domain, remaining_time):
    """
    尝试 HTTPS，失败后再尝试 HTTP。
    remaining_time 表示该网站剩余可用测试时间。
    """
    if remaining_time <= 0:
        return False

    schemes = ["https", "http"]

    for scheme in schemes:
        if remaining_time <= 0:
            return False

        url = f"{scheme}://{domain}"

        # curl 自己的 max-time 不超过剩余时间
        max_time = max(1, int(remaining_time))

        try:
            result = subprocess.run(
                [
                    "curl",
                    "--noproxy", "*",
                    "-L",
                    "-o", "/dev/null",
                    "-s",
                    "--connect-timeout", str(CONNECT_TIMEOUT),
                    "--max-time", str(max_time),
                    url,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=remaining_time + 1,
            )

            if result.returncode == 0:
                return True

        except subprocess.TimeoutExpired:
            return False
        except Exception:
            pass

        return False

    return False


def valid_site(domain):
    """
    每个网站总测试时间不超过 SITE_TIMEOUT。
    """
    start = time.monotonic()

    if not can_resolve(domain):
        return False

    elapsed = time.monotonic() - start
    remaining = SITE_TIMEOUT - elapsed

    if remaining <= 0:
        return False

    return can_access(domain, remaining)


def main():
    count = 0
    checked = 0

    with open(INPUT_FILE, "r", encoding="utf-8-sig", newline="") as f, \
         open(OUTPUT_FILE, "w", encoding="utf-8") as out:

        reader = csv.reader(f)

        for row in reader:
            if len(row) < 2:
                continue

            domain = row[1].strip()

            if not domain:
                continue

            checked += 1

            print(
                f"[checked={checked}, valid={count}/{TARGET_COUNT}] "
                f"testing {domain} ..."
            )

            start = time.monotonic()
            ok = valid_site(domain)
            elapsed = time.monotonic() - start

            if ok:
                count += 1
                out.write(domain + "\n")
                out.flush()

                print(
                    f"  OK ({elapsed:.2f}s) "
                    f"-> saved as #{count}"
                )
            else:
                print(
                    f"  FAIL ({elapsed:.2f}s)"
                )

            if count >= TARGET_COUNT:
                break

    print()
    print(f"Checked sites: {checked}")
    print(f"Valid sites:   {count}")
    print(f"Output:        {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
