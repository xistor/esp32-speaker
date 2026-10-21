#include "PcmJitterBuffer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace airplay {

namespace {
constexpr const char* TAG = "AirPlayJitter";
}

PcmJitterBuffer::~PcmJitterBuffer()
{
    deinit();
}

bool PcmJitterBuffer::init(size_t capacity)
{
    if (_initialized) {
        return true;
    }
    if (capacity == 0) {
        return false;
    }

    _capacity = capacity;
    try {
        _slots.clear();
        _slots.resize(capacity);
        _free.clear();
        _free.reserve(capacity);
    } catch (...) {
        _slots.clear();
        _free.clear();
        _capacity = 0;
        return false;
    }

    for (size_t i = capacity; i > 0; --i) {
        _free.push_back(i - 1);
    }
    _order.clear();
    _initialized = true;

    ESP_LOGI(TAG, "Jitter buffer: %u slots x %u bytes = %u KB", 
             static_cast<unsigned>(capacity),
             static_cast<unsigned>(sizeof(Slot)),
             static_cast<unsigned>((capacity * sizeof(Slot)) / 1024));
    return true;
}

void PcmJitterBuffer::deinit()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _order.clear();
    _free.clear();
    _slots.clear();
    _capacity = 0;
    _initialized = false;
}

bool PcmJitterBuffer::push(uint32_t timestamp, const int16_t* data,
                           size_t samplesPerChannel, uint16_t channels)
{
    if (data == nullptr || samplesPerChannel == 0 || channels == 0 ||
        channels > 2 || samplesPerChannel > kMaxSamplesPerChannel) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) {
        return false;
    }

    auto it = std::lower_bound(
        _order.begin(), _order.end(), timestamp,
        [this](size_t index, uint32_t value) {
            return _slots[index].timestamp < value;
        });

    if (it != _order.end() && _slots[*it].timestamp == timestamp) {
        // Duplicate RTP packet.
        return false;
    }

    if (_free.empty()) {
        // The buffer is full. Drop the oldest frame so a stalled consumer
        // does not permanently block new audio.
        size_t oldest = _order.front();
        _order.pop_front();
        _slots[oldest].inUse = 0;
        _free.push_back(oldest);
    }

    size_t index = _free.back();
    _free.pop_back();

    Slot& slot = _slots[index];
    slot.timestamp = timestamp;
    slot.samples = static_cast<uint16_t>(samplesPerChannel);
    slot.channels = static_cast<uint8_t>(channels);
    slot.inUse = 1;
    std::memcpy(slot.data, data,
                samplesPerChannel * channels * sizeof(int16_t));

    _order.insert(it, index);
    _cv.notify_one();
    return true;
}

bool PcmJitterBuffer::popTo(int16_t* destination, size_t destinationSamples,
                            FrameInfo& info, uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_initialized) {
        return false;
    }

    if (_order.empty()) {
        if (timeoutMs == 0) {
            _cv.wait(lock, [this] { return !_order.empty(); });
        } else {
            if (!_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return !_order.empty(); })) {
                return false;
            }
        }
    }

    size_t index = _order.front();
    _order.pop_front();

    Slot& slot = _slots[index];
    size_t samples = static_cast<size_t>(slot.samples) * slot.channels;
    if (samples > destinationSamples) {
        samples = destinationSamples;
    }

    if (destination != nullptr && samples > 0) {
        std::memcpy(destination, slot.data, samples * sizeof(int16_t));
    }

    info.timestamp = slot.timestamp;
    info.samplesPerChannel = slot.samples;
    info.channels = slot.channels;

    slot.inUse = 0;
    _free.push_back(index);
    return true;
}

bool PcmJitterBuffer::peekOldest(FrameInfo& info) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_order.empty()) {
        return false;
    }
    const Slot& slot = _slots[_order.front()];
    info.timestamp = slot.timestamp;
    info.samplesPerChannel = slot.samples;
    info.channels = slot.channels;
    return true;
}

void PcmJitterBuffer::waitForFrame(uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_initialized || !_order.empty()) {
        return;
    }
    _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                 [this] { return !_order.empty(); });
}

void PcmJitterBuffer::flush()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _order.clear();
    _free.clear();
    _free.reserve(_capacity);
    for (size_t i = _capacity; i > 0; --i) {
        _slots[i - 1].inUse = 0;
        _free.push_back(i - 1);
    }
}

size_t PcmJitterBuffer::size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _order.size();
}

} // namespace airplay
