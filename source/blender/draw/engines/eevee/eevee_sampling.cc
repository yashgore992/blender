/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Random number generator, contains persistent state and sample count logic.
 */

#include "BKE_colortools.hh"
#include "BKE_scene.hh"

#include "BLI_rand.h"

#include "BLI_math_base.hh"
#include "BLI_math_base_safe.h"

#include "eevee_instance.hh"
#include "eevee_sampling.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Sampling
 * \{ */

void Sampling::init(const Scene *scene)
{
  /* Note: Cycles have different option for view layers sample overrides. The current behavior
   * matches the default `Use`, which simply override if non-zero. */
  uint64_t render_sample_count = (inst_.view_layer->samples > 0) ? inst_.view_layer->samples :
                                                                   scene->eevee.taa_render_samples;

  sample_count_ = inst_.is_viewport() ? scene->eevee.taa_samples : render_sample_count;

  if (inst_.is_image_render) {
    sample_count_ = math::max(uint64_t(1), sample_count_);
  }

  if (sample_count_ == 0) {
    BLI_assert(inst_.is_viewport());
    sample_count_ = infinite_sample_count_;
  }

  if (inst_.is_viewport()) {
    /* We can't rely on the film module as it is initialized later. */
    int pixel_size = BKE_render_preview_pixel_size(&inst_.scene->r);
    if (pixel_size > 1) {
      /* Enforce to render at least all the film pixel once. */
      sample_count_ = max_ii(sample_count_, square_i(pixel_size));
    }
  }

  motion_blur_steps_ = !inst_.is_viewport() && ((scene->r.mode & R_MBLUR) != 0) ?
                           scene->eevee.motion_blur_steps :
                           1;
  sample_count_ = divide_ceil_u(sample_count_, motion_blur_steps_);

  if (scene->eevee.flag & SCE_EEVEE_DOF_JITTER) {
    if (sample_count_ == infinite_sample_count_) {
      /* Special case for viewport continuous rendering. We clamp to a max sample
       * to avoid the jittered dof never converging. */
      dof_ring_count_ = 6;
    }
    else {
      dof_ring_count_ = sampling_web_ring_count_get(dof_web_density_, sample_count_);
    }
    dof_sample_count_ = sampling_web_sample_count_get(dof_web_density_, dof_ring_count_);
    /* Change total sample count to fill the web pattern entirely. */
    sample_count_ = divide_ceil_u(sample_count_, dof_sample_count_) * dof_sample_count_;
  }
  else {
    dof_ring_count_ = 0;
    dof_sample_count_ = 1;
  }

  /* Only multiply after to have full the full DoF web pattern for each time steps. */
  sample_count_ *= motion_blur_steps_;

  auto clamp_value_load = [](float value) { return (value > 0.0) ? value : 1e20; };

  clamp_data_.sun_threshold = clamp_value_load(inst_.world.sun_threshold());
  clamp_data_.surface_direct = clamp_value_load(scene->eevee.clamp_surface_direct);
  clamp_data_.surface_indirect = clamp_value_load(scene->eevee.clamp_surface_indirect);
  clamp_data_.volume_direct = clamp_value_load(scene->eevee.clamp_volume_direct);
  clamp_data_.volume_indirect = clamp_value_load(scene->eevee.clamp_volume_indirect);
}

void Sampling::init(const Object &probe_object)
{
  BLI_assert(inst_.is_baking());
  const ::LightProbe &lightprobe = DRW_object_get_data_for_drawing<::LightProbe>(probe_object);

  sample_count_ = max_ii(1, lightprobe.grid_bake_samples);
  sample_ = 0;
}

void Sampling::end_sync()
{
  if (reset_) {
    viewport_sample_ = 0;
  }

  if (inst_.is_viewport()) {

    interactive_mode_ = viewport_sample_ < interactive_mode_threshold;

    bool interactive_mode_disabled = (inst_.scene->eevee.flag & SCE_EEVEE_TAA_REPROJECTION) == 0 ||
                                     inst_.is_viewport_image_render;
    if (interactive_mode_disabled) {
      interactive_mode_ = false;
      sample_ = viewport_sample_;
    }
    else if (interactive_mode_) {
      int interactive_sample_count = interactive_sample_max_;

      if (viewport_sample_ < interactive_sample_count) {
        /* Loop over the same starting samples. */
        sample_ = sample_ % interactive_sample_count;
      }
      else {
        /* Break out of the loop and resume normal pattern. */
        sample_ = interactive_sample_count;
      }
    }
  }
}

void Sampling::step()
{
  {
    /* Repeat the sequence for all pixels that are being up-scaled. */
    uint64_t sample_filter = sample_ / square_i(inst_.film.scaling_factor_get());
    if (interactive_mode()) {
      sample_filter = sample_filter % interactive_sample_aa_;
    }
    /* TODO(fclem) we could use some persistent states to speedup the computation. */
    double2 r, offset = {0, 0};
    /* Using 2,3 primes as per UE4 Temporal AA presentation.
     * http://advances.realtimerendering.com/s2014/epic/TemporalAA.pptx (slide 14) */
    uint2 primes = {2, 3};
    BLI_halton_2d(primes, offset, sample_filter + 1, r);
    /* WORKAROUND: We offset the distribution to make the first sample (0,0). This way, we are
     * assured that at least one of the samples inside the TAA rotation will match the one from the
     * draw manager. This makes sure overlays are correctly composited in static scene. */
    data_.dimensions[SAMPLING_FILTER_U] = fractf(r[0] + (1.0 / 2.0));
    data_.dimensions[SAMPLING_FILTER_V] = fractf(r[1] + (2.0 / 3.0));
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_TIME] = r[0];
    data_.dimensions[SAMPLING_CLOSURE] = r[1];
    data_.dimensions[SAMPLING_RAYTRACE_X] = r[0];
  }
  {
    double3 r, offset = {0, 0, 0};
    uint3 primes = {5, 7, 3};
    BLI_halton_3d(primes, offset, sample_ + 1, r);
    data_.dimensions[SAMPLING_LENS_U] = r[0];
    data_.dimensions[SAMPLING_LENS_V] = r[1];
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_LIGHTPROBE] = r[0];
    data_.dimensions[SAMPLING_TRANSPARENCY] = r[1];
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_AO_U] = r[0];
    data_.dimensions[SAMPLING_AO_V] = r[1];
    data_.dimensions[SAMPLING_AO_W] = r[2];
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_CURVES_U] = r[0];
  }
  {
    uint64_t sample_raytrace = sample_;
    if (interactive_mode()) {
      sample_raytrace = sample_raytrace % interactive_sample_raytrace_;
    }
    /* Using leaped Halton sequence so we can reused the same primes as lens. */
    double3 r, offset = {0, 0, 0};
    uint64_t leap = 13;
    uint3 primes = {5, 7, 11};
    BLI_halton_3d(primes, offset, sample_raytrace * leap + 1, r);
    data_.dimensions[SAMPLING_SHADOW_U] = r[0];
    data_.dimensions[SAMPLING_SHADOW_V] = r[1];
    data_.dimensions[SAMPLING_SHADOW_W] = r[2];
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_RAYTRACE_U] = r[0];
    data_.dimensions[SAMPLING_RAYTRACE_V] = r[1];
    data_.dimensions[SAMPLING_RAYTRACE_W] = r[2];
  }
  {
    double3 r, offset = {0, 0, 0};
    uint3 primes = {2, 3, 5};
    BLI_halton_3d(primes, offset, sample_ + 1, r);
    /* WORKAROUND: We offset the distribution to make the first sample (0,0,0). */
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_SHADOW_I] = fractf(r[0] + (1.0 / 2.0));
    data_.dimensions[SAMPLING_SHADOW_J] = fractf(r[1] + (2.0 / 3.0));
    data_.dimensions[SAMPLING_SHADOW_K] = fractf(r[2] + (4.0 / 5.0));
  }
  {
    uint64_t sample_volume = sample_;
    if (interactive_mode()) {
      sample_volume = sample_volume % interactive_sample_volume_;
    }
    double3 r, offset = {0, 0, 0};
    uint3 primes = {2, 3, 5};
    BLI_halton_3d(primes, offset, sample_volume + 1, r);
    /* WORKAROUND: We offset the distribution to make the first sample (0,0,0). */
    data_.dimensions[SAMPLING_VOLUME_U] = fractf(r[0] + (1.0 / 2.0));
    data_.dimensions[SAMPLING_VOLUME_V] = fractf(r[1] + (2.0 / 3.0));
    data_.dimensions[SAMPLING_VOLUME_W] = fractf(r[2] + (4.0 / 5.0));
  }
  {
    /* Using leaped Halton sequence so we can reused the same primes. */
    double2 r, offset = {0, 0};
    uint64_t leap = 5;
    uint2 primes = {2, 3};
    BLI_halton_2d(primes, offset, sample_ * leap + 1, r);
    data_.dimensions[SAMPLING_SHADOW_X] = r[0];
    data_.dimensions[SAMPLING_SHADOW_Y] = r[1];
    /* TODO de-correlate. */
    data_.dimensions[SAMPLING_SSS_U] = r[0];
    data_.dimensions[SAMPLING_SSS_V] = r[1];
  }
  {
    /* Don't leave unused data undefined. */
    data_.dimensions[SAMPLING_UNUSED_0] = 0.0f;
    data_.dimensions[SAMPLING_UNUSED_1] = 0.0f;
    data_.dimensions[SAMPLING_UNUSED_2] = 0.0f;
  }

  for (int i : IndexRange(SAMPLING_DIMENSION_COUNT)) {
    /* These numbers are often fed to `sqrt`. Make sure their values are in the expected range. */
    BLI_assert(data_.dimensions[i] >= 0.0f);
    BLI_assert(data_.dimensions[i] < 1.0f);
    UNUSED_VARS_NDEBUG(i);
  }

  data_.push_update();

  viewport_sample_++;
  sample_++;

  reset_ = false;
}

