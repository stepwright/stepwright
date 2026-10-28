# Regenerating `docs/assets/banner.png`

`banner.html` is a self-contained 1280×640 page (the proportions GitHub
recommends for a repository's social-preview image), rendered at twice that
resolution, 2560×1280, so that it stays sharp on high-density screens. The same
PNG is the README banner. It uses three fonts under the SIL Open Font License 1.1, loaded
from Google Fonts: [Space Grotesk](https://fonts.google.com/specimen/Space+Grotesk)
(the name), [Inter](https://fonts.google.com/specimen/Inter) (text) and
[JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (code). The
logo is [`../assets/logo.svg`](../assets/logo.svg), drawn inline.

Render it with Chrome or Chromium. `--virtual-time-budget` gives the web fonts
time to load before the screenshot:

```bash
cd docs/banner
python3 -m http.server 8934 &
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --headless --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
  --window-size=1280,640 --virtual-time-budget=8000 \
  --screenshot=../assets/banner.png \
  http://localhost:8934/banner.html
kill %1
```

Keep the code on the banner consistent with the first example in the README.

To use it as the social preview, upload `docs/assets/banner.png` under
**Settings → General → Social preview** (GitHub has no API for this).
