/*
 * Copyright 2011-2016 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* Most of the code is based on the supplemental implementations from https://eheitzresearch.wordpress.com/240-2/. */

/* === GGX Microfacet distribution functions === */

/* Isotropic GGX microfacet distribution */
ccl_device_inline float D_ggx(float3 wm, float alpha)
{
	wm.z *= wm.z;
	alpha *= alpha;
	float tmp = (1.0f - wm.z) + alpha * wm.z;
	return alpha / max(M_PI_F * tmp*tmp, 1e-7f);
}

/* Anisotropic GGX microfacet distribution */
ccl_device_inline float D_ggx_aniso(const float3 wm, const float2 alpha)
{
	float slope_x = -wm.x/alpha.x;
	float slope_y = -wm.y/alpha.y;
	float tmp = wm.z*wm.z + slope_x*slope_x + slope_y*slope_y;

	return 1.0f / max(M_PI_F * tmp*tmp * alpha.x*alpha.y, 1e-7f);
}

/* Sample slope distribution (based on page 14 of the supplemental implementation). */
ccl_device_inline float2 mf_sampleP22_11(const float cosI, const float2 randU)
{
	if(cosI > 0.9999f || cosI < 1e-6f) {
		const float r = sqrtf(randU.x / (1.0f - randU.x));
		const float phi = M_2PI_F * randU.y;
		return make_float2(r*cosf(phi), r*sinf(phi));
	}

	const float sinI = sqrtf(1.0f - cosI*cosI);
	const float tanI = sinI/cosI;
	const float projA = 0.5f * (cosI + 1.0f);
	if(projA < 0.0001f)
		return make_float2(0.0f, 0.0f);
	const float A = 2.0f*randU.x*projA / cosI - 1.0f;
	float tmp = A*A-1.0f;
	if(fabsf(tmp) < 1e-7f)
		return make_float2(0.0f, 0.0f);
	tmp = 1.0f / tmp;
	const float D = safe_sqrtf(tanI*tanI*tmp*tmp - (A*A-tanI*tanI)*tmp);

	const float slopeX2 = tanI*tmp + D;
	const float slopeX = (A < 0.0f || slopeX2 > 1.0f/tanI)? (tanI*tmp - D) : slopeX2;

	float U2;
	if(randU.y >= 0.5f)
		U2 = 2.0f*(randU.y - 0.5f);
	else
		U2 = 2.0f*(0.5f - randU.y);
	const float z = (U2*(U2*(U2*0.27385f-0.73369f)+0.46341f)) / (U2*(U2*(U2*0.093073f+0.309420f)-1.0f)+0.597999f);
	const float slopeY = z * sqrtf(1.0f + slopeX*slopeX);

	if(randU.y >= 0.5f)
		return make_float2(slopeX, slopeY);
	else
		return make_float2(slopeX, -slopeY);
}

/* Visible normal sampling for the GGX distribution (based on page 7 of the supplemental implementation). */
ccl_device_inline float3 mf_sample_vndf(const float3 wi, const float2 alpha, const float2 randU)
{
	const float3 wi_11 = normalize(make_float3(alpha.x*wi.x, alpha.y*wi.y, wi.z));
	const float2 slope_11 = mf_sampleP22_11(wi_11.z, randU);

	const float2 cossin_phi = normalize(make_float2(wi_11.x, wi_11.y));
	const float slope_x = alpha.x*(cossin_phi.x * slope_11.x - cossin_phi.y * slope_11.y);
	const float slope_y = alpha.y*(cossin_phi.y * slope_11.x + cossin_phi.x * slope_11.y);

	kernel_assert(isfinite(slope_x));
	return normalize(make_float3(-slope_x, -slope_y, 1.0f));
}

/* === Phase functions: Glossy, Diffuse and Glass === */

