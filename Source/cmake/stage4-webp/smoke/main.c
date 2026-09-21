#include <webp/decode.h>
#include <webp/demux.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { kExpectedVersion = (1 << 16) | (6 << 8), kMaximumSampleBytes = 16 << 20 };

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL)
            fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    if (length <= 0 || length > kMaximumSampleBytes || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    if (WebPGetDecoderVersion() != kExpectedVersion || WebPGetDemuxVersion() != kExpectedVersion) {
        return 2;
    }

    size_t size = 0;
    uint8_t *bytes = read_file(argv[1], &size);
    if (bytes == NULL)
        return 3;

    WebPData data;
    WebPDataInit(&data);
    data.bytes = bytes;
    data.size = size;
    WebPDemuxState state = WEBP_DEMUX_PARSE_ERROR;
    WebPDemuxer *demux = WebPDemuxPartial(&data, &state);
    if (demux == NULL || state != WEBP_DEMUX_DONE ||
        WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH) == 0 ||
        WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT) == 0 ||
        WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT) == 0) {
        WebPDemuxDelete(demux);
        free(bytes);
        return 4;
    }

    WebPIterator frame;
    if (!WebPDemuxGetFrame(demux, 1, &frame)) {
        WebPDemuxDelete(demux);
        free(bytes);
        return 5;
    }

    int width = 0;
    int height = 0;
    const int has_header = WebPGetInfo(frame.fragment.bytes, frame.fragment.size, &width, &height);
    uint8_t *rgba = has_header
                        ? WebPDecodeRGBA(frame.fragment.bytes, frame.fragment.size, &width, &height)
                        : NULL;
    const int decoded = rgba != NULL && width > 0 && height > 0;
    WebPFree(rgba);
    WebPDemuxReleaseIterator(&frame);
    WebPDemuxDelete(demux);
    free(bytes);
    return decoded ? 0 : 6;
}
