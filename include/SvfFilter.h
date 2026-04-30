#pragma once

#include <cstdint>

namespace capsule
{
namespace sampler
{

// TPT/ZDF State Variable Filter (Andrew Simper 形式) のローパス実装
// 参考: https://cytomic.com/files/dsp/SvfLinearTrapezoidalSin.pdf

// 64サンプル毎に再計算する係数キャッシュ
struct svf_coeffs_t
{
    float a1; // 1 / (1 + g*(g + k))
    float a2; // g * a1
    float a3; // g * a2
    float k;  // 1 / Q
};

// SoundFont 2 absolute cent (1200·log2(fc/8.176Hz)) と Q から係数を計算する
// SAMPLE_RATE = 48000 固定前提（tanfTable がそれを内包している）
void svf_setup_from_cent(svf_coeffs_t *c, float cutoffCent, float Q);

// LPF の inner loop（in/out 同一ポインタ可）
// ic1eq, ic2eq は SamplePlayer が保持する積分器メモリ
void svf_process_lp_block(const float *in, float *out, uint32_t len,
                          float *ic1eq, float *ic2eq,
                          const svf_coeffs_t *c);

// cent → tan(π·fc/48000) の LUT 引き（線形補間）
// 範囲外はテーブルの両端値で頭打ち
float tanf_lut_cent(float cent);

}
}
