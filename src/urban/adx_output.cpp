#include "urban/adx_output.h"
#include "urban/function_hooks.h"
#include "urban/log.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace urban::adxoutput
{
    namespace
    {
        // ADXSJD (the SJ front end of the CRI ADX decoder), object in $s2:
        //   +0x02 channel count (byte), +0x0C/+0x10 output SJ of each channel,
        //   +0x1C + 8 * ch the free chunk {data, length} taken from that SJ for this decode,
        //   +0x38 the most samples per decode (set by the game, read only by the callback below).
        // sub_002E1038 (the decoder's "output buffer" callback) takes a free chunk from every channel, then calls
        // sub_002F8C80 and returns min(length of channel 0 / 2, +0x38) samples. sub_002E14C0 (the "decoded" callback)
        // splits every channel's chunk at that size: a channel with a shorter chunk only keeps its length, and the
        // samples decoded past it are lost (they land on data that is still waiting to go to the IOP).
        // On a PS2 the PSM sends both channels to the IOP in step, so their free space is the same. Here the IOP ring
        // of one channel can wrap at another point and its transfer falls 0x400 bytes behind: each lap of the ring the
        // right channel lost 0x400 bytes, until the movie's speech came out of the right speaker 171 ms before the
        // left one (the whole 0x4000-byte ring), with a click at every lap.
        constexpr uint32_t kGetBufferPtr = 0x2f8c80u;    // called by the callback once the chunks are taken
        constexpr uint32_t kAfterChunksTaken = 0x2e10ccu; // its return address in sub_002E1038
        constexpr uint32_t kDecodedCallback = 0x2e14c0u;
        constexpr uint32_t kChannels = 0x02u;
        constexpr uint32_t kChunks = 0x1cu;
        constexpr uint32_t kMaxSamples = 0x38u;

        struct Saved
        {
            int32_t original = 0;
            int32_t written = 0;
        };
        std::unordered_map<uint32_t, Saved> g_saved; // by ADXSJD object; EE thread only

        int32_t read32(const uint8_t *rdram, uint32_t address)
        {
            int32_t value;
            std::memcpy(&value, rdram + (address & 0x01FFFFFFu), sizeof(value));
            return value;
        }

        void write32(uint8_t *rdram, uint32_t address, int32_t value)
        {
            std::memcpy(rdram + (address & 0x01FFFFFFu), &value, sizeof(value));
        }

        // The most samples this decode as the game set it (what is in memory may still be our limit from a decode that
        // never reached the "decoded" callback).
        int32_t gameMaxSamples(const uint8_t *rdram, uint32_t object)
        {
            const int32_t current = read32(rdram, object + kMaxSamples);
            const auto it = g_saved.find(object);
            return it != g_saved.end() && it->second.written == current ? it->second.original : current;
        }

        void restore(uint8_t *rdram, uint32_t object)
        {
            const auto it = g_saved.find(object);
            if (it == g_saved.end())
                return;
            if (read32(rdram, object + kMaxSamples) == it->second.written)
                write32(rdram, object + kMaxSamples, it->second.original);
            g_saved.erase(it);
        }
    }

    void install()
    {
        if (const char *v = std::getenv("URBAN_ADX_MIN_FREE"); v && *v == '0')
            return;
        const bool limit = FunctionHooks::instance().observeEntry(kGetBufferPtr, "adxsjd_min_free", [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *) {
            if (getRegU32(ctx, 31) != kAfterChunksTaken)
                return;
            const uint32_t object = getRegU32(ctx, 18);
            const uint32_t channels = std::min<uint32_t>(rdram[(object + kChannels) & 0x01FFFFFFu], 2u);
            if (channels < 2u)
                return;
            int32_t shortest = read32(rdram, object + kChunks + 4u);
            for (uint32_t ch = 1; ch < channels; ++ch)
                shortest = std::min(shortest, read32(rdram, object + kChunks + 8u * ch + 4u));
            const int32_t original = gameMaxSamples(rdram, object);
            const int32_t samples = std::min(original, std::max(shortest, 0) / 2);
            g_saved.erase(object);
            write32(rdram, object + kMaxSamples, samples);
            if (samples != original)
                g_saved[object] = Saved{original, samples};
        });
        const bool undo = FunctionHooks::instance().observeEntry(kDecodedCallback, "adxsjd_min_free_restore", [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *) {
            if (!g_saved.empty())
                restore(rdram, getRegU32(ctx, 4));
        });
        if (!limit || !undo)
            URBAN_ERROR(Hook, "ADX output limit not installed (sub_002F8C80 or sub_002E14C0 missing)");
    }
}
