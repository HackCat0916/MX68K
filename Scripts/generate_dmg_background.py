#!/usr/bin/env python3
"""MX68K dmgインストーラーの背景画像を生成する(開発時にデザインを調整する際に
実行する補助スクリプト。Scripts/make_release_dmg.sh の実行時には使わない
——このスクリプトが生成する Scripts/dmg_background.png を静的アセットとして
直接コミットし、リリースのたびに再生成はしない)。
出力: Scripts/dmg_background.png (660x400)
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

# MX68Kアプリアイコン(Assets.xcassets)から抽出した配色
GOLD = (196, 140, 48)
GOLD_LIGHT = (231, 185, 110)
SILVER = (148, 147, 148)
TEXT_DARK = (51, 51, 51)
TEXT_SUB = (120, 120, 120)

SCALE = 2  # @2x で描画してからダウンサンプル(アンチエイリアス)
W, H = 660, 400
ICON_X, ICON_Y = 165, 165   # Finder上のアプリアイコン中心位置(アイコンサイズ128相当)
APP_X, APP_Y = 495, 165     # Applicationsフォルダアイコン中心位置

FONT_JP = "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc"
FONT_EN_BOLD = "/System/Library/Fonts/Helvetica.ttc"


def draw_arrow(draw, x1, y1, x2, y2, color, shaft_width):
    """矢印を1つの閉じたポリゴン(本体の矩形+矢じりの三角形)として描画する。
    直線を3本(本体+矢じり2本)個別に描くと、線の太さを増したときに
    継ぎ目がカクついて見える問題があったため、塗りつぶし形状に変更した
    (石井さんの指摘、2026-10-02)。"""
    angle = math.atan2(y2 - y1, x2 - x1)
    head_len = shaft_width * 3.2
    head_w = shaft_width * 2.4

    # 矢じりの付け根(本体の終点)
    base_x = x2 - head_len * math.cos(angle)
    base_y = y2 - head_len * math.sin(angle)

    # 進行方向に垂直な単位ベクトル
    perp_x = -math.sin(angle)
    perp_y = math.cos(angle)

    shaft_half = shaft_width / 2
    shaft_poly = [
        (x1 + perp_x * shaft_half, y1 + perp_y * shaft_half),
        (base_x + perp_x * shaft_half, base_y + perp_y * shaft_half),
        (base_x - perp_x * shaft_half, base_y - perp_y * shaft_half),
        (x1 - perp_x * shaft_half, y1 - perp_y * shaft_half),
    ]
    draw.polygon(shaft_poly, fill=color)

    head_half = head_w / 2
    head_poly = [
        (base_x + perp_x * head_half, base_y + perp_y * head_half),
        (x2, y2),
        (base_x - perp_x * head_half, base_y - perp_y * head_half),
    ]
    draw.polygon(head_poly, fill=color)


def main():
    w, h = W * SCALE, H * SCALE
    img = Image.new("RGB", (w, h), (255, 255, 255))
    draw = ImageDraw.Draw(img)

    # 背景: ごく薄い縦グラデーション(白→薄いグレー)、アイコンの白背景と調和させる
    top = (255, 255, 255)
    bottom = (238, 238, 240)
    for y in range(h):
        t = y / h
        r = int(top[0] + (bottom[0] - top[0]) * t)
        g = int(top[1] + (bottom[1] - top[1]) * t)
        b = int(top[2] + (bottom[2] - top[2]) * t)
        draw.line([(0, y), (w, y)], fill=(r, g, b))

    # 矢印(アイコン位置→Applications位置の中間を水平に)。
    # ★2026-10-02改訂: 石井さんの実機確認で「矢印の先端がフォルダアイコンに
    # 対してズレて見える」との指摘。こちらの環境でFinderの実際のbounds座標に
    # 合わせて正確に検証した結果、座標計算自体にズレは無かった(矢印中心Y=197
    # vs フォルダ本体中心Y=194、ほぼ一致)が、表示環境(ディスプレイ解像度等)
    # による見え方の差があり得るため、多少のズレでも破綻しにくいよう矢印を
    # 短く・太くし、アイコンとの間隔を広げる方向でより頑健なデザインへ変更。
    arrow_y = ICON_Y * SCALE
    arrow_x1 = (ICON_X + 85) * SCALE
    arrow_x2 = (APP_X - 85) * SCALE
    draw_arrow(draw, arrow_x1, arrow_y, arrow_x2, arrow_y, GOLD, 8 * SCALE)

    # 案内文言(日本語+英語)
    font_jp = ImageFont.truetype(FONT_JP, 20 * SCALE)
    font_en = ImageFont.truetype(FONT_EN_BOLD, 13 * SCALE)

    jp_text = "MX68K をアプリケーションフォルダへドラッグしてください"
    en_text = "Drag MX68K to the Applications folder to install"

    def centered_text(y, text, font, fill):
        bbox = draw.textbbox((0, 0), text, font=font)
        tw = bbox[2] - bbox[0]
        draw.text(((w - tw) / 2, y), text, font=font, fill=fill)

    centered_text(310 * SCALE, jp_text, font_jp, TEXT_DARK)
    centered_text(345 * SCALE, en_text, font_en, TEXT_SUB)

    img = img.resize((W, H), Image.LANCZOS)
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dmg_background.png")
    img.save(out_path)
    print("saved:", out_path)


if __name__ == "__main__":
    main()
