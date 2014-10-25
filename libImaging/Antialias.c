/*
 * The Python Imaging Library
 * $Id$
 *
 * pilopen antialiasing support
 *
 * history:
 * 2002-03-09 fl  Created (for PIL 1.1.3)
 * 2002-03-10 fl  Added support for mode "F"
 *
 * Copyright (c) 1997-2002 by Secret Labs AB
 *
 * See the README file for information on usage and redistribution.
 */

#include "Imaging.h"

#include <math.h>

/* resampling filters (from antialias.py) */

struct filter {
    float (*filter)(float x);
    float support;
};

static inline float sinc_filter(float x)
{
    if (x == 0.0)
        return 1.0;
    x = x * M_PI;
    return sin(x) / x;
}

static inline float antialias_filter(float x)
{
    /* lanczos (truncated sinc) */
    if (-3.0 <= x && x < 3.0)
        return sinc_filter(x) * sinc_filter(x/3);
    return 0.0;
}

static struct filter ANTIALIAS = { antialias_filter, 3.0 };

static inline float nearest_filter(float x)
{
    if (-0.5 <= x && x < 0.5)
        return 1.0;
    return 0.0;
}

static struct filter NEAREST = { nearest_filter, 0.5 };

static inline float bilinear_filter(float x)
{
    if (x < 0.0)
        x = -x;
    if (x < 1.0)
        return 1.0-x;
    return 0.0;
}

static struct filter BILINEAR = { bilinear_filter, 1.0 };

static inline float bicubic_filter(float x)
{
    /* http://en.wikipedia.org/wiki/Bicubic_interpolation#Bicubic_convolution_algorithm */
#define a -0.5
    if (x < 0.0)
        x = -x;
    if (x < 1.0)
        return ((a + 2.0) * x - (a + 3.0)) * x*x + 1;
    if (x < 2.0)
        return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
#undef a
}

static struct filter BICUBIC = { bicubic_filter, 2.0 };

