from pathlib import Path
from PIL import ImageFont
import struct, sys, zlib
out=Path(sys.argv[1])
if len(sys.argv) != 3:
    raise SystemExit("Usage: make-ui-font.py OUTPUT_HEADER NOTO_CJK_TTC_DIRECTORY")
fonts=Path(sys.argv[2])
ascii_chars=''.join(map(chr,range(32,127)))
body=ascii_chars+(
    "電池計測前回約記録保存なし：タップ使用状況給判定中"
    "長押源オフ起動再を短表示写真から戻る切替"
    "アラーム設定テスト受信開始停止下スワイ黄で閉じ"
    "今内訳はまだありません画面点灯音の路消待機緑重複あり費力未"
    "時刻天気取得に接続きデータ推値度現在名古屋晴れくもり霧雨雪にわか雷不明"
    "試聴毎日本量時分有効無鳴めした"
    "残段階秒観用擬複数交差個別新日時月火水木金土"
    "−＋−　／・（）℃")
heading=ascii_chars+(
    "ボタン操作赤黄青電池の使用状況現在天気名古屋晴れくもり霧雨雪にわか雷不明"
    "アラーム試聴毎日の時分有効無システム係信号点滅状態受オフ待ち複数の交差点"
    "日月火水木金土設定年月日時−＋　（）℃")
# Include UI literals from the app so a new fixed label cannot silently render as tofu.
ui_sources=out.with_name("StopWatchPICS.ino").read_text()+out.with_name("local_weather.h").read_text()+out.with_name("wifi_menu.h").read_text()
cjk=''.join(ch for ch in ui_sources if 0x3000 <= ord(ch) <= 0x9fff or 0xff00 <= ord(ch) <= 0xffef)
body+=cjk
heading+=cjk
def vlw(chars, fontfile, px):
    f=ImageFont.truetype(str(fonts/fontfile),px,index=0)
    specs=[]
    for c in sorted(set(chars)):
        mask,(dx,dy)=f.getmask2(c,mode="L",anchor="ls")
        w,h=mask.size
        specs.append((ord(c),h,w,round(f.getlength(c)),-dy,dx,0,bytes(mask)))
    a=max(g[4] for g in specs)
    d=max(g[1]-g[4] for g in specs)
    return (struct.pack(">6i",len(specs),11,px,0,a,d)
            +b"".join(struct.pack(">7i",*g[:7]) for g in specs)
            +b"".join(g[7] for g in specs))
sets=[("ui_body",body,"NotoSansCJK-Medium.ttc",21),
      ("ui_heading",heading,"NotoSansCJK-Bold.ttc",29),
      ("ui_clock","0123456789:-.℃ C","NotoSansCJK-Bold.ttc",47),
      ("ui_large","0123456789:-ALRM","NotoSansCJK-Bold.ttc",66)]
with out.open("w") as fp:
    fp.write("#pragma once\n#include <cstdint>\n// StopWatch UI glyphs derived from Noto Sans CJK JP, SIL OFL 1.1.\n")
    for name,chars,file,px in sets:
        raw=vlw(chars,file,px); data=zlib.compress(raw,9)
        assert zlib.decompress(data)==raw
        print(name,len(raw),len(data))
        fp.write(f"inline constexpr unsigned {name}_length={len(raw)};\n")
        fp.write(f"inline constexpr uint8_t {name}[] = {{\n")
        for i in range(0,len(data),24):
            fp.write("  "+",".join(f"0x{b:02x}" for b in data[i:i+24])+",\n")
        fp.write("};\n")
