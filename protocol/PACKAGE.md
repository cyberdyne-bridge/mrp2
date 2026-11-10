# MRP2 渲染協議 — 打包版

- 打包日期：2026-11-10
- 內容：協議實作（Python＋C）、權威文件、測試、字模與範例
- 授權：自用（PoC）

## 快速開始
```sh
python3 test_proto.py && python3 test_compositor.py && python3 test_evdev.py
gcc -O2 -o /var/tmp/mrpd csrc/mrpd.c
/var/tmp/mrpd assets/text.wire out.ppm 1 1920 1080 --font assets/font.rom
python3 gpu/ppm2png.py out.ppm out.png
```

依賴：Python 3.12＋（stdlib）；可選 freetype-py（烘字模）、yt-dlp（B 站）、
ffmpeg（真影片）；C 端只需 libc（KMS 需 libdrm）。

## 檔案清單（49 個）

| 檔案 | bytes | sha256(前16) |
|---|---|---|
| `HANDOVER.md` | 9387 | `3f5c2ab43971ec17` |
| `PROTOCOL.md` | 8957 | `4c480a01614ce02b` |
| `README.md` | 19946 | `bab638efe0beeb5a` |
| `assets/doc.json` | 6539 | `21b8ac50b28024dd` |
| `assets/font.rom` | 31174 | `e2b1be9f5a0196b8` |
| `assets/plane.wire` | 71 | `4d8b8842d6a2ef06` |
| `assets/text.wire` | 2335 | `575a948e3c5651a4` |
| `bench/RESULTS.md` | 1367 | `f3b05773d86db101` |
| `bench/compositor_sim.py` | 2809 | `e19099b0a374c7eb` |
| `bench/plan.c` | 4038 | `03eb7d6047934e22` |
| `bench/plan_bench.py` | 1483 | `225f11dd2b877c23` |
| `bench/raster.c` | 1918 | `b72991c662227be2` |
| `bench/raster.js` | 1388 | `88b0d91fa9cd461e` |
| `bench/raster.py` | 1275 | `9a910f8d4d11fabb` |
| `bili.py` | 2326 | `beddf3a46924869e` |
| `bili_test.html` | 539 | `4519648e2b3f1693` |
| `compositor.py` | 14488 | `c95f677df054300e` |
| `csrc/mrpd.c` | 20744 | `8e3c13fbd29b3c79` |
| `csrc/shmvid.c` | 3159 | `80c7bd64d3a60225` |
| `demo_proto.py` | 4035 | `6db49ecea250c88d` |
| `demo_shm.py` | 1751 | `b30f9244f7853136` |
| `demo_video.py` | 2673 | `b27fa0cba60a99b4` |
| `demo_video_real.py` | 2462 | `fe0682076d54582d` |
| `display.py` | 1987 | `6f0b7fc9edc77211` |
| `evdev.py` | 5922 | `e4d49642b215d90a` |
| `frontend.py` | 1568 | `8524d207c27ef92c` |
| `glyphfont.py` | 4068 | `f81fc657e9a8c0ca` |
| `gpu/glclear.c` | 4532 | `7c3143f9db1bb94c` |
| `gpu/glquad.c` | 8144 | `1a27461f804ba59b` |
| `gpu/ppm2png.py` | 1662 | `d84a5f6a9b6d1f52` |
| `kms/README.md` | 1971 | `91fe0feaaf8be94c` |
| `kms/kms.c` | 6208 | `de78c38d017254e9` |
| `minified.html` | 500 | `3feccd30e13d2b81` |
| `minireader.py` | 20889 | `eaae4e1d7365d252` |
| `mkfont.py` | 3036 | `babfce6c268963b1` |
| `proto.py` | 7691 | `7c89c5c6ac9421f6` |
| `reader.py` | 5745 | `bcb99e069c22bfa3` |
| `render.py` | 8297 | `59c67b31714dc415` |
| `resolver.py` | 7140 | `d979fbe8d3c1b111` |
| `sample.html` | 2092 | `ac1b2e8a945b1fea` |
| `server.py` | 6712 | `dd67420e2cab3fc9` |
| `shell.py` | 4268 | `a4788596ba5f6838` |
| `test_bili.py` | 1563 | `3657c53d96ffc13d` |
| `test_compositor.py` | 5478 | `783ab4dac2a98575` |
| `test_evdev.py` | 3846 | `570a4b66b8d3c30f` |
| `test_proto.py` | 4212 | `2e41101d7be24e02` |
| `test_resolver.py` | 4167 | `d7fde62211e65b42` |
| `torture.html` | 994 | `0c7ad98bea9a0387` |
| `tui.py` | 8119 | `9c48c5a4528c3bf6` |
