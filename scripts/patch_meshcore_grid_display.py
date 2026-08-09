Import("env")

from pathlib import Path


def _patch_once(path: Path, old: str, new: str) -> bool:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return False
    if old not in text:
        raise RuntimeError(f"Expected snippet not found in {path}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    return True


def main() -> None:
    pioenv = env.get("PIOENV", "")
    if pioenv != "heltec_v4_grid_custom_touch":
        print(f"[grid-display-patch] skip env={pioenv}")
        return

    project_dir = Path(env.get("PROJECT_DIR"))
    libdeps_dir = Path(env.get("PROJECT_LIBDEPS_DIR"))
    meshcore_dir = libdeps_dir / pioenv / "MeshCore" / "src" / "helpers" / "ui"

    header = meshcore_dir / "ST7789LCDDisplay.h"
    cpp = meshcore_dir / "ST7789LCDDisplay.cpp"

    if not header.exists() or not cpp.exists():
        print("[grid-display-patch] MeshCore UI files not found yet; skipping this run")
        return

    changed = False

    changed |= _patch_once(
        header,
        "#include <Adafruit_ST7789.h>",
        "#if defined(HELTEC_V4_GRID_CUSTOM)\n#include <Adafruit_ST7796S.h>\n#else\n#include <Adafruit_ST7789.h>\n#endif",
    )

    changed |= _patch_once(
        header,
        "  Adafruit_ST7789 display;",
        "#if defined(HELTEC_V4_GRID_CUSTOM)\n  Adafruit_ST7796S display;\n#else\n  Adafruit_ST7789 display;\n#endif",
    )

    changed |= _patch_once(
        cpp,
        "#define DISPLAY_WIDTH 240\n#define DISPLAY_HEIGHT 320",
        "#if defined(HELTEC_V4_GRID_CUSTOM)\n#define DISPLAY_WIDTH 320\n#define DISPLAY_HEIGHT 480\n#else\n#define DISPLAY_WIDTH 240\n#define DISPLAY_HEIGHT 320\n#endif",
    )

    changed |= _patch_once(
        cpp,
        "    display.init(DISPLAY_WIDTH, DISPLAY_HEIGHT);",
        "#if defined(HELTEC_V4_GRID_CUSTOM)\n    display.init(DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0, ST7796S_BGR);\n#else\n    display.init(DISPLAY_WIDTH, DISPLAY_HEIGHT);\n#endif",
    )

    if changed:
        print("[grid-display-patch] applied ST7796S/320x480 compatibility patch")
    else:
        print("[grid-display-patch] already patched")


main()
