/* P11-C5: mathematical oracle for every Porter-Duff blend factor exported to Vulkan and GLES.
 * Sample pixels are premultiplied: RGB values do not exceed their own alpha. An external
 * AHB cannot make an RGBX alpha channel meaningful; txrendergpu tests that CPU fallback. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "gpucopytypes_src.inc"

static int failures;
static float factor(uint8_t which, float srcAlpha, float dstAlpha) {
    switch (which) {
        case LORIE_FACTOR_ZERO:                return 0;
        case LORIE_FACTOR_ONE:                 return 1;
        case LORIE_FACTOR_SRC_ALPHA:           return srcAlpha;
        case LORIE_FACTOR_ONE_MINUS_SRC_ALPHA: return 1 - srcAlpha;
        case LORIE_FACTOR_DST_ALPHA:           return dstAlpha;
        case LORIE_FACTOR_ONE_MINUS_DST_ALPHA: return 1 - dstAlpha;
        default: failures++; return 0;
    }
}

static float expected(uint8_t op, float cs, float cd, float as, float ad) {
    switch (op) {
        case LORIE_GPU_COPY_OP_SRC:          return cs;
        case LORIE_GPU_COPY_OP_OVER:         return cs + cd * (1 - as);
        case LORIE_GPU_COPY_OP_CLEAR:        return 0;
        case LORIE_GPU_COPY_OP_OVER_REVERSE: return cs * (1 - ad) + cd;
        case LORIE_GPU_COPY_OP_IN:           return cs * ad;
        case LORIE_GPU_COPY_OP_IN_REVERSE:   return cd * as;
        case LORIE_GPU_COPY_OP_OUT:          return cs * (1 - ad);
        case LORIE_GPU_COPY_OP_OUT_REVERSE:  return cd * (1 - as);
        case LORIE_GPU_COPY_OP_ATOP:         return cs * ad + cd * (1 - as);
        case LORIE_GPU_COPY_OP_ATOP_REVERSE: return cs * (1 - ad) + cd * as;
        case LORIE_GPU_COPY_OP_XOR:          return cs * (1 - ad) + cd * (1 - as);
        case LORIE_GPU_COPY_OP_ADD:          return fminf(1, cs + cd);
        default: failures++; return -1;
    }
}

int main(void) {
    unsigned samples = 0;
    for (uint8_t op = 0; op < LORIE_GPU_COPY_OP_COUNT; op++) {
        uint8_t sf = 255, df = 255;
        if (!lorieGpuCompositeFactors(op, &sf, &df)) {
            failures++; printf("  FAIL unsupported Porter-Duff op=%u\n", op); continue;
        }
        for (int sa = 0; sa <= 10; sa++)
            for (int da = 0; da <= 10; da++) {
                float as = (float) sa / 10, ad = (float) da / 10;
                for (int sc = 0; sc <= 3; sc++)
                    for (int dc = 0; dc <= 3; dc++) {
                        float cs = as * (float) sc / 3, cd = ad * (float) dc / 3;
                        float got = fminf(1, cs * factor(sf, as, ad) + cd * factor(df, as, ad));
                        float want = expected(op, cs, cd, as, ad);
                        if (fabsf(got - want) > 1e-5f) {
                            failures++;
                            if (failures < 5)
                                printf("  FAIL op=%u As=%g Ad=%g Cs=%g Cd=%g got=%g expected=%g\n",
                                       op, as, ad, cs, cd, got, want);
                        }
                        samples++;
                    }
            }
    }
    { uint8_t a, b;
      if (lorieGpuCompositeFactors(255, &a, &b) || lorieGpuCompositeFactors(LORIE_GPU_COPY_OP_COUNT, &a, &b))
          failures++;
    }
    printf("TPORTERDUFF 12 premultiplied blend ops × %u samples: %s (%d failures)\n",
           samples, failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
