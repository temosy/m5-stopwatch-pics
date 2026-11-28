# M5 StopWatch PICS モニター

M5Stack StopWatch で、対応交差点が放送する高度化PICSのBLEアドバタイズを受信・可視化する試作です。信号の状態と残り段階（1〜8）を、丸い画面の数字とリングに表示します。送信機への接続や、模擬データの外部送信は行いません。

> **これは観測・工作用です。** 残り段階は秒数ではありません。受信が途切れたり、表示中のチャンネルが目の前の横断歩道と一致しなかったりします。横断の判断には使わず、現地の信号を確認してください。

## できること

- 受信した赤・青・青点滅を、丸い画面の大きな数字とリングで表示。実受信とデモには同じ信号画面を使い、デモには「テスト・模擬データ」と表示します。新しい信号データがない場合は待機画面に戻り、複数の交差点を検出した場合は個別表示を停止します。
- 日本語フォントとリングを滑らかに描画。待ち受けに曜日、時刻、モカとラッテ、電池・天気のボタンを配置。下端の白いガイドから上へスワイプすると操作説明を開きます。
- 起動時はBLE受信オフ。青ボタンを長押ししてオン／オフを切り替えます。受信中の時間制限はなく、画面は点灯したままです。青ボタンの短押しは、信号表示と確認音を試すデモです。
- 受信オフで無操作30秒になると、電池駆動・USB接続の両方で表示回路の電源を切り、黄色ボタンで復帰します。本体は動き続けるのでアラームも動作します。電源状態の確認や表示回路の切断ができないときは、表示回路を維持したまま消灯します。
- 電池駆動中は緑の状態LEDを消灯。電池ボタンから、表示・BLE・Wi-Fiなどの動作時間を確認できます。内訳は消費電力の実測値ではなく、動作時間の目安です。
- Wi-Fi接続先を本体に最大5件保存。時刻合わせと、設定した地点の今日・明日の天気表示に使います。

## ボタン操作

| ボタン | 操作 |
| --- | --- |
| 赤 | 電池駆動：長押しで電源オフ、素早く3回で再起動（電池計測を保存）。USB接続：短押しで起動・再起動、素早く2回で電源オフ |
| 黄 | 短押しで画面復帰／写真から戻る／受信チャンネル切替。長押しでアラーム設定 |
| 青 | 通常画面では短押しでデモ、長押しで実受信のオン／オフ。アラーム設定中は短押しで保存、長押しで試聴 |

## ビルドと書き込み

対象は **M5Stack StopWatch（C152 / ESP32-S3 / 16 MBフラッシュ）** です。Arduino CLI 1.6.0、M5Stack Arduino core 3.3.10、M5GFX 0.3.0、M5Unified 0.3.0でビルドを確認しました。

