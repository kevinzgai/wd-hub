# Offline city directory

Source: [public-wheels/china-cities](https://github.com/public-wheels/china-cities)
at commit `91375815c8a1a5b834705be249bd1aa5f645053a`, retrieved 2026-10-08.
The upstream MIT license is preserved as `LICENSE`.

This is a weather-location directory, not a current administrative-division
register. Some county names reflect the source snapshot. The phone page offers
custom names and decimal coordinates for places absent from the snapshot or
renamed since it. It does not claim street-level positioning accuracy.

`legacy_centres.json` preserves the 50 urban-centre coordinates used in rc.11,
including Zhuhai and Chongqing. `tools/generate_city_catalog.py` rounds other
coordinates to four decimals, merges the old 开县 into 开州, and uses 济南 as
the radio parent for 莱芜. Hong Kong, Macau and Taiwan retain their catalog
locations and their Radio Browser country codes.

Regenerate with `python3 tools/generate_city_catalog.py`; use `--check` to verify
the checked-in JSON, deterministic gzip, city glyph list and radio translations.
The device serves the gzip bytes from flash. Only the phone decompresses it.