/* Phase function for reflective materials, either without a fresnel term (for compatibility) or with the conductive fresnel term. */
ccl_device_inline float3 mf_sample_phase_glossy(const float3 wi, float3 *n, float3 *k, float3 *weight, const float3 wm)
{
	if(n && k)
		*weight *= fresnel_conductor(dot(wi, wm), *n, *k);

	return -wi + 2.0f * wm * dot(wi, wm);
}

ccl_device_inline float3 mf_eval_phase_glossy(const float3 w, const float lambda, const float3 wo, const float2 alpha, float3 *n, float3 *k)
{
	if(w.z > 0.9999f)
		return make_float3(0.0f, 0.0f, 0.0f);

	const float3 wh = normalize(wo - w);
	if(wh.z < 0.0f)
		return make_float3(0.0f, 0.0f, 0.0f);

	float pArea = (w.z < -0.9999f)? 1.0f: lambda*w.z;

	const float dotW_WH = dot(-w, wh);
	if(dotW_WH < 0.0f)
		return make_float3(0.0f, 0.0f, 0.0f);

	float phase = max(0.0f, dotW_WH) * 0.25f / max(pArea * dotW_WH, 1e-7f);
	if(alpha.x == alpha.y)
		phase *= D_ggx(wh, alpha.x);
	else
		phase *= D_ggx_aniso(wh, alpha);

	if(n && k) {
		/* Apply conductive fresnel term. */
		return phase * fresnel_conductor(dotW_WH, *n, *k);
	}

	return make_float3(phase, phase, phase);
}

/* Phase function for rough lambertian diffuse surfaces. */
ccl_device_inline float3 mf_sample_phase_diffuse(const float3 wm, const float randu, const float randv)
{
	float3 tm, bm;
	make_orthonormals(wm, &tm, &bm);

	float2 disk = concentric_sample_disk(randu, randv);
	return disk.x*tm + disk.y*bm + safe_sqrtf(1.0f - disk.x*disk.x - disk.y*disk.y)*wm;
}

ccl_device_inline float3 mf_eval_phase_diffuse(const float3 w, const float3 wm)
{
	const float v = max(0.0f, dot(w, wm)) * M_1_PI_F;
	return make_float3(v, v, v);
}

/* Phase function for dielectric transmissive materials, including both reflection and refraction according to the dielectric fresnel term. */
ccl_device_inline float3 mf_sample_phase_glass(const float3 wi, const float eta, const float3 wm, const float randV, bool *outside)
{
	float cosI = dot(wi, wm);
	float f = fresnel_dielectric_cos(cosI, eta);
	if(randV < f) {
		*outside = true;
		return -wi + 2.0f * wm * cosI;
	}
	*outside = false;
	float inv_eta = 1.0f/eta;
	float cosT = -safe_sqrtf(1.0f - (1.0f - cosI*cosI) * inv_eta*inv_eta);
	return normalize(wm*(cosI*inv_eta + cosT) - wi*inv_eta);
}

ccl_device_inline float3 mf_eval_phase_glass(const float3 w, const float lambda, const float3 wo, const bool wo_outside, const float2 alpha, const float eta)
{
	if(w.z > 0.9999f)
		return make_float3(0.0f, 0.0f, 0.0f);

	float pArea = (w.z < -0.9999f)? 1.0f: lambda*w.z;
	float v;
	if(wo_outside) {
		const float3 wh = normalize(wo - w);
		if(wh.z < 0.0f)
			return make_float3(0.0f, 0.0f, 0.0f);

		const float dotW_WH = dot(-w, wh);
		v = fresnel_dielectric_cos(dotW_WH, eta) * max(0.0f, dotW_WH) * D_ggx(wh, alpha.x) * 0.25f / (pArea * dotW_WH);
	}
	else {
		float3 wh = normalize(wo*eta - w);
		if(wh.z < 0.0f)
			wh = -wh;
		const float dotW_WH = dot(-w, wh), dotWO_WH = dot(wo, wh);
		if(dotW_WH < 0.0f)
			return make_float3(0.0f, 0.0f, 0.0f);

		float temp = dotW_WH + eta*dotWO_WH;
		v = (1.0f - fresnel_dielectric_cos(dotW_WH, eta)) * max(0.0f, dotW_WH) * max(0.0f, -dotWO_WH) * D_ggx(wh, alpha.x) / (pArea * temp * temp);
	}

	return make_float3(v, v, v);
}

