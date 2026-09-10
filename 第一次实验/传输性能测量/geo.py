import math
import pandas as pd
import geoip2.database

INPUT_FILE = "results.csv"
OUTPUT_FILE = "results_geo.csv"
DB_FILE = "dbip-city-lite-2026-09.mmdb"

# 修改成你实验所在地的大致经纬度
CLIENT_LAT = 40.3160
CLIENT_LON = 116.6318


def haversine(lat1, lon1, lat2, lon2):
    R = 6371.0

    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)

    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)

    a = (
        math.sin(dphi / 2) ** 2
        + math.cos(phi1)
        * math.cos(phi2)
        * math.sin(dlambda / 2) ** 2
    )

    c = 2 * math.atan2(
        math.sqrt(a),
        math.sqrt(1 - a)
    )

    return R * c


def c_latency_rtt_ms(distance_km):
    # 光纤速度约 0.6C
    speed_km_s = 0.6 * 300000

    # RTT: 往返两倍距离
    return (
        2 * distance_km / speed_km_s
    ) * 1000


df = pd.read_csv(INPUT_FILE)

reader = geoip2.database.Reader(DB_FILE)

server_latitudes = []
server_longitudes = []
countries = []
cities = []
distances = []
c_latencies = []

for ip in df["ip"]:

    try:
        response = reader.city(ip)

        lat = response.location.latitude
        lon = response.location.longitude

        country = response.country.name
        city = response.city.name

        if lat is None or lon is None:
            raise ValueError()

        distance = haversine(
            CLIENT_LAT,
            CLIENT_LON,
            lat,
            lon
        )

        c_latency = c_latency_rtt_ms(distance)

    except Exception:
        lat = None
        lon = None
        country = None
        city = None
        distance = None
        c_latency = None

    server_latitudes.append(lat)
    server_longitudes.append(lon)
    countries.append(country)
    cities.append(city)
    distances.append(distance)
    c_latencies.append(c_latency)

reader.close()

df["server_lat"] = server_latitudes
df["server_lon"] = server_longitudes
df["country"] = countries
df["city"] = cities
df["distance_km"] = distances
df["c_latency_ms"] = c_latencies

df.to_csv(
    OUTPUT_FILE,
    index=False
)

print(f"Saved to {OUTPUT_FILE}")