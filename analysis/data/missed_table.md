clang: 108 kernel loops vectorized, 44 kernel loops not vectorized (outer `nl` timing loops excluded)

### unsafe dependent memory operations in loop — Backward loop carried data dependence (25)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s116 | 274 | no | **yes** |
| s126 | 513 | no | no |
| s161 | 723 | no | no |
| s211 | 962 | no | **yes** |
| s212 | 985 | no | **yes** |
| s1213 | 1006 | no | **yes** |
| s221 | 1029 | no | no |
| s222 | 1071 | no | no |
| s231 | 1095 | no | no |
| s232 | 1119 | no | no |
| s233 | 1165 | no | no |
| s2233 | 1190 | no | no |
| s235 | 1217 | no | no |
| s241 | 1240 | no | **yes** |
| s242 | 1267 | no | no |
| s244 | 1313 | no | **yes** |
| s1244 | 1335 | no | **yes** |
| s256 | 1577 | no | no |
| s261 | 1653 | no | **yes** |
| s275 | 1782 | no | no |
| s277 | 1854 | no | no |
| s2111 | 2234 | no | no |
| s321 | 2687 | no | no |
| s322 | 2709 | no | no |
| s323 | 2731 | no | no |

### value that could not be identified as reduction is used outside the loop (11)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s2251 | 1425 | no | no |
| s258 | 1626 | no | no |
| s292 | 2140 | no | no |
| s315 | 2401 | **yes** | no |
| s318 | 2487 | no | no |
| s3110 | 2550 | no | no |
| s13110 | 2582 | no | no |
| s3112 | 2638 | no | no |
| s341 | 2820 | no | no |
| s342 | 2848 | no | no |
| s343 | 2877 | no | no |

### unsafe dependent memory operations in loop — Unknown data dependence (3)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s1113 | 182 | no | no |
| s114 | 206 | no | no |
| s281 | 2063 | no | no |

### unsafe dependent memory operations in loop — Unsafe indirect dependence (2)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s141 | 643 | no | no |
| s1161 | 752 | **yes** | no |

### Cannot vectorize uncountable loop (1)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s482 | 3395 | no | no |

### Early exit loop with store but no supported condition load (1)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s481 | 3369 | no | no |

### cannot identify array bounds (1)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s123 | 428 | no | no |

icx vectorizes 2 of the 44 loops clang misses: s1161, s315

clang + vecpass vectorizes 8 of the 44 loops clang misses: s116, s211, s212, s1213, s241, s244, s1244, s261