void Sampling::reset()
{
  BLI_assert(inst_.is_viewport());
  reset_ = true;
}

bool Sampling::is_reset() const
{
  BLI_assert(inst_.is_viewport());
  return reset_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling patterns
 * \{ */

float3 Sampling::sample_ball(const float3 &rand)
{
  float3 sample;
  sample.z = rand.x * 2.0f - 1.0f; /* cos theta */

  float r = sqrtf(fmaxf(0.0f, 1.0f - square_f(sample.z))); /* sin theta */

  float omega = rand.y * 2.0f * M_PI;
  sample.x = r * cosf(omega);
  sample.y = r * sinf(omega);

  sample *= sqrtf(sqrtf(rand.z));
  return sample;
}

float2 Sampling::sample_disk(const float2 &rand)
{
  float omega = rand.y * 2.0f * M_PI;
  return sqrtf(rand.x) * float2(cosf(omega), sinf(omega));
}

float3 Sampling::sample_hemisphere(const float2 &rand)
{
  const float omega = rand.y * 2.0f * M_PI;
  const float cos_theta = rand.x;
  const float sin_theta = safe_sqrtf(1.0f - square_f(cos_theta));
  return float3(sin_theta * float2(cosf(omega), sinf(omega)), cos_theta);
}

float3 Sampling::sample_sphere(const float2 &rand)
{
  const float omega = rand.y * 2.0f * M_PI;
  const float cos_theta = rand.x * 2.0f - 1.0f;
  const float sin_theta = safe_sqrtf(1.0f - square_f(cos_theta));
  return float3(sin_theta * float2(cosf(omega), sinf(omega)), cos_theta);
}

float2 Sampling::sample_spiral(const float2 &rand)
{
  /* Fibonacci spiral. */
  float omega = 4.0f * M_PI * (1.0f + sqrtf(5.0f)) * rand.x;
  float r = sqrtf(rand.x);
  /* Random rotation. */
  omega += rand.y * 2.0f * M_PI;
  return r * float2(cosf(omega), sinf(omega));
}

void Sampling::dof_disk_sample_get(float *r_radius, float *r_theta) const
{
  if (dof_ring_count_ == 0) {
    *r_radius = *r_theta = 0.0f;
    return;
  }

  int s = sample_ - 1;
  int ring = 0;
  int ring_sample_count = 1;
  int ring_sample = 1;

  s = s * (dof_web_density_ - 1);
  s = s % dof_sample_count_;

  /* Choosing sample to we get faster convergence.
   * The issue here is that we cannot map a low discrepancy sequence to this sampling pattern
   * because the same sample could be chosen twice in relatively short intervals. */
  /* For now just use an ascending sequence with an offset. This gives us relatively quick
   * initial coverage and relatively high distance between samples. */
  /* TODO(@fclem) We can try to order samples based on a LDS into a table to avoid duplicates.
   * The drawback would be some memory consumption and initialize time. */
  int samples_passed = 1;
  while (s >= samples_passed) {
    ring++;
    ring_sample_count = ring * dof_web_density_;
    ring_sample = s - samples_passed;
    ring_sample = (ring_sample + 1) % ring_sample_count;
    samples_passed += ring_sample_count;
  }

  *r_radius = ring / float(dof_ring_count_);
  *r_theta = 2.0f * M_PI * ring_sample / float(ring_sample_count);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cumulative Distribution Function (CDF)
 * \{ */

void Sampling::cdf_from_curvemapping(const CurveMapping &curve, Vector<float> &cdf)
{
  BLI_assert(cdf.size() > 1);
  cdf[0] = 0.0f;
  /* Actual CDF evaluation. */
  for (int u : IndexRange(cdf.size() - 1)) {
    float x = float(u + 1) / float(cdf.size() - 1);
    cdf[u + 1] = cdf[u] + BKE_curvemapping_evaluateF(&curve, 0, x);
  }
  /* Normalize the CDF. */
  for (int u : cdf.index_range()) {
    cdf[u] /= cdf.last();
  }
  /* Just to make sure. */
  cdf.last() = 1.0f;
}

void Sampling::cdf_invert(Vector<float> &cdf, Vector<float> &inverted_cdf)
{
  BLI_assert(cdf.first() == 0.0f && cdf.last() == 1.0f);
  for (int u : inverted_cdf.index_range()) {
    float x = clamp_f(u / float(inverted_cdf.size() - 1), 1e-5f, 1.0f - 1e-5f);
    for (int i : cdf.index_range().drop_front(1)) {
      if (cdf[i] >= x) {
        float t = (x - cdf[i]) / (cdf[i] - cdf[i - 1]);
        inverted_cdf[u] = (float(i) + t) / float(cdf.size() - 1);
        break;
      }
    }
  }
}

/** \} */

}  // namespace blender::eevee
