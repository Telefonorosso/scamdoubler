/*
 * Minimal libyuv AArch64/NEON RGB565 -> I420 worker for Emu68 POC43.
 *
 * The RGB565 expansion, Y row and UV row assembler below are copied from
 * upstream libyuv source/row_neon64.cc (BSD-style license).  Their arithmetic
 * is unchanged.  Emu68 is AArch64 big-endian, so each loaded RGB565 vector is
 * byte-swapped with REV16 before entering the upstream little-endian kernel.  This file deliberately does NOT include Emu68 support.h /
 * RegLock.h, because upstream libyuv uses v20/v21 as temporaries while Emu68
 * reserves q19/q20/q21 in its normal translation units.
 *
 * The exported wrapper saves/restores q19-q21 across the whole conversion.
 */
#include <stdint.h>

#define RGB565TOARGB                                                        \
  "shrn       v1.8b, v0.8h, #3               \n"                      \
  "shrn2      v1.16b, v4.8h, #3              \n"                      \
  "uzp2       v2.16b, v0.16b, v4.16b         \n"                      \
  "uzp1       v0.16b, v0.16b, v4.16b         \n"                      \
  "sri        v1.16b, v1.16b, #6             \n"                      \
  "shl        v0.16b, v0.16b, #3             \n"                      \
  "sri        v2.16b, v2.16b, #5             \n"                      \
  "sri        v0.16b, v0.16b, #5             \n"

#define RGBTOUV_SETUP_REG                                                   \
  "movi       v20.8h, #112          \n"                                  \
  "movi       v21.8h, #74           \n"                                  \
  "movi       v22.8h, #38           \n"                                  \
  "movi       v23.8h, #18           \n"                                  \
  "movi       v24.8h, #94           \n"                                  \
  "movi       v25.8h, #0x80, lsl #8 \n"

#define RGBTOUV(QB, QG, QR)                                                  \
  "mul        v3.8h, " #QB ",v20.8h          \n"                         \
  "mul        v4.8h, " #QR ",v20.8h          \n"                         \
  "mls        v3.8h, " #QG ",v21.8h          \n"                         \
  "mls        v4.8h, " #QG ",v24.8h          \n"                         \
  "mls        v3.8h, " #QR ",v22.8h          \n"                         \
  "mls        v4.8h, " #QB ",v23.8h          \n"                         \
  "addhn      v0.8b, v3.8h, v25.8h           \n"                         \
  "addhn      v1.8b, v4.8h, v25.8h           \n"

/* Upstream libyuv RGB565ToUVRow_NEON, local symbol only. */
static void RGB565ToUVRow_NEON(const uint8_t* src_rgb565,
                               int src_stride_rgb565,
                               uint8_t* dst_u,
                               uint8_t* dst_v,
                               int width) {
  const uint8_t* src_rgb565_1 = src_rgb565 + src_stride_rgb565;
  asm volatile(
      RGBTOUV_SETUP_REG
      "1:          \n"
      "ldp         q0, q4, [%0], #32             \n"
      "rev16       v0.16b, v0.16b                   \n"
      "rev16       v4.16b, v4.16b                   \n"
      "subs        %w4, %w4, #16                 \n"
      RGB565TOARGB
      "uaddlp      v16.8h, v0.16b                \n"
      "prfm        pldl1keep, [%0, 448]          \n"
      "uaddlp      v17.8h, v1.16b                \n"
      "uaddlp      v18.8h, v2.16b                \n"
      "ldp         q0, q4, [%1], #32             \n"
      "rev16       v0.16b, v0.16b                   \n"
      "rev16       v4.16b, v4.16b                   \n"
      RGB565TOARGB
      "uadalp      v16.8h, v0.16b                \n"
      "prfm        pldl1keep, [%1, 448]          \n"
      "uadalp      v17.8h, v1.16b                \n"
      "uadalp      v18.8h, v2.16b                \n"
      "urshr       v0.8h, v16.8h, #2             \n"
      "urshr       v1.8h, v17.8h, #2             \n"
      "urshr       v2.8h, v18.8h, #2             \n"
      RGBTOUV(v0.8h, v1.8h, v2.8h)
      "st1         {v0.8b}, [%2], #8             \n"
      "st1         {v1.8b}, [%3], #8             \n"
      "b.gt        1b                            \n"
      : "+r"(src_rgb565), "+r"(src_rgb565_1), "+r"(dst_u), "+r"(dst_v),
        "+r"(width)
      :
      : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v16", "v17",
        "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27",
        "v28");
}

/* Upstream libyuv RGB565ToYRow_NEON, local symbol only. */
static void RGB565ToYRow_NEON(const uint8_t* src_rgb565,
                              uint8_t* dst_y,
                              int width) {
  asm volatile(
      "movi        v24.16b, #25                  \n"
      "movi        v25.16b, #129                 \n"
      "movi        v26.16b, #66                  \n"
      "movi        v27.16b, #16                  \n"
      "1:          \n"
      "ldp         q0, q4, [%0], #32             \n"
      "rev16       v0.16b, v0.16b                   \n"
      "rev16       v4.16b, v4.16b                   \n"
      "subs        %w2, %w2, #16                 \n"
      RGB565TOARGB
      "umull       v3.8h, v0.8b, v24.8b          \n"
      "umull2      v4.8h, v0.16b, v24.16b        \n"
      "prfm        pldl1keep, [%0, 448]          \n"
      "umlal       v3.8h, v1.8b, v25.8b          \n"
      "umlal2      v4.8h, v1.16b, v25.16b        \n"
      "umlal       v3.8h, v2.8b, v26.8b          \n"
      "umlal2      v4.8h, v2.16b, v26.16b        \n"
      "uqrshrn     v0.8b, v3.8h, #8              \n"
      "uqrshrn     v1.8b, v4.8h, #8              \n"
      "uqadd       v0.8b, v0.8b, v27.8b          \n"
      "uqadd       v1.8b, v1.8b, v27.8b          \n"
      "stp         d0, d1, [%1], #16             \n"
      "b.gt        1b                            \n"
      : "+r"(src_rgb565), "+r"(dst_y), "+r"(width)
      :
      : "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v6", "v24", "v25", "v26",
        "v27");
}

void poc43_libyuv_rgb565_to_i420_neon(const uint8_t *src_rgb565,
                                       int src_stride_rgb565,
                                       uint8_t *dst_i420,
                                       int width,
                                       int height)
{
    /* Preserve Emu68's globally reserved SIMD registers on CPU1. */
    __uint128_t save19, save20, save21;
    asm volatile("str q19, %0\nstr q20, %1\nstr q21, %2"
                 : "=m"(save19), "=m"(save20), "=m"(save21) :: "memory");

    uint8_t *dst_y = dst_i420;
    uint8_t *dst_u = dst_y + (uint32_t)width * (uint32_t)height;
    uint8_t *dst_v = dst_u + ((uint32_t)width * (uint32_t)height) / 4U;
    int uv_stride = width / 2;

    for (int y = 0; y < height; y += 2) {
        RGB565ToUVRow_NEON(src_rgb565, src_stride_rgb565, dst_u, dst_v, width);
        RGB565ToYRow_NEON(src_rgb565, dst_y, width);
        RGB565ToYRow_NEON(src_rgb565 + src_stride_rgb565, dst_y + width, width);
        src_rgb565 += src_stride_rgb565 * 2;
        dst_y += width * 2;
        dst_u += uv_stride;
        dst_v += uv_stride;
    }

    asm volatile("ldr q19, %0\nldr q20, %1\nldr q21, %2"
                 :: "m"(save19), "m"(save20), "m"(save21) : "memory");
}