/* === Utility functions for the random walks === */

/* Smith Lambda function for GGX (based on page 12 of the supplemental implementation). */
ccl_device_inline float mf_lambda(const float3 w, const float2 alpha)
{
	if(w.z > 0.9999f)
		return 0.0f;
	else if(w.z < -0.9999f)
		return -0.9999f;

	const float inv_wz2 = 1.0f / max(w.z*w.z, 1e-7f);
	const float2 wa = make_float2(w.x, w.y)*alpha;
	float v = sqrtf(1.0f + dot(wa, wa) * inv_wz2);
	if(w.z <= 0.0f)
		v = -v;

	return 0.5f*(v - 1.0f);
}

/* Height distribution CDF (based on page 4 of the supplemental implementation). */
ccl_device_inline float mf_invC1(const float h)
{
	return 2.0f * saturate(h) - 1.0f;
}

ccl_device_inline float mf_C1(const float h)
{
	return saturate(0.5f * (h + 1.0f));
}

/* Masking function (based on page 16 of the supplemental implementation). */
ccl_device_inline float mf_G1(const float3 w, const float C1, const float lambda)
{
	if(w.z > 0.9999f)
		return 1.0f;
	if(w.z < 1e-5f)
		return 0.0f;
	return powf(C1, lambda);
}

/* Sampling from the visible height distribution (based on page 17 of the supplemental implementation). */
ccl_device_inline bool mf_sample_height(const float3 w, float *h, float *C1, float *G1, float *lambda, const float U)
{
	if(w.z > 0.9999f)
		return false;
	if(w.z < -0.9999f) {
		*C1 *= U;
		*h = mf_invC1(*C1);
		*G1 = mf_G1(w, *C1, *lambda);
	}
	else if(fabsf(w.z) >= 0.0001f) {
		if(U > 1.0f - *G1)
			return false;
		if(*lambda >= 0.0f) {
			*C1 = 1.0f;
		}
		else {
			*C1 *= powf(1.0f-U, -1.0f / *lambda);
		}
		*h = mf_invC1(*C1);
		*G1 = mf_G1(w, *C1, *lambda);
	}
	return true;
}

/* === PDF approximations for the different phase functions. ===
 * As explained in bsdf_microfacet_multi_impl.h, using approximations with MIS still produces an unbiased result. */

/* Approximation for the albedo of the single-scattering GGX distribution,
 * the missing energy is then approximated as a diffuse reflection for the PDF. */
ccl_device_inline float mf_ggx_albedo(float r)
{
	float albedo = 0.806495f*expf(-1.98712f*r*r) + 0.199531f;
	albedo -= ((((((1.76741f*r - 8.43891f)*r + 15.784f)*r - 14.398f)*r + 6.45221f)*r - 1.19722f)*r + 0.027803f)*r + 0.00568739f;
	return saturate(albedo);
}

ccl_device_inline float mf_ggx_pdf(const float3 wi, const float3 wo, const float alpha)
{
	float D = D_ggx(normalize(wi+wo), alpha);
	float lambda = mf_lambda(wi, make_float2(alpha, alpha));
	float albedo = mf_ggx_albedo(alpha);
	return 0.25f * D / max((1.0f + lambda) * wi.z, 1e-7f) + (1.0f - albedo) * wo.z;
}

ccl_device_inline float mf_ggx_aniso_pdf(const float3 wi, const float3 wo, const float2 alpha)
{
	return 0.25f * D_ggx_aniso(normalize(wi+wo), alpha) / ((1.0f + mf_lambda(wi, alpha)) * wi.z) + (1.0f - mf_ggx_albedo(sqrtf(alpha.x*alpha.y))) * wo.z;
}

