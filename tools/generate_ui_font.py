#!/usr/bin/env python3
"""Generate and verify the shared Simplified Chinese UI fonts.

Two fonts are produced from the same Mi Sans source:

- 14 px, 4 bpp, full coverage: all 3,755 GB2312 level-one ideographs plus
  every non-ASCII character found in the firmware's compiled C strings.
- 24 px, 2 bpp, compact charset: ASCII, the action icons, the source-string
  glyphs and ~500 of the highest-frequency Chinese characters.  Its
  ``fallback`` pointer targets the 14 px font, so any character missing at
  24 px silently renders with the 14 px glyph instead.
"""

from __future__ import annotations

import argparse
import ast
import dataclasses
import re
import shlex
import subprocess
import sys
from os import environ
from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parents[1]
FONT_SOURCE = Path("assets/fonts/MiSans-Regular.otf")
ICON_FONT_SOURCE = Path(
    "managed_components/lvgl__lvgl/scripts/built_in_font/"
    "FontAwesome5-Solid+Brands+Regular.woff"
)
ASCII_RANGE = "0x20-0x7E"
ICON_RANGE = "0xF077-0xF078,0xF0CA,0xF013,0xF240-0xF244,0xF1EB,0xF293"
ICON_GLYPH_COUNT = 11
COMMON_PUNCTUATION = "，。！？；：、“”‘’（）《》〈〉【】〔〕…—·￥～"
SDKCONFIG_DEFAULTS = Path("sdkconfig.defaults")
COMPRESSED_FONT_OPTION = "CONFIG_LV_USE_FONT_COMPRESSED"

# 昵称固定用字（24px 紧凑字库必须包含，避免回退到 14px 混排）
NICKNAME_CHARS = "周旋"

# Highest-frequency Simplified Chinese characters (approximate frequency
# order; membership matters, order does not) plus common surnames, since the
# badge fields are mostly names.  Characters outside this set fall back to the
# 14 px font when rendered at 24 px.
TOP_FREQUENCY_CHARS = (
    "的一是了我不人在他有这上们来到时大地为子中你说生国年着就那和要她出也得里"
    "后自以会家可下而过天去能对小多然于心学么之都好看起发当没成只如事把还用第"
    "样道想作种开美总从无情己面最女但现前些所同日手又行意动方期它头经长儿回位"
    "分爱老因很给名法间斯知世什两次使身者被高已亲其进此话常与活正感"
    "见明问力理尔点文几定本公特做外孩相西果走将月十实向声车全信重三机工物气每"
    "并别真打太新比才便夫再书部水像眼等体却加电主界门利海受听表德少克代员许先"
    "口由死安写性马光白或住难望教命花结乐色更拉东神记处让母父应直字场平报友关"
    "放至张认接告入笑内英军候民岁往何度山觉路带万男边风解叫任金快原吃妈变通师"
    "立象数四失满战远格士音轻目条呢病始达深完今提求清王化空业思切怎非找片罗钱"
    "吗语元喜曾离飞科言干流欢约各即指合反题必该论交终林请医晚制球决传画保读运"
    "及则房早院量苦火布值紧左攻盛习右"
    "而图止革强验究根功存类推基较试细算批担择护责执展构苏显热查整乱举升集践属"
    "突击证据收备志劳况准半青百办送急停质演油析异足除响致支八般适讲按市降占阳"
    "威触密补短仍装速雄独施峰渐杯排议兵卫察例供红继纸据参续遍拉述维持压"
    "啦哪谢宜连委服争某议迎慢怕初专号"
    "冯蒋沈韩杨朱秦尤吕施孔曹严华金魏陶姜戚邹喻柏水窦章云潘葛范彭郎鲁韦昌苗凤"
    "方俞袁柳唐罗薛雷贺倪汤滕殷毕郝邬常于时傅皮齐康伍余元卜顾孟黄穆萧尹姚邵湛"
    "汪祁毛禹狄米贝臧计伏谈宋茅庞熊纪舒屈项祝董梁杜阮蓝闵席季麻贾路娄危江童颜"
    "郭梅盛林刁钟徐邱骆高夏蔡田樊胡凌霍虞万支柯管卢莫"
)

C_STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"', re.DOTALL)
LOCAL_INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)
OPTS_RE = re.compile(r"^ \* Opts: (.+)$", re.MULTILINE)
BITMAP_FORMAT_RE = re.compile(r"\.bitmap_format\s*=\s*(\d+)")

