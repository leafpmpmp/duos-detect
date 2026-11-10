/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Calibration data of the Raspberry Pi Camera Module 3 (IMX708), from the
 * Raspberry Pi tuning file src/ipa/rpi/pisp/data/imx708.json in libcamera.
 *
 * Copyright (C) 2019-2020 Raspberry Pi Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef DD_IMX708_TUNING_H
#define DD_IMX708_TUNING_H

/* Colour temperature curve: K, R/G and B/G of a grey surface (rpi.awb) */
static const double imx708_ct_curve[][3] = {
	{ 2964, 0.7451, 0.3213 },
	{ 3610, 0.6119, 0.4443 },
	{ 4640, 0.5168, 0.5419 },
	{ 5910, 0.4436, 0.6229 },
	{ 7590, 0.3847, 0.6921 },
};

/*
 * Search range of the "auto" mode, K, and the largest distance off the
 * curve. The Pi allows 0.018 (transverse_pos 0.01752, neg 0.01831). Grey
 * walls under office LED panels need 0.06.
 */
#define IMX708_CT_LO		2500
#define IMX708_CT_HI		7700
#define IMX708_TRANSVERSE	0.06

/* Colour matrices: K, then camera RGB to sRGB, row by row (rpi.ccm) */
static const double imx708_ccm[][10] = {
	{ 2964, 1.72129, -0.45961, -0.26169, -0.30042, 1.56924, -0.26882,
	  0.15133, -1.13293, 1.98161 },
	{ 3610, 1.54474, -0.35082, -0.19391, -0.36989, 1.67926, -0.30936,
	  -0.00524, -0.55197, 1.55722 },
	{ 4640, 1.52972, -0.35168, -0.17804, -0.28309, 1.67098, -0.38788,
	  0.01695, -0.57209, 1.55515 },
	{ 5910, 1.56879, -0.42159, -0.14719, -0.27275, 1.59354, -0.32079,
	  -0.02862, -0.40662, 1.43525 },
	{ 7590, 1.41424, -0.21092, -0.20332, -0.17646, 1.71734, -0.54087,
	  0.01297, -0.63111, 1.61814 },
};

/* "normal" exposure mode: exposure time (us) rises first, then gain */
static const double imx708_exposure_us[] = { 100, 10000, 30000, 50000, 66666 };
static const double imx708_exposure_gain[] = { 1.0, 1.5, 2.0, 4.0, 16.0 };

#endif
