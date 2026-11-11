using Ryujinx.Horizon.Common;

namespace Ryujinx.Horizon.Sdk.Codec
{
    static class CodecResult
    {
        private const int ModuleId = 111;

        public static Result InvalidLength => new(ModuleId, 3);
        public static Result OpusBadArg => new(ModuleId, 130);
        public static Result OpusInvalidPacket => new(ModuleId, 133);
        // S2Arm：同时存活的解码器到上限。真机 SDK nn::codec::InitializeHardwareOpusDecoder 对这个结果（0x3026F）专门转成返回值 8（资源不足）交给游戏、不中止
        public static Result OutOfDecoders => new(ModuleId, 385);
        public static Result InvalidNumberOfStreams => new(ModuleId, 1000);
        public static Result InvalidSampleRate => new(ModuleId, 1001);
        public static Result InvalidChannelCount => new(ModuleId, 1002);
    }
}