ccl_device_inline float mf_diffuse_pdf(const float3 wo)
{
	return M_1_PI_F * wo.z;
}

ccl_device_inline float mf_glass_pdf(const float3 wi, const float3 wo, const float alpha, const float eta)
{
	float3 wh;
	float fresnel;
	if(wi.z*wo.z > 0.0f) {
		wh = normalize(wi + wo);
		fresnel = fresnel_dielectric_cos(dot(wi, wh), eta);
	}
	else {
		wh = normalize(wi + wo*eta);
		fresnel = 1.0f - fresnel_dielectric_cos(dot(wi, wh), eta);
	}
	if(wh.z < 0.0f)
		wh = -wh;
	float3 r_wi = (wi.z < 0.0f)? -wi: wi;
	return fresnel * max(0.0f, dot(r_wi, wh)) * D_ggx(wh, alpha) / ((1.0f + mf_lambda(r_wi, make_float2(alpha, alpha))) * r_wi.z) + fabsf(wo.z);
}

/* === Actual random walk implementations, one version of mf_eval and mf_sample per phase function. === */

#define MF_NAME_JOIN(x,y) x ## _ ## y
#define MF_NAME_EVAL(x,y) MF_NAME_JOIN(x,y)
#define MF_FUNCTION_FULL_NAME(prefix) MF_NAME_EVAL(prefix, MF_PHASE_FUNCTION)

#define MF_PHASE_FUNCTION glass
#define MF_MULTI_GLASS
#include "bsdf_microfacet_multi_impl.h"

/* The diffuse phase function is not implemented as a node yet. */
#if 0
#define MF_PHASE_FUNCTION diffuse
#define MF_MULTI_DIFFUSE
#include "bsdf_microfacet_multi_impl.h"
#endif

#define MF_PHASE_FUNCTION glossy
#define MF_MULTI_GLOSSY
#include "bsdf_microfacet_multi_impl.h"

ccl_device void bsdf_microfacet_multi_ggx_blur(ShaderClosure *sc, float roughness)
{
	MicrofacetBsdf *bsdf = (MicrofacetBsdf*)sc;

	bsdf->alpha_x = fmaxf(roughness, bsdf->alpha_x);
	bsdf->alpha_y = fmaxf(roughness, bsdf->alpha_y);
}

/* === Closure implementations === */

/* Multiscattering GGX Glossy closure */

ccl_device int bsdf_microfacet_multi_ggx_common_setup(MicrofacetBsdf *bsdf)
{
	bsdf->alpha_x = clamp(bsdf->alpha_x, 1e-4f, 1.0f);
	bsdf->alpha_y = clamp(bsdf->alpha_y, 1e-4f, 1.0f);
	bsdf->extra->color.x = saturate(bsdf->extra->color.x);
	bsdf->extra->color.y = saturate(bsdf->extra->color.y);
	bsdf->extra->color.z = saturate(bsdf->extra->color.z);

	bsdf->type = CLOSURE_BSDF_MICROFACET_MULTI_GGX_ID;

	return SD_BSDF|SD_BSDF_HAS_EVAL|SD_BSDF_NEEDS_LCG;
}

ccl_device int bsdf_microfacet_multi_ggx_aniso_setup(MicrofacetBsdf *bsdf)
{
	if(is_zero(bsdf->T))
		bsdf->T = make_float3(1.0f, 0.0f, 0.0f);

	return bsdf_microfacet_multi_ggx_common_setup(bsdf);
}

ccl_device int bsdf_microfacet_multi_ggx_setup(MicrofacetBsdf *bsdf)
{
	bsdf->alpha_y = bsdf->alpha_x;

	return bsdf_microfacet_multi_ggx_common_setup(bsdf);
}

ccl_device float3 bsdf_microfacet_multi_ggx_eval_transmit(const ShaderClosure *sc, const float3 I, const float3 omega_in, float *pdf, ccl_addr_space uint *lcg_state) {
	*pdf = 0.0f;
	return make_float3(0.0f, 0.0f, 0.0f);
}

