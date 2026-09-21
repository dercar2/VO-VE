#include <libheif/heif.h>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc != 2) {
        return 1;
    }
    if (heif_get_version_number_major() != 1 || heif_get_version_number_minor() != 23 ||
        heif_get_version_number_maintenance() != 1) {
        return 2;
    }

    const heif_error init_result = heif_init(nullptr);
    if (init_result.code != heif_error_Ok) {
        return 3;
    }

    const heif_decoder_descriptor *decoder = nullptr;
    const int all_decoder_count =
        heif_get_decoder_descriptors(heif_compression_undefined, nullptr, 0);
    const int decoder_count = heif_get_decoder_descriptors(heif_compression_HEVC, &decoder, 1);
    const char *decoder_name =
        decoder == nullptr ? nullptr : heif_decoder_descriptor_get_name(decoder);
    const bool has_libde265 = all_decoder_count == 2 && decoder_count == 1 &&
                              decoder_name != nullptr &&
                              std::strstr(decoder_name, "libde265") != nullptr;
    const heif_decoder_descriptor *av1_decoder = nullptr;
    const int av1_decoder_count =
        heif_get_decoder_descriptors(heif_compression_AV1, &av1_decoder, 1);
    const char *av1_name =
        av1_decoder == nullptr ? nullptr : heif_decoder_descriptor_get_name(av1_decoder);
    const bool has_dav1d = av1_decoder_count == 1 && av1_name != nullptr &&
                           std::strstr(av1_name, "dav1d") != nullptr;

    const int encoder_count =
        heif_get_encoder_descriptors(heif_compression_undefined, nullptr, nullptr, 0);
    if (!has_libde265 || !has_dav1d || encoder_count != 0) {
        std::fprintf(stderr,
                     "Unexpected codec registry: all_decoders=%d hevc_decoders=%d encoders=%d "
                     "hevc_name=%s av1_decoders=%d av1_name=%s\n",
                     all_decoder_count, decoder_count, encoder_count,
                     decoder_name == nullptr ? "" : decoder_name, av1_decoder_count,
                     av1_name == nullptr ? "" : av1_name);
        heif_deinit();
        return 4;
    }

    heif_context *context = heif_context_alloc();
    if (context == nullptr) {
        heif_deinit();
        return 5;
    }

    heif_error result = heif_context_read_from_file(context, argv[1], nullptr);
    const char *stage = "read";
    heif_image_handle *handle = nullptr;
    if (result.code == heif_error_Ok) {
        stage = "primary-image";
        result = heif_context_get_primary_image_handle(context, &handle);
    }

    heif_image *image = nullptr;
    if (result.code == heif_error_Ok) {
        stage = "decode";
        result = heif_decode_image(handle, &image, heif_colorspace_RGB,
                                   heif_chroma_interleaved_RGBA, nullptr);
    }

    const bool decoded = result.code == heif_error_Ok && image != nullptr &&
                         heif_image_get_width(image, heif_channel_interleaved) > 0 &&
                         heif_image_get_height(image, heif_channel_interleaved) > 0;

    if (!decoded) {
        std::fprintf(stderr, "HEIC smoke %s failed: code=%d subcode=%d message=%s\n", stage,
                     static_cast<int>(result.code), static_cast<int>(result.subcode),
                     result.message == nullptr ? "" : result.message);
    } else {
        std::printf("HEVC RGBA decode PASS: %dx%d; decoders=libde265,dav1d encoders=0\n",
                    heif_image_get_width(image, heif_channel_interleaved),
                    heif_image_get_height(image, heif_channel_interleaved));
    }
    if (image != nullptr) {
        heif_image_release(image);
    }
    if (handle != nullptr) {
        heif_image_handle_release(handle);
    }
    heif_context_free(context);
    heif_deinit();
    return decoded ? 0 : 6;
}