# 生成文件自身不作为「源码用字」的扫描对象
_FONT_OUTPUT_PATHS = (
    Path("components/passport_ui/src/passport_ui_font_zh_14.c"),
    Path("components/passport_ui/src/passport_ui_font_zh_24.c"),
)


@dataclasses.dataclass(frozen=True)
class FontSpec:
    size: int
    bpp: int
    name: str
    output: Path
    extra_glyphs: frozenset[str]
    fallback: str | None
    # (字体文件, code range 或 None) 列表；None 表示该字体仅取 --symbols 中的字符
    sources: tuple[tuple[Path, str | None], ...] = ()


def gb2312_level_one_glyphs() -> frozenset[str]:
    """Return the 3755 common ideographs in GB2312 rows 16 through 55."""
    glyphs: set[str] = set()
    for lead in range(0xB0, 0xD8):
        for trail in range(0xA1, 0xFF):
            try:
                glyphs.add(bytes((lead, trail)).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    if len(glyphs) != 3755:
        raise RuntimeError(f"unexpected GB2312 level-one glyph count: {len(glyphs)}")
    return frozenset(glyphs)


def compact_glyphs() -> frozenset[str]:
    """Return the ~600 highest-frequency characters used by the 24 px font."""
    glyphs = {ch for ch in TOP_FREQUENCY_CHARS if 0x4E00 <= ord(ch) <= 0x9FFF}
    glyphs |= set(NICKNAME_CHARS)  # 昵称用字必须落在 24px 档内
    return frozenset(glyphs)


def _component_dirs(project_dir: Path) -> list[Path]:
    roots = [project_dir / "main"]
    components = project_dir / "components"
    if components.is_dir():
        roots.extend(path for path in sorted(components.iterdir()) if path.is_dir())
    return roots


def _compiled_sources(project_dir: Path) -> list[Path]:
    """Read literal source entries from each idf_component_register call."""
    generated = {(project_dir / path).resolve() for path in _FONT_OUTPUT_PATHS}
    files: list[Path] = []
    for root in _component_dirs(project_dir):
        cmake = root / "CMakeLists.txt"
        if not cmake.is_file():
            continue
        source = cmake.read_text(encoding="utf-8")
        match = re.search(
            r"\bSRCS\b(.*?)(?:\bINCLUDE_DIRS\b|\bPRIV_INCLUDE_DIRS\b|"
            r"\bREQUIRES\b|\bPRIV_REQUIRES\b|\))",
            source,
            re.DOTALL,
        )
        if match is None:
            continue
        for relative in re.findall(r'"([^"]+)"', match.group(1)):
            path = (root / relative).resolve()
            if path in generated:
                continue
            if path.suffix not in {".c", ".h"} or not path.is_file():
                raise FileNotFoundError(path)
            files.append(path)
    if not files:
        raise ValueError("no CMake source files found")
    return files


def source_files(project_dir: Path = PROJECT_DIR) -> list[Path]:
    """Return compiled sources and project-local headers reachable from them."""
    roots = _component_dirs(project_dir)
    include_dirs = [root for root in roots]
    include_dirs.extend(root / "include" for root in roots)
    include_dirs.extend(root / "src" for root in roots)
    project_root = project_dir.resolve()
    generated = {(project_dir / path).resolve() for path in _FONT_OUTPUT_PATHS}
    pending = _compiled_sources(project_dir)
    discovered: set[Path] = set()

    while pending:
        path = pending.pop().resolve()
        if path in discovered or path in generated:
            continue
        discovered.add(path)
        source = path.read_text(encoding="utf-8")
        for include in LOCAL_INCLUDE_RE.findall(source):
            candidates = [path.parent / include]
            candidates.extend(directory / include for directory in include_dirs)
            for candidate in candidates:
                candidate = candidate.resolve()
                try:
                    candidate.relative_to(project_root)
                except ValueError:
                    continue
                if candidate.is_file() and candidate.suffix == ".h":
                    pending.append(candidate)
                    break
    return sorted(discovered)


def _decode_c_literal(literal: str) -> str:
    value = ast.literal_eval(literal)
    if not isinstance(value, str):
        raise TypeError(f"unexpected non-string C literal: {literal}")
    return value


def source_glyphs(project_dir: Path = PROJECT_DIR) -> frozenset[str]:
    glyphs: set[str] = set(COMMON_PUNCTUATION)
    for path in source_files(project_dir):
        source = path.read_text(encoding="utf-8")
        for match in C_STRING_RE.finditer(source):
            try:
                value = _decode_c_literal(match.group(0))
            except (SyntaxError, ValueError):
                continue
            glyphs.update(
                character
                for character in value
                if ord(character) > 0x7F
                and character.isprintable()
                and not character.isspace()
                and not 0xE000 <= ord(character) <= 0xF8FF
            )
    return frozenset(glyphs)


def font_specs(project_dir: Path = PROJECT_DIR) -> list[FontSpec]:
    """Build the per-font specifications (charset differs per font)."""
    source = source_glyphs(project_dir)
    full_sources = ((FONT_SOURCE, ASCII_RANGE), (ICON_FONT_SOURCE, ICON_RANGE))
    return [
        FontSpec(
            size=14,
            bpp=4,
            name="passport_ui_font_zh_14",
            output=Path("components/passport_ui/src/passport_ui_font_zh_14.c"),
            extra_glyphs=frozenset(gb2312_level_one_glyphs()) | source,
            fallback=None,
            sources=full_sources,
        ),
        FontSpec(
            size=24,
            bpp=2,
            name="passport_ui_font_zh_24",
            output=Path("components/passport_ui/src/passport_ui_font_zh_24.c"),
            extra_glyphs=compact_glyphs() | source,
            fallback="passport_ui_font_zh_14",
            sources=full_sources,
        ),
    ]


FONT_SPECS: list[FontSpec] = []  # populated by font_specs(); kept for tests/debug


def _generated_options(path: Path) -> list[str]:
    match = OPTS_RE.search(path.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"missing lv_font_conv options in {path}")
    # posix=False 保留路径中的反斜杠（Windows 上 lv_font_conv 会把路径
    # 重写成 \ 分隔，POSIX 模式会当作转义符吞掉）
    return shlex.split(match.group(1), posix=False)


def _option_value(options: list[str], name: str) -> str | None:
    try:
        return options[options.index(name) + 1]
    except (ValueError, IndexError):
        return None


def _option_values(options: list[str], name: str) -> list[str]:
    """Return every value belonging to a repeated lv_font_conv option."""
    return [
        options[index + 1]
        for index, option in enumerate(options[:-1])
        if option == name
    ]


def generated_bitmap_format(path: Path) -> int:
    """Return the single bitmap format declared by an LVGL C font."""
    formats = {int(value) for value in BITMAP_FORMAT_RE.findall(path.read_text(encoding="utf-8"))}
    if len(formats) != 1:
        raise ValueError(f"expected one bitmap_format in {path}, found {sorted(formats)}")
    return formats.pop()


def sdkconfig_option_enabled(project_dir: Path, option: str) -> bool:
    """Return whether an sdkconfig default is explicitly enabled."""
    config = project_dir / SDKCONFIG_DEFAULTS
    if not config.is_file():
        return False
    return re.search(
        rf"^{re.escape(option)}=y$", config.read_text(encoding="utf-8"), re.MULTILINE
    ) is not None


def check_font(spec: FontSpec, project_dir: Path) -> int:
    output = project_dir / spec.output
    if not output.is_file():
        print(f"missing generated font: {spec.output}", file=sys.stderr)
        return 1

    expected = spec.extra_glyphs
    options = _generated_options(output)
    actual = set(_option_value(options, "--symbols") or "")
    expected_options = {
        "--size": str(spec.size),
        "--bpp": str(spec.bpp),
        "--format": "lvgl",
        "--lv-font-name": spec.name,
    }
    errors = [
        f"{name} must be {value}"
        for name, value in expected_options.items()
        if _option_value(options, name) != value
    ]
    if "--no-kerning" not in options:
        errors.append("--no-kerning must be enabled")
    expected_fonts = [Path(f).as_posix() for f, _ in spec.sources]
    actual_fonts = [p.replace("\\", "/") for p in _option_values(options, "--font")]
    if actual_fonts != expected_fonts:
        errors.append(f"--font must be {expected_fonts}, got {actual_fonts}")
    expected_ranges = [r for _, r in spec.sources if r]
    actual_ranges = _option_values(options, "-r")
    if actual_ranges != expected_ranges:
        errors.append(f"-r must be {expected_ranges}, got {actual_ranges}")

    try:
        bitmap_format = generated_bitmap_format(output)
    except ValueError as error:
        errors.append(str(error))
    else:
        if bitmap_format != 0 and not sdkconfig_option_enabled(
            project_dir, COMPRESSED_FONT_OPTION
        ):
            errors.append(
                f"{COMPRESSED_FONT_OPTION}=y is required for compressed "
                f"bitmap_format={bitmap_format}"
            )

    missing = expected - actual
    extra = actual - expected
    if missing:
        errors.append(f"missing {len(missing)} glyphs")
    if extra:
        errors.append(f"extra {len(extra)} glyphs")

    text = output.read_text(encoding="utf-8")
    if spec.fallback:
        if f".fallback = &{spec.fallback}," not in text:
            errors.append(f"fallback must target {spec.fallback}")
        if f"LV_FONT_DECLARE({spec.fallback});" not in text:
            errors.append(f"missing LV_FONT_DECLARE({spec.fallback})")
    elif ".fallback = NULL," not in text:
        errors.append("fallback must stay unset for the full-coverage font")

    if errors:
        for error in errors:
            print(f"UI font check failed ({spec.name}): {error}", file=sys.stderr)
        print("Run: python3 tools/generate_ui_font.py", file=sys.stderr)
        return 1

    fixed = sum(95 for _, r in spec.sources if r == ASCII_RANGE)
    fixed += sum(ICON_GLYPH_COUNT for _, r in spec.sources if r == ICON_RANGE)
    print(
        f"UI font coverage: PASS {spec.name} "
        f"(size={spec.size} bpp={spec.bpp}, {len(expected) + fixed} glyphs, "
        f"fallback={spec.fallback or 'none'})"
    )
    return 0


def generate_font(spec: FontSpec, project_dir: Path) -> None:
    source = project_dir / FONT_SOURCE
    icon_source = project_dir / ICON_FONT_SOURCE
    output = project_dir / spec.output
    if not source.is_file():
        raise FileNotFoundError(source)
    if not icon_source.is_file():
        raise FileNotFoundError(icon_source)
    output.parent.mkdir(parents=True, exist_ok=True)
    symbols = "".join(sorted(spec.extra_glyphs, key=ord))
    command = [
        "lv_font_conv.cmd",
        "--size",
        str(spec.size),
        "--bpp",
        str(spec.bpp),
        "--format",
        "lvgl",
        "--lv-include",
        "lvgl.h",
        "--lv-font-name",
        spec.name,
        "-o",
        str(spec.output),
    ]
    for i, (font_path, glyph_range) in enumerate(spec.sources):
        command += ["--font", str(font_path)]
        if glyph_range:
            command += ["-r", glyph_range]
        if i == 0:
            # --symbols 跟随前一个 --font 生效，必须紧跟第一个字体
            command += ["--symbols", symbols]
    command += ["--no-kerning"]
    fixed = sum(95 for _, r in spec.sources if r == ASCII_RANGE)
    fixed += sum(ICON_GLYPH_COUNT for _, r in spec.sources if r == ICON_RANGE)
    print(
        f"Generating {spec.output} ({spec.size}px {spec.bpp}bpp, "
        f"{len(symbols) + fixed} glyphs)"
    )
    env = {**dict(environ), "PATH": "E:\\code\\code tools\\npm-global;" + str(environ.get("PATH", ""))}
    subprocess.run(command, cwd=project_dir, check=True, env=env)
    if spec.fallback:
        text = output.read_text(encoding="utf-8")
        text, count = re.subn(
            r"\.fallback = NULL,",
            f".fallback = &{spec.fallback},",
            text,
        )
        if count != 1:
            raise RuntimeError(f"expected exactly one .fallback in {spec.output}, found {count}")
        # 结构体引用了另一套字库的符号，需要在 include 块后补前向声明
        inc = text.find("#include")
        endif_end = text.find("#endif", inc)
        if inc < 0 or endif_end < 0:
            raise RuntimeError(f"cannot locate include block in {spec.output}")
        insert_at = text.find("\n", endif_end) + 1
        text = text[:insert_at] + f"LV_FONT_DECLARE({spec.fallback});\n" + text[insert_at:]
        output.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the committed fonts without invoking lv_font_conv",
    )
    args = parser.parse_args()
    specs = font_specs()
    if args.check:
        return max(check_font(spec, PROJECT_DIR) for spec in specs)
    for spec in specs:
        generate_font(spec, PROJECT_DIR)
    return max(check_font(spec, PROJECT_DIR) for spec in specs)


if __name__ == "__main__":
    raise SystemExit(main())