Imaging
ImagingStretchHorizaontal(Imaging imOut, Imaging imIn, int filter)
{
    /* FIXME: this is a quick and straightforward translation from a
       python prototype.  might need some further C-ification... */

    ImagingSectionCookie cookie;
    struct filter *filterp;
    float support, scale, filterscale;
    float center, ww, ss, xmin, xmax;
    int xx, yy, x, b, kmax;
    float *k, *kk, *xbounds;

    /* check modes */
    if (!imOut || !imIn || strcmp(imIn->mode, imOut->mode) != 0)
        return (Imaging) ImagingError_ModeError();

    if (imOut->ysize != imIn->ysize)
        return (Imaging) ImagingError_ValueError(
            "ImagingStretchHorizaontal requires equal heights"
        );

    /* check filter */
    switch (filter) {
    case IMAGING_TRANSFORM_NEAREST:
        filterp = &NEAREST;
        break;
    case IMAGING_TRANSFORM_ANTIALIAS:
        filterp = &ANTIALIAS;
        break;
    case IMAGING_TRANSFORM_BILINEAR:
        filterp = &BILINEAR;
        break;
    case IMAGING_TRANSFORM_BICUBIC:
        filterp = &BICUBIC;
        break;
    default:
        return (Imaging) ImagingError_ValueError(
            "unsupported resampling filter"
            );
    }

    /* prepare for horizontal stretch */
    filterscale = scale = (float) imIn->xsize / imOut->xsize;

    /* determine support size (length of resampling filter) */
    support = filterp->support;

    if (filterscale < 1.0) {
        filterscale = 1.0;
    }

    support = support * filterscale;

    /* maximum number of coofs */
    kmax = (int) ceil(support) * 2 + 1;

    /* coefficient buffer (with rounding safety margin) */
    kk = malloc(imOut->xsize * kmax * sizeof(float));
    if ( ! kk)
        return (Imaging) ImagingError_MemoryError();

    xbounds = malloc(imOut->xsize * 3 * sizeof(float));
    if ( ! xbounds) {
        free(kk);
        return (Imaging) ImagingError_MemoryError();
    }

    for (xx = 0; xx < imOut->xsize; xx++) {
        k = &kk[xx * kmax];
        center = (xx + 0.5) * scale;
        ww = 0.0;
        ss = 1.0 / filterscale;
        xmin = floor(center - support);
        if (xmin < 0.0)
            xmin = 0.0;
        xmax = ceil(center + support);
        if (xmax > (float) imIn->xsize)
            xmax = (float) imIn->xsize;
        for (x = (int) xmin; x < (int) xmax; x++) {
            float w = filterp->filter((x - center + 0.5) * ss) * ss;
            k[x - (int) xmin] = w;
            ww = ww + w;
        }
        if (ww == 0.0)
            ww = 1.0;
        else
            ww = 1.0 / ww;
        xbounds[xx * 3 + 0] = xmin;
        xbounds[xx * 3 + 1] = xmax;
        xbounds[xx * 3 + 2] = ww;
    }

    ImagingSectionEnter(&cookie);
    /* horizontal stretch */
    for (xx = 0; xx < imOut->xsize; xx++) {
        center = (xx + 0.5) * scale;
        ww = 0.0;
        ss = 1.0 / filterscale;
        xmin = floor(center - support);
        if (xmin < 0.0)
            xmin = 0.0;
        xmax = ceil(center + support);
        if (xmax > (float) imIn->xsize)
            xmax = (float) imIn->xsize;
        for (x = (int) xmin; x < (int) xmax; x++) {
            float w = filterp->filter((x - center + 0.5) * ss) * ss;
            k[x - (int) xmin] = w;
            ww = ww + w;
        }
        if (ww == 0.0)
            ww = 1.0;
        else
            ww = 1.0 / ww;
        if (imIn->image8) {
            /* 8-bit grayscale */
            for (yy = 0; yy < imOut->ysize; yy++) {
                ss = 0.0;
                for (x = (int) xmin; x < (int) xmax; x++)
                    ss = ss + imIn->image8[yy][x] * k[x - (int) xmin];
                ss = ss * ww + 0.5;
                if (ss < 0.5)
                    imOut->image8[yy][xx] = (UINT8) 0;
                else if (ss >= 255.0)
                    imOut->image8[yy][xx] = (UINT8) 255;
                else
                    imOut->image8[yy][xx] = (UINT8) ss;
            }
        } else
            switch(imIn->type) {
            case IMAGING_TYPE_UINT8:
                /* n-bit grayscale */
                for (yy = 0; yy < imOut->ysize; yy++) {
                    for (b = 0; b < imIn->bands; b++) {
                        if (imIn->bands == 2 && b)
                            b = 3; /* hack to deal with LA images */
                        ss = 0.0;
                        for (x = (int) xmin; x < (int) xmax; x++)
                            ss = ss + (UINT8) imIn->image[yy][x*4+b] * k[x - (int) xmin];
                        ss = ss * ww + 0.5;
                        if (ss < 0.5)
                            imOut->image[yy][xx*4+b] = (UINT8) 0;
                        else if (ss >= 255.0)
                            imOut->image[yy][xx*4+b] = (UINT8) 255;
                        else
                            imOut->image[yy][xx*4+b] = (UINT8) ss;
                    }
                }
                break;
            case IMAGING_TYPE_INT32:
                /* 32-bit integer */
                for (yy = 0; yy < imOut->ysize; yy++) {
                    ss = 0.0;
                    for (x = (int) xmin; x < (int) xmax; x++)
                        ss = ss + IMAGING_PIXEL_I(imIn, x, yy) * k[x - (int) xmin];
                    IMAGING_PIXEL_I(imOut, xx, yy) = (int) ss * ww;
                }
                break;
            case IMAGING_TYPE_FLOAT32:
                /* 32-bit float */
                for (yy = 0; yy < imOut->ysize; yy++) {
                    ss = 0.0;
                    for (x = (int) xmin; x < (int) xmax; x++)
                        ss = ss + IMAGING_PIXEL_F(imIn, x, yy) * k[x - (int) xmin];
                    IMAGING_PIXEL_F(imOut, xx, yy) = ss * ww;
                }
                break;
            default:
                ImagingSectionLeave(&cookie);
                return (Imaging) ImagingError_ModeError();
            }
    }
    ImagingSectionLeave(&cookie);

    free(kk);
    free(xbounds);

    return imOut;
}


Imaging
ImagingStretch(Imaging imOut, Imaging imIn, int filter)
{
    Imaging imTemp1, imTemp2, imTemp3;
    int xsize = imOut->xsize;
    int ysize = imOut->ysize;

    if (strcmp(imIn->mode, "P") == 0 || strcmp(imIn->mode, "1") == 0)
        return (Imaging) ImagingError_ModeError();

    /* two-pass resize */
    imTemp1 = ImagingNew(imIn->mode, xsize, imIn->ysize);
    if ( ! imTemp1)
        return NULL;

    /* first pass */
    if ( ! ImagingStretchHorizaontal(imTemp1, imIn, filter)) {
        ImagingDelete(imTemp1);
        return NULL;
    }

    imTemp2 = ImagingNew(imIn->mode, imIn->ysize, xsize);
    if ( ! imTemp2) {
        ImagingDelete(imTemp1);
        return NULL;
    }

    /* transpose image once */
    if ( ! ImagingTranspose(imTemp2, imTemp1)) {
        ImagingDelete(imTemp1);
        ImagingDelete(imTemp2);
        return NULL;
    }
    ImagingDelete(imTemp1);

    imTemp3 = ImagingNew(imIn->mode, ysize, xsize);
    if ( ! imTemp3) {
        ImagingDelete(imTemp2);
        return NULL;
    }

    /* second pass */
    if ( ! ImagingStretchHorizaontal(imTemp3, imTemp2, filter)) {
        ImagingDelete(imTemp2);
        ImagingDelete(imTemp3);
        return NULL;
    }
    ImagingDelete(imTemp2);

    /* transpose result */
    if ( ! ImagingTranspose(imOut, imTemp3)) {
        ImagingDelete(imTemp3);
        return NULL;
    }
    ImagingDelete(imTemp3);

    return imOut;
}
