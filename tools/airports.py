#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Every paved runway an airport with a code has, from OurAirports
(`docs/flight.md` F8): its `airports.csv` and `runways.csv`, public
domain, fetched at a pinned commit into build/downloads and checked by
their sums, made small into one file Flight carries.

    python3 tools/airports.py OUT

One line an airport, its fields apart by tabs:

    ident  iata  name  municipality  country  size  lat  lon  feet  runways

`ident` is the code a pilot says - ICAO's where it has one ("SBRJ"), or
the local one ("00A"); `iata` is the traveller's ("SDU"), or empty;
`size` is OurAirports' type, `l`arge, `m`edium or `s`mall. Its
runways, apart by `|`, each nine fields apart by `;`:

    le_ident he_ident length_ft width_ft
    le_lat le_lon le_feet he_lat he_lon he_feet

the threshold at either end, each with its elevation (the airport's when
the runway has none). Kept: small, medium and large airports, open
runways at least 1,500 feet long whose surface is asphalt, concrete,
bitumen or pierced steel, with both ends' places known - which is what
an aircraft can line up on and a heading be reckoned from. The file is
sorted by `ident`, so a reader can bisect it.
"""
import csv
import hashlib
import os
import sys
import urllib.request

COMMIT = "e850302d9e66c2209edaa94a3b36bf8baf4312a1"      # 21 November 2026
SUMS = {
    "airports.csv": "eeb71bb033c440c9f542837f374ec460abdca649fb473f6c61dc9814ddf32ddf",
    "runways.csv": "9cb3b0b1f6793636626f6c3e293db14412bdc1667eec852df253bd71eb718893",
}
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOWNLOADS = os.path.join(HERE, "build", "downloads", "ourairports-" + COMMIT[:8])
URL = "https://raw.githubusercontent.com/davidmegginson/ourairports-data/%s/%s"

PAVED = ("ASP", "CON", "PEM", "BIT", "PAVE", "TAR", "BET", "MAC", "PSP", "PER")
TYPES = {"small_airport", "medium_airport", "large_airport"}


def fetched(name):
    """The file at the pinned commit, fetched once and held to its sum."""
    os.makedirs(DOWNLOADS, exist_ok=True)
    path = os.path.join(DOWNLOADS, name)
    if not os.path.exists(path):
        with urllib.request.urlopen(URL % (COMMIT, name), timeout=120) as r, \
                open(path + ".part", "wb") as f:
            f.write(r.read())
        os.replace(path + ".part", path)
    with open(path, "rb") as f:
        got = hashlib.sha256(f.read()).hexdigest()
    if got != SUMS[name]:
        sys.exit("airports: %s is %s, not the pinned %s" % (path, got, SUMS[name]))
    return path


def clean(text):
    return " ".join(text.replace("\t", " ").replace("|", "/").replace(";", ",").split())


def number(text):
    try:
        return float(text)
    except ValueError:
        return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    airports = {}
    with open(fetched("airports.csv"), encoding="utf-8", newline="") as f:
        for a in csv.DictReader(f):
            if a["type"] not in TYPES:
                continue
            ident = a["icao_code"] or a["gps_code"] or a["ident"]
            airports[a["id"]] = {
                "ident": clean(ident).upper(), "iata": clean(a["iata_code"]).upper(),
                "name": clean(a["name"]), "town": clean(a["municipality"]),
                "country": a["iso_country"], "size": a["type"][0],
                "lat": a["latitude_deg"],
                "lon": a["longitude_deg"], "feet": a["elevation_ft"] or "0", "runways": [],
            }
    with open(fetched("runways.csv"), encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            a = airports.get(r["airport_ref"])
            surface = r["surface"].upper()
            length, width = number(r["length_ft"]), number(r["width_ft"])
            ends = [number(r[k]) for k in ("le_latitude_deg", "le_longitude_deg",
                                           "he_latitude_deg", "he_longitude_deg")]
            if a is None or r["closed"] == "1" or not surface.startswith(PAVED) or \
                    length is None or length < 1500 or None in ends:
                continue
            feet = a["feet"]
            a["runways"].append(";".join([
                clean(r["le_ident"]), clean(r["he_ident"]), "%d" % length,
                "%d" % (width or 75), r["le_latitude_deg"], r["le_longitude_deg"],
                r["le_elevation_ft"] or feet, r["he_latitude_deg"], r["he_longitude_deg"],
                r["he_elevation_ft"] or feet]))
    lines = []
    seen = set()
    for a in airports.values():
        if not a["runways"] or a["ident"] in seen:
            continue
        seen.add(a["ident"])
        lines.append("\t".join([a["ident"], a["iata"], a["name"], a["town"], a["country"],
                                a["size"], a["lat"], a["lon"], a["feet"],
                                "|".join(a["runways"])]))
    lines.sort()
    with open(sys.argv[1] + ".part", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(sys.argv[1] + ".part", sys.argv[1])
    runways = sum(line.count("|") + 1 for line in lines)
    print("airports: %d airports, %d runways, %d KB in %s"
          % (len(lines), runways, os.path.getsize(sys.argv[1]) // 1024, sys.argv[1]))


if __name__ == "__main__":
    main()