ccl_device float3 bsdf_microfacet_multi_ggx_eval_reflect(const ShaderClosure *sc, const float3 I, const float3 omega_in, float *pdf, ccl_addr_space uint *lcg_state) {
	const MicrofacetBsdf *bsdf = (const MicrofacetBsdf*)sc;
	bool is_aniso = (bsdf->alpha_x != bsdf->alpha_y);
	float3 X, Y, Z;
	Z = bsdf->N;
	if(is_aniso)
		make_orthonormals_tangent(Z, bsdf->T, &X, &Y);
	else
		make_orthonormals(Z, &X, &Y);

	float3 localI = make_float3(dot(I, X), dot(I, Y), dot(I, Z));
	float3 localO = make_float3(dot(omega_in, X), dot(omega_in, Y), dot(omega_in, Z));

	if(is_aniso)
		*pdf = mf_ggx_aniso_pdf(localI, localO, make_float2(bsdf->alpha_x, bsdf->alpha_y));
	else
		*pdf = mf_ggx_pdf(localI, localO, bsdf->alpha_x);
	return mf_eval_glossy(localI, localO, true, bsdf->extra->color, bsdf->alpha_x, bsdf->alpha_y, lcg_state, NULL, NULL);
}

ccl_device int bsdf_microfacet_multi_ggx_sample(KernelGlobals *kg, const ShaderClosure *sc, float3 Ng, float3 I, float3 dIdx, float3 dIdy, float randu, float randv, float3 *eval, float3 *omega_in, float3 *domega_in_dx, float3 *domega_in_dy, float *pdf, ccl_addr_space uint *lcg_state)
{
	const MicrofacetBsdf *bsdf = (const MicrofacetBsdf*)sc;
	bool is_aniso = (bsdf->alpha_x != bsdf->alpha_y);
	float3 X, Y, Z;
	Z = bsdf->N;
	if(is_aniso)
		make_orthonormals_tangent(Z, bsdf->T, &X, &Y);
	else
		make_orthonormals(Z, &X, &Y);

	float3 localI = make_float3(dot(I, X), dot(I, Y), dot(I, Z));
	float3 localO;

	*eval = mf_sample_glossy(localI, &localO, bsdf->extra->color, bsdf->alpha_x, bsdf->alpha_y, lcg_state, NULL, NULL);
	if(is_aniso)
		*pdf = mf_ggx_aniso_pdf(localI, localO, make_float2(bsdf->alpha_x, bsdf->alpha_y));
	else
		*pdf = mf_ggx_pdf(localI, localO, bsdf->alpha_x);
	*eval *= *pdf;

	*omega_in = X*localO.x + Y*localO.y + Z*localO.z;
#ifdef __RAY_DIFFERENTIALS__
	*domega_in_dx = (2 * dot(Z, dIdx)) * Z - dIdx;
	*domega_in_dy = (2 * dot(Z, dIdy)) * Z - dIdy;
#endif
	return LABEL_REFLECT|LABEL_GLOSSY;
}

/* Multiscattering GGX Glass closure */

ccl_device int bsdf_microfacet_multi_ggx_glass_setup(MicrofacetBsdf *bsdf)
{
	bsdf->alpha_x = clamp(bsdf->alpha_x, 1e-4f, 1.0f);
	bsdf->alpha_y = bsdf->alpha_x;
	bsdf->ior = max(0.0f, bsdf->ior);
	bsdf->extra->color.x = saturate(bsdf->extra->color.x);
	bsdf->extra->color.y = saturate(bsdf->extra->color.y);
	bsdf->extra->color.z = saturate(bsdf->extra->color.z);

	bsdf->type = CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID;

	return SD_BSDF|SD_BSDF_HAS_EVAL|SD_BSDF_NEEDS_LCG;
}

