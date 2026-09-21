#include <jxl/cms.h>
#include <jxl/decode.h>
#include <cstdio>

int main() {
    if (JxlDecoderVersion() != 12000) return 1;
    JxlDecoder* decoder = JxlDecoderCreate(nullptr);
    if (decoder == nullptr) return 2;
    const JxlCmsInterface* cms = JxlGetDefaultCms();
    const bool configured = cms != nullptr &&
        JxlDecoderSetCms(decoder, *cms) == JXL_DEC_SUCCESS &&
        JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS;
    const unsigned char invalid[] = {0, 0, 0, 0};
    const bool input_set = JxlDecoderSetInput(decoder, invalid, sizeof(invalid)) == JXL_DEC_SUCCESS;
    JxlDecoderCloseInput(decoder);
    const bool rejected = JxlDecoderProcessInput(decoder) == JXL_DEC_ERROR;
    JxlDecoderDestroy(decoder);
    if (!configured || !input_set || !rejected) return 3;
    std::puts("libjxl 0.12.0 static decoder/CMS linkage and malformed-input rejection PASS");
    return 0;
}
