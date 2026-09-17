"""Build README animations with Pillow; run from any directory.

Usage: python scripts/tools/render_readme_media.py [--font /path/to/font.ttf]
Requires Pillow. Console PNGs are real browser captures in docs/assets/readme.
Runtime animation source: docs/assets/readme/runtime-animation.html.
Use render_runtime_animation.cjs to capture and encode its timeline.
"""
from pathlib import Path
import argparse
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'docs/assets/readme'
BG = '#080b10'
WHITE = '#edf2f7'
MUTED = '#98a8b9'
TEAL = '#63dcc4'
BLUE = '#87b8ff'
GOLD = '#f1c879'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path)
    parser.add_argument('--runtime-frames', type=Path)
    parser.add_argument('--language', choices=['zh', 'en'], default='zh')
    parser.add_argument('--runtime-only', action='store_true')
    args = parser.parse_args()
    candidates = [args.font, Path('C:/Windows/Fonts/segoeui.ttf'),
                  Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')]
    font_path = next((p for p in candidates if p and p.exists()), None)
    if font_path is None:
        raise SystemExit('Supply --font with an existing TrueType font.')
    font = lambda size: ImageFont.truetype(str(font_path), size)
    OUT.mkdir(parents=True, exist_ok=True)
    if args.runtime_frames:
        paths = sorted(args.runtime_frames.glob('[0-9][0-9][0-9][0-9].png'))
        if not paths:
            raise SystemExit('No runtime frames were found.')
        # 用多个场景建立共享调色板，避免帧间颜色抖动；仅编码变化区域。
        sample = Image.new('RGB', (1280, 800 * 6))
        for i, index in enumerate([0, 90, 175, 250, 350, len(paths)-1]):
            with Image.open(paths[min(index, len(paths)-1)]) as frame:
                sample.paste(frame, (0, 800*i))
        palette = sample.quantize(colors=256)
        frames = []
        for path in paths:
            with Image.open(path) as frame:
                frames.append(frame.convert('RGB').quantize(palette=palette, dither=Image.Dither.NONE))
        suffix = '.en' if args.language == 'en' else ''
        target = OUT / f'runtime-flow{suffix}.gif'
        frames[0].save(target, save_all=True, append_images=frames[1:],
                       duration=80, loop=0, optimize=True, disposal=1)
        with Image.open(args.runtime_frames / 'poster.png') as frame:
            frame.save(OUT / f'runtime-flow{suffix}.png', optimize=True)
        with Image.open(target) as animation:
            duration = sum(animation.seek(i) or animation.info.get('duration', 0)
                           for i in range(animation.n_frames))
            print(f'{target.name}: {animation.n_frames} frames, {duration/1000}s, {target.stat().st_size:,} bytes')
    if args.runtime_only:
        return

    steps=[('nodes','01 / NODES','Check the field connection.'),
           ('tasks','02 / TASKS','Find the configured collection task.'),
           ('runs','03 / RUNS','Open the successful execution.'),
           ('evidence','04 / ORIGINAL FILE','Trace the file path and SHA-256.'),
           ('qc','05 / QUALITY CONTROL','See which validation failed.'),
           ('alerts','06 / ALERTS','Follow the linked issue.')]
    slides=[]
    for name,title,description in steps:
        capture=Image.open(OUT/f'console-{name}.png').convert('RGB')
        slide=Image.new('RGB',(1440,1060),BG)
        slide.paste(capture,(0,80))
        d=ImageDraw.Draw(slide)
        d.text((24,15),'LABBRIDGE  /  CONSOLE WALKTHROUGH',font=font(15),fill=TEAL)
        d.text((24,41),title+'   '+description,font=font(22),fill=WHITE)
        d.text((24,1032),'ACTUAL LOCAL DEMO  /  CHINESE UI  /  CREDENTIAL ENTRY OMITTED',font=font(13),fill=MUTED)
        slides.append(slide.quantize(colors=256))
    slides[0].save(OUT/'console-demo.gif',save_all=True,append_images=slides[1:],duration=[2800,2800,3200,3600,4400,3600],loop=0,optimize=True,disposal=1)
    for file in [OUT/'console-demo.gif']:
        with Image.open(file) as im:
            print(f'{file.name}: {im.size}, {im.n_frames} frames, {file.stat().st_size:,} bytes')


if __name__ == '__main__':
    main()
