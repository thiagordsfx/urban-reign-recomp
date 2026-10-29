#pragma once

class PS2Runtime;

namespace urban::adxoutput
{
    // The CRI ADX decoder (ADXSJD) sizes each decode by the free space of channel 0 only; this limits it to the channel
    // with the least free space, so the right channel never loses samples (docs/PROGRESS.md §120).
    // URBAN_ADX_MIN_FREE=0 turns it off.
    void install();
}
