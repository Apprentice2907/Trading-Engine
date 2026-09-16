#!/usr/bin/env python3
"""Generate adversarial seed corpus for fuzz_decoder (YahooParser)."""

import pathlib

OUT = pathlib.Path(__file__).parent

def write(name, data):
    if isinstance(data, str):
        data = data.encode('utf-8')
    (OUT / name).write_bytes(data)

# 1. Valid US Quote
write("valid_us.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "currency": "USD",
        "symbol": "AAPL",
        "exchangeName": "NMS",
        "instrumentType": "EQUITY",
        "regularMarketTime": 1710000000,
        "regularMarketPrice": 185.50,
        "regularMarketVolume": 45000000
      }
    }],
    "error": null
  }
}""")

# 2. Valid Indian Quote
write("valid_in.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "currency": "INR",
        "symbol": "RELIANCE.NS",
        "exchangeName": "NSE",
        "regularMarketTime": 1710001000,
        "regularMarketPrice": 2985.75,
        "regularMarketVolume": 8500000
      }
    }],
    "error": null
  }
}""")

# 3. Valid ETF
write("valid_etf.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "currency": "USD",
        "symbol": "SPY",
        "exchangeName": "PCX",
        "regularMarketTime": 1710002000,
        "regularMarketPrice": 510.25,
        "regularMarketVolume": 70000000
      }
    }],
    "error": null
  }
}""")

# 4. Error response
write("error_not_found.bin", """{
  "chart": {
    "result": null,
    "error": {
      "code": "Not Found",
      "description": "No data found for symbol"
    }
  }
}""")

# 5. Missing price
write("missing_price.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": "AAPL",
        "regularMarketVolume": 10000
      }
    }]
  }
}""")

# 6. Missing symbol
write("missing_symbol.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "regularMarketPrice": 150.00,
        "regularMarketVolume": 10000
      }
    }]
  }
}""")

# 7. Truncated JSON
write("truncated.bin", """{"chart": {"result": [{"meta": {"symbol": "AAPL", "regularMarketPrice": 18""")

# 8. Empty input
write("empty.bin", b"")

# 9. Single byte
write("one_byte.bin", b"{")

# 10. Minimal object
write("two_bytes.bin", b"{}")

# 11. Negative price
write("negative_price.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": "AAPL",
        "regularMarketPrice": -150.25
      }
    }]
  }
}""")

# 12. Zero price
write("zero_price.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": "AAPL",
        "regularMarketPrice": 0.0
      }
    }]
  }
}""")

# 13. Deeply nested
write("deep_nested.bin", "[" * 50 + '{"chart":{"result":[{"meta":{"symbol":"AAPL","regularMarketPrice":100}}]}}' + "]" * 50)

# 14. Non-numeric price
write("invalid_price.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": "AAPL",
        "regularMarketPrice": "abc"
      }
    }]
  }
}""")

# 15. Binary garbage
write("binary_garbage.bin", bytes(range(256)))

# 16. Very long symbol
write("long_symbol.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": \"""" + "A" * 200 + """",
        "regularMarketPrice": 100.0
      }
    }]
  }
}""")

# 17. Null meta
write("null_meta.bin", """{"chart": {"result": [{"meta": null}]}}""")

# 18. Large volume
write("large_volume.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": "AAPL",
        "regularMarketPrice": 100.0,
        "regularMarketVolume": 999999999999999
      }
    }]
  }
}""")

# 19. All whitespace
write("whitespace.bin", "   \t\r\n   ")

# 20. Escape sequences
write("escapes.bin", """{
  "chart": {
    "result": [{
      "meta": {
        "symbol": "A\\\"A\\nP\\tL",
        "regularMarketPrice": 100.5
      }
    }]
  }
}""")

print(f"Generated {len(list(OUT.glob('*.bin')))} seed files in {OUT}")