ccl_device float3 bsdf_microfacet_multi_ggx_glass_eval_transmit(const ShaderClosure *sc, const float3 I, const float3 omega_in, float *pdf, ccl_addr_space uint *lcg_state) {
	const MicrofacetBsdf *bsdf = (const MicrofacetBsdf*)sc;
	float3 X, Y, Z;
	Z = bsdf->N;
	make_orthonormals(Z, &X, &Y);

	float3 localI = make_float3(dot(I, X), dot(I, Y), dot(I, Z));
	float3 localO = make_float3(dot(omega_in, X), dot(omega_in, Y), dot(omega_in, Z));

	*pdf = mf_glass_pdf(localI, localO, bsdf->alpha_x, bsdf->ior);
	return mf_eval_glass(localI, localO, false, bsdf->extra->color, bsdf->alpha_x, bsdf->alpha_y, lcg_state, bsdf->ior);
}

ccl_device float3 bsdf_microfacet_multi_ggx_glass_eval_reflect(const ShaderClosure *sc, const float3 I, const float3 omega_in, float *pdf, ccl_addr_space uint *lcg_state) {
	const MicrofacetBsdf *bsdf = (const MicrofacetBsdf*)sc;
	float3 X, Y, Z;
	Z = bsdf->N;
	make_orthonormals(Z, &X, &Y);

	float3 localI = make_float3(dot(I, X), dot(I, Y), dot(I, Z));
	float3 localO = make_float3(dot(omega_in, X), dot(omega_in, Y), dot(omega_in, Z));

	*pdf = mf_glass_pdf(localI, localO, bsdf->alpha_x, bsdf->ior);
	return mf_eval_glass(localI, localO, true, bsdf->extra->color, bsdf->alpha_x, bsdf->alpha_y, lcg_state, bsdf->ior);
}

ccl_device int bsdf_microfacet_multi_ggx_glass_sample(KernelGlobals *kg, const ShaderClosure *sc, float3 Ng, float3 I, float3 dIdx, float3 dIdy, float randu, float randv, float3 *eval, float3 *omega_in, float3 *domega_in_dx, float3 *domega_in_dy, float *pdf, ccl_addr_space uint *lcg_state)
{
	const MicrofacetBsdf *bsdf = (const MicrofacetBsdf*)sc;
	float3 X, Y, Z;
	Z = bsdf->N;
	make_orthonormals(Z, &X, &Y);

	float3 localI = make_float3(dot(I, X), dot(I, Y), dot(I, Z));
	float3 localO;

	*eval = mf_sample_glass(localI, &localO, bsdf->extra->color, bsdf->alpha_x, bsdf->alpha_y, lcg_state, bsdf->ior);
	*pdf = mf_glass_pdf(localI, localO, bsdf->alpha_x, bsdf->ior);
	*eval *= *pdf;

	*omega_in = X*localO.x + Y*localO.y + Z*localO.z;
	if(localO.z*localI.z > 0.0f) {
#ifdef __RAY_DIFFERENTIALS__
		*domega_in_dx = (2 * dot(Z, dIdx)) * Z - dIdx;
		*domega_in_dy = (2 * dot(Z, dIdy)) * Z - dIdy;
#endif
		return LABEL_REFLECT|LABEL_GLOSSY;
	}
	else {
#ifdef __RAY_DIFFERENTIALS__
		float cosI = dot(Z, I);
		float dnp = max(sqrtf(1.0f - (bsdf->ior * bsdf->ior * (1.0f - cosI*cosI))), 1e-7f);
		*domega_in_dx = -(bsdf->ior * dIdx) + ((bsdf->ior - bsdf->ior * bsdf->ior * cosI / dnp) * dot(dIdx, Z)) * Z;
		*domega_in_dy = -(bsdf->ior * dIdy) + ((bsdf->ior - bsdf->ior * bsdf->ior * cosI / dnp) * dot(dIdy, Z)) * Z;
#endif

		return LABEL_TRANSMIT|LABEL_GLOSSY;
	}
}

CCL_NAMESPACE_END