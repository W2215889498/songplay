#include "wels_dec.h"

#include <stdlib.h>
#include <string.h>

#include "codec_api.h"

struct wels_dec {
    ISVCDecoder *dec;
    SBufferInfo info;
    unsigned char *planes[3];
    int width;
    int height;
    int stride[2];
};

wels_dec_t *wels_dec_create(void)
{
    ISVCDecoder *dec = NULL;
    if (WelsCreateDecoder(&dec) != 0 || dec == NULL) {
        return NULL;
    }
    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.uiTargetDqLayer = (unsigned char)-1;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.sVideoProperty.size = sizeof(param.sVideoProperty);
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    if (dec->Initialize(&param) != 0) {
        WelsDestroyDecoder(dec);
        return NULL;
    }
    wels_dec_t *d = (wels_dec_t *)calloc(1, sizeof(*d));
    if (d == NULL) {
        dec->Uninitialize();
        WelsDestroyDecoder(dec);
        return NULL;
    }
    d->dec = dec;
    return d;
}

void wels_dec_destroy(wels_dec_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->dec != NULL) {
        d->dec->Uninitialize();
        WelsDestroyDecoder(d->dec);
    }
    free(d);
}

int wels_dec_decode(wels_dec_t *d, const uint8_t *data, size_t len)
{
    if (d == NULL || d->dec == NULL) {
        return -1;
    }
    memset(&d->info, 0, sizeof(d->info));
    d->planes[0] = d->planes[1] = d->planes[2] = NULL;
    DECODING_STATE st = d->dec->DecodeFrameNoDelay(data, (int)len, d->planes, &d->info);
    if (st != dsErrorFree && d->info.iBufferStatus != 1) {
        /* Recoverable mid-stream errors are common right after join; drop the
         * unit and keep going. */
        return 0;
    }
    if (d->info.iBufferStatus == 1) {
        d->width = d->info.UsrData.sSystemBuffer.iWidth;
        d->height = d->info.UsrData.sSystemBuffer.iHeight;
        d->stride[0] = d->info.UsrData.sSystemBuffer.iStride[0];
        d->stride[1] = d->info.UsrData.sSystemBuffer.iStride[1];
        return 1;
    }
    return 0;
}

int wels_dec_width(const wels_dec_t *d)
{
    return d != NULL ? d->width : 0;
}

int wels_dec_height(const wels_dec_t *d)
{
    return d != NULL ? d->height : 0;
}

int wels_dec_stride(const wels_dec_t *d, int plane)
{
    if (d == NULL || plane < 0 || plane > 2) {
        return 0;
    }
    return plane == 0 ? d->stride[0] : d->stride[1];
}

const uint8_t *wels_dec_plane(const wels_dec_t *d, int plane)
{
    if (d == NULL || plane < 0 || plane > 2) {
        return NULL;
    }
    return d->planes[plane];
}
