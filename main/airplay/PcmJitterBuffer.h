#ifndef __AIRPLAY_PCM_JITTER_BUFFER_H__
#define __AIRPLAY_PCM_JITTER_BUFFER_H__

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "AirPlayTypes.h"

namespace airplay {

class PcmJitterBuffer {
public:
    struct FrameInfo {
        uint32_t timestamp = 0;
        size_t samplesPerChannel = 0;
        uint16_t channels = 0;
    };

    PcmJitterBuffer() = default;
    ~PcmJitterBuffer();

    PcmJitterBuffer(const PcmJitterBuffer&) = delete;
    PcmJitterBuffer& operator=(const PcmJitterBuffer&) = delete;

    bool init(size_t capacity = kPcmJitterSlots);
    void deinit();

    bool push(uint32_t timestamp, const int16_t* data,
              size_t samplesPerChannel, uint16_t channels);
    bool popTo(int16_t* destination, size_t destinationSamples,
               FrameInfo& info, uint32_t timeoutMs);
    bool peekOldest(FrameInfo& info) const;
    // Blocks until a frame is buffered or the timeout expires. Used by the
    // playback loop when the buffer is empty: waiting on the condition
    // variable resumes as soon as a packet arrives and yields the CPU,
    // unlike a tick-granularity sleep.
    void waitForFrame(uint32_t timeoutMs);
    void flush();
    size_t size() const;

private:
    struct Slot {
        uint32_t timestamp = 0;
        uint16_t samples = 0;
        uint8_t channels = 0;
        uint8_t inUse = 0;
        int16_t data[kMaxSamplesPerChannel * 2] = {};
    };

    bool _initialized = false;
    size_t _capacity = 0;
    std::vector<Slot> _slots;
    std::vector<size_t> _free;
    std::deque<size_t> _order;
    mutable std::mutex _mutex;
    std::condition_variable _cv;
};

} // namespace airplay

#endif // __AIRPLAY_PCM_JITTER_BUFFER_H__
