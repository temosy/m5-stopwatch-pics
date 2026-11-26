# M5 StopWatch PICS モニター

M5Stack StopWatch で、対応交差点が放送する高度化PICSのBLEアドバタイズを受信・可視化する試作です。信号の状態と残り段階（1〜8）を、丸い画面の数字とリングに表示します。送信機への接続や、模擬データの外部送信は行いません。

> **これは観測・工作用です。** 残り段階は秒数ではありません。受信が途切れたり、表示中のチャンネルが目の前の横断歩道と一致しなかったりします。横断の判断には使わず、現地の信号を確認してください。

## できること

- 受信した赤・青・青点滅を、1チャンネルずつ表示。選択中のチャンネルは画面に表示します。新しいデータがない場合は受信待ち、複数の交差点を検出した場合は個別表示を停止します。
- 青ボタンで実際の受信とは独立した模擬テスト。段階や色の変化、受信待ちへの切り替えと短い確認音を、対応交差点に行かずに確認できます。
- 日本時間の時計、電池残量（推定値）、電池で動いた時間の計測。電池切れなどで突然止まった場合、前回の記録は最後に保存した時点までの概算です。計測中は約1分ごとに保存します。
- 30秒無操作で消灯。黒を表示してAMOLEDの輝度を0にし、表示パネルはスリープさせません。黄色ボタンで輝度を戻して再描画します。消灯中もBLE受信は続きます。この方式での電池持ちは未計測です。
- 画面にはシステム係のマークと、白いモカと茶色いラッテが表示されます。

## ボタン操作

| ボタン | 操作 |
| --- | --- |
| 赤 | 1回短押しで電源オン／再起動。素早く2回で電源オフ |
| 黄 | 短押しで画面復帰／信号切り替え。長押しでアラーム設定 |
| 青 | 通常画面では模擬受信テスト（約21秒）の開始・終了。アラーム設定中は短押しで保存、長押しで試聴 |

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

最大12枚、変換後のJPEG合計が1,350,000バイト以内。上限を超える場合は写真を減らしてください。写真は縦横比を保って四隅を丸く表示します。

- 全体表示では、左右スワイプで写真をめくります。
- ダブルタップで全体 → 2倍 → 4倍 → 全体と切り替えます。拡大中は上下左右にドラッグして見たい場所へ動かせます。
- 写真表示中の黄色短押しで通常画面、青短押しで模擬受信テスト。消灯中はまず黄色で画面を戻してください。写真表示中もBLEスキャンは続きます。
- 旧版で生成した写真ヘッダーがある場合は、この版のツールで作り直してください。

写真は生成ファイル `StopWatchPICS/photo_gallery.local.h` と、そのファイルを使ってビルドしたファームウェアに埋め込まれます。元画像も含めて、GitHubや配布用ファームウェアには追加しないでください。写真なしへ戻すときは、生成ファイルを外して再ビルド・書き込みします。

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

本体への書き込み・起動、BLEスキャン開始、青ボタンの模擬データ処理を確認済みです。消灯方式の修正後は、30秒での自動消灯、消灯中のBLEスキャン継続、黄色ボタンでの画面復帰を実機で確認しました。日時が表示されない場合は、上記の時計合わせを実行してください。実機のローカル版で、写真の2倍・4倍表示、拡大中のドラッグ、アラーム試聴はタップと青長押しの両方で再生処理が始まることを実機ログで確認しました。予約時刻での鳴動と、実際の交差点での受信は未確認です。

## 参考・権利表記

- [Tsubasa Yumura / ble-pics-viewer](https://github.com/yumu19/ble-pics-viewer) の解析・可視化実装を参考にしました。上流のMITライセンスと著作権表示は [THIRD-PARTY-LICENSE.txt](THIRD-PARTY-LICENSE.txt) にあります。
- 画面の構成はビーバーズ・ハイブの[技術記事](https://zenn.dev/beavers_hive/articles/m5singo-ble-pics)と[制作動画](https://www.youtube.com/watch?v=muLwOMST8zg)を参考にしています。
- このリポジトリ独自の追加部分について、再配布・改変などのライセンスは未指定です。システム係のロゴは上流のMITライセンスの対象ではありません。
