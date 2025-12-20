# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

CapsuleSamplerはESP32マイコン向けのリアルタイムオーディオサンプラーライブラリです。MIDI音源として使用でき、16チャンネル・最大32同時発音に対応しています。ADSRエンベロープ、ピッチベンド、シュレーダーリバーブエフェクトを内蔵しています。

## ビルドコマンド

PlatformIOプロジェクトです。以下のコマンドでビルド・アップロードできます:

```bash
# M5Stack CoreS3向けにビルド（デフォルト）
pio run

# 特定の環境向けにビルド
pio run -e m5stack-cores3
pio run -e m5stack-core2
pio run -e native           # SDL2を使用したデスクトップビルド（テスト用）

# デバイスにアップロード
pio run -t upload
```

native環境はデスクトップでのシミュレーション用で、SDL2のインストールが必要です。

## アーキテクチャ

### コアコンポーネント（`include/`および`src/`）

- **Sampler** (`Sampler.h/cpp`): メインのサンプラーエンジン。16チャンネル・32ボイスのポリフォニーを管理。NoteOn/NoteOff/PitchBendはメッセージキューに追加され、`Process()`呼び出し時に処理されるスレッドセーフな設計。

- **Sample**: 生のオーディオデータ構造体。int16_t配列（48kHz）とADSRエンベロープパラメータを保持。ループポイントはサンプル数で指定。

- **Timbre**: MappedSampleの集合。ノート番号とベロシティの範囲から適切なサンプルへのマッピングを定義（SoundFontのゾーンに相当）。

- **Channel**: MIDIチャンネルの抽象化。Timbreへの参照と発音中のノートを管理。

- **SamplePlayer**: 個々のボイスインスタンス。再生位置、ゲイン、ピッチを管理。

- **EffectReverb**: マスターエフェクトとしてのシュレーダーリバーブ実装。

### パフォーマンス最適化

サンプル処理のインナーループ（`sampler_process_inner`）には以下の2つの実装があります:
- C++実装（weak属性、非Xtensaプラットフォームで使用）
- Xtensaアセンブリによる手動最適化版 (`Sampler_asm.S`、ESP32で使用)

ESP32-S3ではfloat→int16変換時にSIMD命令を使用してさらなる高速化を実現。

### スレッドセーフティ

FreeRTOS環境ではメッセージキューにスピンロック、プレイヤー配列にセマフォを使用。非FreeRTOS環境（nativeビルド）では標準C++のmutexを使用。

## サンプルプロジェクト

`examples/music/`ディレクトリに完全なデモがあります:
- サンプル楽器（ピアノ、ベース、ドラム、supersaw、エレピ）
- MIDI形式のプリシーケンス楽曲
- M5Unifiedとの連携（ディスプレイ・オーディオ出力）