1. [M5Stackのボード設定](https://docs.m5stack.com/ja/arduino/arduino_board)に従ってM5Stackのボードパッケージを入れ、`M5StopWatch` を選びます。
2. Arduino IDEの場合は、`StopWatchPICS/StopWatchPICS.ino` を開き、M5GFXとM5Unifiedを入れてビルドします。Arduino CLIの場合はリポジトリのルートで以下を実行します。

```sh
BOARD_INDEX='https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json'
arduino-cli core update-index --additional-urls "$BOARD_INDEX"
arduino-cli core install m5stack:esp32@3.3.10 --additional-urls "$BOARD_INDEX"
arduino-cli lib install M5GFX@0.3.0 M5Unified@0.3.0
arduino-cli compile --fqbn m5stack:esp32:m5stack_stopwatch \
  --build-path "$PWD/build" "$PWD/StopWatchPICS"
```

3. 書き込み前に、必要なら元のファームウェアと設定を自分の端末からバックアップしてください。書き込みによって元のアプリは置き換わります。USBデータケーブルで接続し、[StopWatch公式の手順](https://docs.m5stack.com/en/arduino/stopwatch/program)で書き込みモードに入れ、IDEから書き込むか、ポートを選んで下記を実行します。

```sh
arduino-cli board list
PORT=/dev/cu.usbmodemXXXX  # 実際のポートに置き換える。Windowsなら COM番号
arduino-cli upload -p "$PORT" --fqbn m5stack:esp32:m5stack_stopwatch \
  --input-dir "$PWD/build" "$PWD/StopWatchPICS"
```

### 好きな写真を表示する（任意）

公開リポジトリには写真データを含めていません。写真なしでも通常のPICSモニターとして動きます。写真を使う場合は、リポジトリのルートで次のように、表示順に画像ファイルを指定してから再ビルド・書き込みします。

```sh
python3 -m venv .venv
.venv/bin/python -m pip install Pillow
.venv/bin/python tools/make-gallery.py /path/to/photo1.jpg /path/to/photo2.png
```

最大12枚、変換後のJPEGは合計850,000バイトまで。上限を超える場合は写真を減らしてください。写真は縦横比を保ち、四隅を滑らかに丸く表示します。

- 全体表示では、左右スワイプで写真をめくります。
- ダブルタップで全体 → 2倍 → 4倍 → 8倍 → 全体と切り替えます。拡大中は上下左右にドラッグして見たい場所へ動かせます。
- 写真表示中の黄色短押しで通常画面、青短押しで模擬受信テスト。消灯中はまず黄色で画面を戻してください。受信がオンの場合、写真表示中もBLEスキャンは続きます。
- 旧版で生成した写真ヘッダーがある場合は、この版のツールで作り直してください。

写真は生成ファイル `StopWatchPICS/photo_gallery.local.h` と、そのファイルを使ってビルドしたファームウェアに埋め込まれます。元画像も含めて、GitHubや配布用ファームウェアには追加しないでください。写真なしへ戻すときは、生成ファイルを外して再ビルド・書き込みします。

### Wi-Fiと天気（任意）

待ち受けの天気ボタンから「Wi-Fi設定」を開き、SSIDとパスワードを本体で入力します。天気を表示したい場合は地点の緯度・経度を入力してください。Wi-Fi経由で現在地を自動推定する機能ではありません。地点が未設定でもWi-Fiで時計を合わせられます。

設定は本体に保存され、公開コードに登録済みのWi-Fiは入っていません。接続時にNICTのNTPサーバーを利用し、天気の取得時は設定地点（小数第2位まで）を [Open-Meteo](https://open-meteo.com/) に送ります。不要ならWi-Fiを登録せず、USBで時刻を合わせられます。

### 毎日のアラーム

黄色を長押しして設定を開き、画面上の＋／−で時刻を選び、ONにして青を短押しすると保存されます。初期状態はOFFです。画面の「試聴」をタップするか、設定中に青を長押しすると約4秒だけ音を確認できます（音量MAX）。黄・青ボタン、または画面をタッチすると停止します。

アラームは本体の日本時間を使い、電源が入っていれば画面消灯中も動作します。電源オフ・電池切れ・時計未設定のときは鳴りません。

### 時計を合わせる

初回、または画面に「日時未設定」と出たら、書き込みモードを抜けて通常起動し、給電したままPCのシリアルモニターを閉じて実行します。PCの時計が正しいことを確認してください。時刻はUSBで設定し、StopWatchのRTCにはJSTで保存します。

```sh
python3 -m venv .venv
.venv/bin/python -m pip install pyserial
.venv/bin/python tools/sync-clock.py "$PORT"
```

Windowsでは `.venv\Scripts\python.exe` を使用してください。

## 確認範囲

公開版は写真・Wi-Fi設定なしでビルドを確認しました。表示回路の電源オフと黄色ボタンでの復帰は、同じ処理を組み込んだ実機版で電池駆動・USB接続の両方を確認しています。電池持ちの改善幅と、予約時刻でのアラーム鳴動は未確認です。

2026年11月28日、高度化PICS対応の交差点1か所で、M5 StopWatchが信号情報をBLEで受信し、画面に表示することを確認しました。[現地テストの動画（X）](https://x.com/temosy/status/2126609939972112830)

## 参考・権利表記

- [Tsubasa Yumura / ble-pics-viewer](https://github.com/yumu19/ble-pics-viewer) の解析・可視化実装を参考にしました。上流のMITライセンスと著作権表示は [THIRD-PARTY-LICENSE.txt](THIRD-PARTY-LICENSE.txt) にあります。
- 画面の構成はビーバーズ・ハイブの[技術記事](https://zenn.dev/beavers_hive/articles/m5singo-ble-pics)と[制作動画](https://www.youtube.com/watch?v=muLwOMST8zg)を参考にしています。
- 画面のビットマップ字形は [Noto Sans CJK JP](https://github.com/notofonts/noto-cjk) から生成したサブセットです。著作権表示とSIL Open Font License 1.1は [licenses/STOPWATCH-UI-OFL.txt](licenses/STOPWATCH-UI-OFL.txt) にあります。再生成する場合はPillowを入れ、`python tools/make-ui-font.py StopWatchPICS/ui_type.h TTCのディレクトリ` を実行してください。
- このリポジトリ独自の追加部分について、再配布・改変などのライセンスは未指定です。システム係のロゴは上流のMITライセンスの対象ではありません。
