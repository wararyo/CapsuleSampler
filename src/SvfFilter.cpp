#include "SvfFilter.h"
#include "Tables.h"

namespace capsule
{
namespace sampler
{

float tanf_lut_cent(float cent)
{
    float idx_f = (cent - TANF_CENT_MIN) * TANF_CENT_INV_STEP;
    if (idx_f <= 0.0f) return tanfTable[0];
    if (idx_f >= (float)(TANF_LUT_SIZE - 1)) return tanfTable[TANF_LUT_SIZE - 1];
    int i = (int)idx_f;
    float frac = idx_f - (float)i;
    float a = tanfTable[i];
    float b = tanfTable[i + 1];
    return a + (b - a) * frac;
}

void svf_setup_from_cent(svf_coeffs_t *c, float cutoffCent, float Q)
{
    if (Q < 0.5f) Q = 0.5f;
    float g = tanf_lut_cent(cutoffCent);
    float k = 1.0f / Q;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    c->a1 = a1;
    c->a2 = a2;
    c->a3 = a3;
    c->k  = k;
}

__attribute((hot, optimize("-O3")))
void svf_process_lp_block(const float *in, float *out, uint32_t len,
                          float *ic1eq, float *ic2eq,
                          const svf_coeffs_t *c)
{
    float ic1 = *ic1eq;
    float ic2 = *ic2eq;
    const float a1 = c->a1;
    const float a2 = c->a2;
    const float a3 = c->a3;
    do
    {
        float v0 = *in++;
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        *out++ = v2;
    } while (--len);
    *ic1eq = ic1;
    *ic2eq = ic2;
}

}
}
